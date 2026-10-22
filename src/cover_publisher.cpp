//  Copyright (c) 2024 Exle
//
//  Permission is hereby granted, free of charge, to any person obtaining a
//  copy of this software and associated documentation files (the "Software"),
//  to deal in the Software without restriction, including without limitation
//  the rights to use, copy, modify, merge, publish, distribute, sublicense,
//  and/or sell copies of the Software, and to permit persons to whom the
//  Software is furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
//  OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
//  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//  DEALINGS IN THE SOFTWARE.

#include "cover_publisher.h"

// winsock2.h must precede windows.h, which it would otherwise clash with.
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <wincodec.h>
#include <winhttp.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <map>
#include <mutex>
#include <random>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "utils.h"

// WIC and the COM stream plumbing it needs; declared here so the plugin and the
// offline test binary link the same way without extra project wiring. ws2_32 is
// the OAuth loopback listener; shell32 is ShellExecuteA, which opens the
// consent page, so the standalone test build links it like the plugin does.
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

using CoverPublisher::Result;

// Defined with the logger sink further down; the Drive consent path logs before
// that definition, so the declaration comes first.
void EmitLog(const std::string& line);

// --- Transport constants ----------------------------------------------------

// Per-HTTP-operation budget. Each upload attempt and each health check gets
// this much time; a transport failure is retried once after a short pause, so
// an upload is bounded by two attempts plus the backoff.
//
// Sized against live measurement, not guessed: a ~30-300 KB cover uploads to
// uguu in 0.8-4.1 s and a blocked host answers 403 immediately. The budget only
// has to catch a HANG, and a hang is better answered by moving to the next host
// than by waiting - the previous 20 s per host made a stalled host cost the
// owner ~40 s of black card before the chain gave up.
constexpr DWORD kTotalTimeoutMs = 6000;
constexpr DWORD kRetryBackoffMs = 500;
constexpr DWORD kMaxResponseBytes = 64 * 1024;
constexpr size_t kMaxCacheBytes = 1024 * 1024;
constexpr long long kNegativeCacheTtlSeconds = 60;
// 29 bytes keep the closing delimiter ("\r\n--<boundary>--\r\n") at 37 bytes,
// matching the live measurement of the host's request framing.
constexpr size_t kBoundaryLength = 29;

// --- Google Drive host constants --------------------------------------------
//
// The consent wait is the one long wait in the module. It is interactive, so it
// cannot share the 6 s transport budget, but it runs on the cover worker only:
// AIMP's message thread is never held, and CancelDriveConsent() lets an unload
// end the wait immediately instead of joining a sleeping thread.
constexpr DWORD kDriveConsentWindowMs = 120 * 1000;
constexpr DWORD kDriveConsentPollMs = 250;
// Per-receive budget on an accepted loopback socket. A client that connects and
// sends nothing costs at most this much, and the consent deadline is checked
// again before the next receive, so the wait can never outlive the window.
constexpr DWORD kDriveReceiveTimeoutMs = 2000;
// After an abandoned or declined consent, do not open a browser again for this
// long. Without it every new track would pop a browser, because the negative
// cache is keyed per image.
constexpr DWORD kDriveConsentCooldownMs = 10 * 60 * 1000;
// An access token is refreshed this many seconds before its stated expiry.
constexpr long long kDriveAccessTokenSkewSeconds = 60;
constexpr size_t kMaxDriveResponseBytes = 64 * 1024;
constexpr size_t kMaxLoopbackRequestBytes = 8 * 1024;
constexpr char kDriveFolderName[] = "aimp-covers";
constexpr char kDriveAuthEndpoint[] = "https://accounts.google.com/o/oauth2/v2/auth";
constexpr char kDriveScope[] = "https://www.googleapis.com/auth/drive.file";
// Discord renders this lh3 short link as presence art (verified live); the id
// is the Drive file id returned by the upload.
constexpr char kDriveLh3UrlPrefix[] = "https://lh3.googleusercontent.com/d/";

// Cover transfer shaping: the image is centre-cropped to a square (the shorter
// side) and then scaled down to at most 512 px - Discord re-crops every large
// image to a centre square anyway, so cropping here keeps the payload small and
// correctly framed. The result is encoded as JPEG q85, unless the source is
// already both at or below 512 px and at most 256 KB, in which case it goes up
// untouched (the existing skip rule).
constexpr UINT kMaxImageDimension = 512;
constexpr size_t kSkipReencodeMaxBytes = 256 * 1024;
constexpr float kJpegQuality = 0.85f;

// The GitHub contents response carries a base64 `content` object and can run to
// a few hundred KB; the keyless hosts answer in bytes, so the larger limit
// applies to the GitHub request only.
constexpr size_t kMaxGitHubResponseBytes = 1024 * 1024;

constexpr wchar_t kAgentName[] = L"AIMP-Discord-Presence/2.0";

// Upload hosts are tried in order until one returns a usable URL, and the chain
// is policy-driven (CoverHost): "auto" - the default - leads with Drive when
// its credentials exist, otherwise GitHub when a token and repo exist, and
// otherwise starts keyless; "drive" and "github" pin that authenticated host.
// The keyless hosts are appended in every case, in this order: catbox.moe,
// then uguu.se, then the dormant litter.catbox.moe - last, because it answers
// HTTP 403 to a BunkerWeb WAF here, so it is never reached when an earlier host
// succeeds, though it may still serve other networks. A pinned host that is
// not configured, or one that fails, costs its round trip only and leaves the
// keyless fallbacks; it never skips a layer of the cover chain. Each host owns
// both its endpoint and the shape of its request body.
// A cached keyless URL is ephemeral: a hit on one is revalidated, or upgraded
// to a permanent host when the policy names one. A permanent URL (Drive,
// GitHub) is reused as-is.
enum class UploadShape {
  // GitHub: PUT /repos/{owner}/{repo}/contents/covers/{name}.jpg with a JSON
  // body (base64 image). The response is JSON; the URL is content.download_url.
  kGitHub,
  // catbox: POST /user/api.php with reqtype=fileupload and the image in
  // `fileToUpload`. The response body is the bare URL as text.
  kCatbox,
  // uguu: POST /upload?output=text with no extra fields and the image in
  // `files[]`. The response body is the bare URL as text.
  kUguu,
  // litterbox: POST /resources/internals/api.php with reqtype=fileupload,
  // time=72h and the image in `fileToUpload`.
  kLitterbox,
  // Drive: POST /upload/drive/v3/files with a multipart/related body (a JSON
  // metadata part, then the bytes) and a bearer token. The response is JSON;
  // the URL is built from the file id.
  kDrive,
};

struct UploadHost {
  std::wstring name;   // log marker, e.g. "litter.catbox.moe"
  std::wstring host;   // for WinHttpConnect
  std::wstring path;   // request target, query string included
  UploadShape shape;
};

// The log marker as UTF-8, so it can be concatenated into a std::string line.
std::string HostNameUtf8(const UploadHost& upload_host) {
  return Utils::ToString(upload_host.name);
}

// The keyless hosts, always present and always in this order: catbox, then
// uguu, then the dormant litterbox. The GitHub host, when configured, is
// prepended to these by ActiveHosts().
const size_t kBaseUploadHostCount = 3;
const UploadHost kBaseUploadHosts[kBaseUploadHostCount] = {
    {L"catbox.moe", L"catbox.moe", L"/user/api.php", UploadShape::kCatbox},
    {L"uguu.se", L"uguu.se", L"/upload?output=text", UploadShape::kUguu},
    {L"litter.catbox.moe", L"litterbox.catbox.moe", L"/resources/internals/api.php", UploadShape::kLitterbox},
};

// The default GitHub repository: the owner's permanent art repo. CoverRepo
// overrides it; CoverToken (empty by default) enables the host at all.
const wchar_t kDefaultGitHubRepo[] = L"nathwn12/aimp-discord-presence-art";

// The GitHub host configuration. `token` is only ever sent to api.github.com;
// it is never logged, cached, or put into a Result. Guarded by g_host_mutex
// because ConfigureAuth runs before the publisher worker starts, but the worker
// reads it for each publish.
std::mutex g_host_mutex;
std::string g_github_token;
std::wstring g_github_owner;
std::wstring g_github_repo;

// The Drive host configuration. `client_secret` is only ever sent to
// oauth2.googleapis.com, in the token request body; it is never logged,
// cached, or put into a Result. `state_path` is the sidecar that holds the
// refresh token and the bootstrap folder id.
std::string g_drive_client_id;
std::string g_drive_client_secret;
std::string g_drive_state_path;

// The preferred host, from CoverHost. "auto" is the default; the keyless hosts
// are always the last resort, whatever the preference.
enum class CoverHostPolicy { kAuto, kDrive, kGithub, kKeyless };

CoverHostPolicy g_cover_host = CoverHostPolicy::kAuto;

// Splits "owner/name" into its two parts. A malformed value yields false, which
// skips the GitHub host rather than building a nonsense request target.
bool ParseOwnerRepo(const std::wstring& repo, std::wstring* owner, std::wstring* name) {
  const size_t slash = repo.find(L'/');
  if (slash == std::wstring::npos || slash == 0 || slash + 1 >= repo.size()) {
    return false;
  }
  *owner = repo.substr(0, slash);
  *name = repo.substr(slash + 1);
  return name->find(L'/') == std::wstring::npos;
}

// "auto", "drive", "github" or "keyless", case-insensitive and trimmed; every
// other value - including empty - is "auto", so a typo in AIMP.ini can never
// leave the chain empty.
CoverHostPolicy ParseCoverHostPolicy(const std::string& value) {
  static const char kWhitespace[] = " \t\r\n";
  size_t begin = 0;
  size_t end = value.size();
  while (begin < end && std::strchr(kWhitespace, value[begin]) != nullptr) {
    ++begin;
  }
  while (end > begin && std::strchr(kWhitespace, value[end - 1]) != nullptr) {
    --end;
  }
  std::string lower;
  lower.reserve(end - begin);
  for (size_t i = begin; i < end; ++i) {
    const char c = value[i];
    lower.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
  }
  if (lower == "drive") {
    return CoverHostPolicy::kDrive;
  }
  if (lower == "github") {
    return CoverHostPolicy::kGithub;
  }
  if (lower == "keyless") {
    return CoverHostPolicy::kKeyless;
  }
  return CoverHostPolicy::kAuto;
}

// Which of the two authenticated hosts a publish leads with. Pure, so the
// selection matrix is testable without credentials or a network:
//   auto     drive when available, otherwise github when available
//   drive    drive when available
//   github   github when available
//   keyless  neither - the anonymous hosts only
// The keyless hosts are appended by ActiveHosts() in every case, so a failed
// preferred host still falls through exactly as the chain always did.
struct HostChainPlan {
  bool drive = false;
  bool github = false;
};

HostChainPlan PlanHostChain(CoverHostPolicy policy, bool drive_available, bool github_available) {
  HostChainPlan plan;
  switch (policy) {
    case CoverHostPolicy::kDrive:
      plan.drive = drive_available;
      break;
    case CoverHostPolicy::kGithub:
      plan.github = github_available;
      break;
    case CoverHostPolicy::kKeyless:
      break;
    case CoverHostPolicy::kAuto:
    default:
      plan.drive = drive_available;
      plan.github = !drive_available && github_available;
      break;
  }
  return plan;
}

// The permanent hosts a cache upgrade may re-publish to, in the order the chain
// tries them. Derived from the same HostChainPlan as a fresh upload, so the
// cache path can never bypass the configured policy: with CoverHost=keyless the
// list is empty and no authenticated host is ever touched, and with auto + Drive
// credentials Drive comes first. Keyless hosts are never upgrade targets - an
// ephemeral URL is exactly what an upgrade exists to replace.
enum class PermanentHost { kDrive, kGitHub };

std::vector<PermanentHost> PlanCacheUpgrade(CoverHostPolicy policy, bool drive_available,
                                           bool github_available) {
  const HostChainPlan plan = PlanHostChain(policy, drive_available, github_available);
  std::vector<PermanentHost> hosts;
  if (plan.drive) {
    hosts.push_back(PermanentHost::kDrive);
  }
  if (plan.github) {
    hosts.push_back(PermanentHost::kGitHub);
  }
  return hosts;
}

// The policy's own name, for the startup line and the cache logs.
std::string PolicyName(CoverHostPolicy policy) {
  switch (policy) {
    case CoverHostPolicy::kDrive:
      return "drive";
    case CoverHostPolicy::kGithub:
      return "github";
    case CoverHostPolicy::kKeyless:
      return "keyless";
    case CoverHostPolicy::kAuto:
    default:
      return "auto";
  }
}

// Why a host cannot be used, as a fixed token built from booleans only. No
// configured value - not even a prefix or a length - can reach the log through
// this path.
std::string DriveUnavailableReason(bool client_id, bool client_secret, bool state_path) {
  if (!client_id && !client_secret) {
    return "no-credentials";
  }
  if (!client_id) {
    return "no-client-id";
  }
  if (!client_secret) {
    return "no-client-secret";
  }
  if (!state_path) {
    return "no-state-path";
  }
  return std::string();
}

std::string GitHubUnavailableReason(bool token, bool repo) {
  if (!token) {
    return "no-token";
  }
  if (!repo) {
    return "bad-repo";
  }
  return std::string();
}

// Everything the startup line needs, all of it booleans and fixed tokens.
struct HostConfigSummary {
  CoverHostPolicy policy = CoverHostPolicy::kAuto;
  bool drive_usable = false;
  bool github_usable = false;
  std::string drive_reason;   // why Drive is unusable; empty when usable
  std::string github_reason;  // why GitHub is unusable; empty when usable
};

// One line naming the policy and each host's participation: "on" means the host
// is in the chain for this load, with the reason when it is not. Pure, so the
// diagnostic line itself is pinned by a test.
std::string FormatHostConfigLine(const HostConfigSummary& summary) {
  const HostChainPlan plan =
      PlanHostChain(summary.policy, summary.drive_usable, summary.github_usable);
  std::string line = std::string("local-cover hosts policy=") + PolicyName(summary.policy);
  line += std::string(" drive=") + (plan.drive ? "on" : "off");
  if (!plan.drive) {
    const std::string drive_reason =
        summary.drive_usable ? std::string("policy") : summary.drive_reason;
    if (!drive_reason.empty()) {
      line += " drive-reason=" + drive_reason;
    }
  }
  line += std::string(" github=") + (plan.github ? "on" : "off");
  if (!plan.github) {
    const std::string github_reason =
        summary.github_usable ? std::string("policy") : summary.github_reason;
    if (!github_reason.empty()) {
      line += " github-reason=" + github_reason;
    }
  }
  line += " keyless=on";
  return line;
}

// A Drive host needs both OAuth values and a writable sidecar path: without the
// sidecar the refresh token could not survive a restart, so the host is off.
bool DriveConfiguredLocked() {
  return !g_drive_client_id.empty() && !g_drive_client_secret.empty() && !g_drive_state_path.empty();
}

bool GitHubConfiguredLocked() {
  return !g_github_token.empty() && !g_github_owner.empty() && !g_github_repo.empty();
}

// Every host to try, in order: the preferred authenticated host (Drive and/or
// GitHub per the policy), then the keyless hosts, unchanged.
std::vector<UploadHost> ActiveHosts() {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  const HostChainPlan plan =
      PlanHostChain(g_cover_host, DriveConfiguredLocked(), GitHubConfiguredLocked());
  std::vector<UploadHost> hosts;
  if (plan.drive) {
    UploadHost drive;
    drive.name = L"drive.google.com";
    drive.host = L"www.googleapis.com";
    drive.path = L"/upload/drive/v3/files?uploadType=multipart&fields=id";
    drive.shape = UploadShape::kDrive;
    hosts.push_back(drive);
  }
  if (plan.github) {
    UploadHost github;
    github.name = L"github.com";
    github.host = L"api.github.com";
    github.path = L"/repos/" + g_github_owner + L"/" + g_github_repo + L"/contents/covers/";
    github.shape = UploadShape::kGitHub;
    hosts.push_back(github);
  }
  for (size_t i = 0; i < kBaseUploadHostCount; ++i) {
    hosts.push_back(kBaseUploadHosts[i]);
  }
  return hosts;
}

bool GitHubTokenPresent() {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  return !g_github_token.empty();
}

// The GitHub host is usable only when a token AND a parseable repo are
// configured - the same condition ActiveHosts() uses to prepend it. A token
// with a malformed repo is present-but-unused, exactly like no token.
bool GitHubUploadActive() {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  return !g_github_token.empty() && !g_github_owner.empty() && !g_github_repo.empty();
}

// The Drive host is active only when the policy selects it AND its credentials
// and sidecar are configured - the same condition ActiveHosts() uses.
bool DriveUploadActive() {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  return PlanHostChain(g_cover_host, DriveConfiguredLocked(), GitHubConfiguredLocked()).drive;
}

// Each host owns the exact URL shape it answers with, so a lookalike host
// ("...moe.evil.test") or a body that is not that host's URL is rejected. The
// success response is a bare https URL matching the host that answered; a WAF
// or JSON error page, or HTTP 200 with an empty body, is a failure.
// uguu round-robins across single-label subdomains (n., h., ...), so the
// subdomain is matched generally rather than pinned to one letter - pinning it
// rejected real responses as "unexpected body".
constexpr char kLitterboxUrlPattern[] = R"(^https://litter\.catbox\.moe/[a-z0-9]{6}\.(png|jpg|jpeg)$)";
constexpr char kUguuUrlPattern[] = R"(^https://([A-Za-z0-9-]+\.)?uguu\.se/[A-Za-z0-9]+\.(png|jpg|jpeg|gif|webp|bmp)$)";
constexpr char kCatboxUrlPattern[] = R"(^https://files\.catbox\.moe/[A-Za-z0-9]+\.(png|jpg|jpeg|gif|webp|bmp)$)";
// A GitHub raw cover URL, generic over owner/repo so the cache can validate a
// stored URL; the per-request classifier pins the exact owner and repo.
constexpr char kGitHubRawUrlPattern[] =
    R"(^https://raw\.githubusercontent\.com/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+/main/covers/[0-9a-f]{64}\.jpg$)";
// A Drive cover URL: the lh3 short link Discord renders. Pinned to the host, so
// a lookalike can never be adopted as a cover.
constexpr char kDriveLh3UrlPattern[] =
    R"(^https://lh3\.googleusercontent\.com/d/[A-Za-z0-9_-]{10,}$)";
// Fallback when no host is specified (kept for callers that classify a bare
// response): the strict litterbox shape.
std::string UploadUrlPatternFor(const UploadHost& upload_host) {
  switch (upload_host.shape) {
    case UploadShape::kCatbox:
      return std::string(kCatboxUrlPattern);
    case UploadShape::kUguu:
      return std::string(kUguuUrlPattern);
    case UploadShape::kGitHub:
      return std::string(kGitHubRawUrlPattern);
    case UploadShape::kDrive:
      return std::string(kDriveLh3UrlPattern);
    case UploadShape::kLitterbox:
    default:
      return std::string(kLitterboxUrlPattern);
  }
}
constexpr char kMimePattern[] = R"(^[A-Za-z0-9][A-Za-z0-9.+-]*/[A-Za-z0-9][A-Za-z0-9.+-]*$)";

Result Failure(const std::string& reason) {
  Result result;
  result.reason = reason;
  return result;
}

std::string TrimWhitespace(const std::string& text) {
  const char* kWhitespace = " \t\r\n";
  const size_t begin = text.find_first_not_of(kWhitespace);
  if (begin == std::string::npos) {
    return std::string();
  }
  const size_t end = text.find_last_not_of(kWhitespace);
  return text.substr(begin, end - begin + 1);
}

// A URL is accepted only when it matches one configured host's exact shape. The
// strictness per host is what keeps a lookalike host, an HTML page or a JSON
// blob from ever being adopted as a cover.
bool LooksLikeUploadUrl(const std::string& text) {
  static const std::regex litterbox_pattern(kLitterboxUrlPattern);
  static const std::regex uguu_pattern(kUguuUrlPattern);
  static const std::regex catbox_pattern(kCatboxUrlPattern);
  static const std::regex github_pattern(kGitHubRawUrlPattern);
  static const std::regex drive_pattern(kDriveLh3UrlPattern);
  return std::regex_match(text, litterbox_pattern) || std::regex_match(text, uguu_pattern) ||
         std::regex_match(text, catbox_pattern) || std::regex_match(text, github_pattern) ||
         std::regex_match(text, drive_pattern);
}

// The per-host predicate used by the classifier: a 200 is a success only when
// the body is the bare URL of the host that answered. GitHub never uses this
// form (its response is JSON, validated with its own owner/repo pin), so its
// pattern here is the generic raw-URL shape.
bool LooksLikeHostUrl(const std::string& text, const UploadHost& upload_host) {
  const std::string url_pattern = UploadUrlPatternFor(upload_host);
  static const std::map<std::string, std::regex> patterns = {
      {kLitterboxUrlPattern, std::regex(kLitterboxUrlPattern)},
      {kUguuUrlPattern, std::regex(kUguuUrlPattern)},
      {kCatboxUrlPattern, std::regex(kCatboxUrlPattern)},
      {kGitHubRawUrlPattern, std::regex(kGitHubRawUrlPattern)},
      {kDriveLh3UrlPattern, std::regex(kDriveLh3UrlPattern)},
  };
  const auto found = patterns.find(url_pattern);
  return found != patterns.end() && std::regex_match(text, found->second);
}

// The host that owns a URL, as its log marker, or empty when the shape is not
// one of the known hosts. Pure, so a cache hit can name its host with no
// network and no upload: a stored URL's host is the host that served it.
std::string HostNameForUrl(const std::string& url) {
  if (LooksLikeHostUrl(url, UploadHost{L"catbox.moe", L"", L"", UploadShape::kCatbox})) {
    return "catbox.moe";
  }
  if (LooksLikeHostUrl(url, UploadHost{L"uguu.se", L"", L"", UploadShape::kUguu})) {
    return "uguu.se";
  }
  if (LooksLikeHostUrl(url, UploadHost{L"litter.catbox.moe", L"", L"", UploadShape::kLitterbox})) {
    return "litter.catbox.moe";
  }
  if (LooksLikeHostUrl(url, UploadHost{L"github.com", L"", L"", UploadShape::kGitHub})) {
    return "github.com";
  }
  if (LooksLikeHostUrl(url, UploadHost{L"drive.google.com", L"", L"", UploadShape::kDrive})) {
    return "drive.google.com";
  }
  return std::string();
}

// A cached URL on a keyless host (catbox, uguu, litterbox) is ephemeral: it
// can die while the cache entry lives (measured: a uguu URL answering 404
// months later). A GitHub raw URL is the owner's own permanent repo. The
// cache only ever stores URLs that pass LooksLikeUploadUrl, so an unknown
// shape passes through as durable - it can only be a future host's URL.
bool IsEphemeralCachedUrl(const std::string& url) {
  return LooksLikeHostUrl(url, UploadHost{L"", L"", L"", UploadShape::kCatbox}) ||
         LooksLikeHostUrl(url, UploadHost{L"", L"", L"", UploadShape::kUguu}) ||
         LooksLikeHostUrl(url, UploadHost{L"", L"", L"", UploadShape::kLitterbox});
}

bool IsGitHubCachedUrl(const std::string& url) {
  return LooksLikeHostUrl(url, UploadHost{L"", L"", L"", UploadShape::kGitHub});
}

// What a cache hit needs: use it as-is, re-publish the local bytes to a
// permanent host, or revalidate the stored URL before trusting it.
// Pure, so the policy is testable without a network.
enum class CachedUrlAction {
  kUse,  // durable (GitHub, Drive) or unknown shape: answer the hit
  // Ephemeral URL and the host policy selects a permanent authenticated host:
  // re-publish there. The target is whichever host PlanCacheUpgrade names, in
  // chain order - never the keyless hosts themselves.
  kUpgradeToPermanent,
  // The historical name for the same action, kept so existing callers and
  // tests keep their meaning; the target is no longer necessarily GitHub.
  kUpgradeToGitHub = kUpgradeToPermanent,
  kRevalidate,  // ephemeral URL, no permanent host: health-check, re-publish if dead
};

// A hit on an ephemeral keyless URL is never trusted blindly. `permanent_active`
// is PlanCacheUpgrade(...).empty() negated: with a permanent host the bytes are
// re-published there, without one the stored URL is revalidated.
CachedUrlAction PlanCachedUrl(const std::string& url, bool permanent_active) {
  if (!IsEphemeralCachedUrl(url)) {
    return CachedUrlAction::kUse;
  }
  return permanent_active ? CachedUrlAction::kUpgradeToPermanent : CachedUrlAction::kRevalidate;
}

std::string SanitizeMime(const char* mime) {
  const std::string candidate = mime != nullptr ? std::string(mime) : std::string();
  static const std::regex pattern(kMimePattern);
  if (std::regex_match(candidate, pattern)) {
    return candidate;
  }
  return "application/octet-stream";
}

const char* FilenameForMime(const std::string& mime) {
  if (mime == "image/png") {
    return "cover.png";
  }
  if (mime == "image/jpeg") {
    return "cover.jpg";
  }
  if (mime == "image/gif") {
    return "cover.gif";
  }
  if (mime == "image/webp") {
    return "cover.webp";
  }
  if (mime == "image/bmp") {
    return "cover.bmp";
  }
  return "cover.bin";
}

// --- Multipart body ---------------------------------------------------------

std::string MultipartPrefix(const std::string& boundary, const std::string& mime,
                            UploadShape shape) {
  const std::string delimiter = "--" + boundary + "\r\n";
  std::string prefix;
  if (shape == UploadShape::kLitterbox) {
    // Three parts: reqtype, time, then the file in `fileToUpload`.
    prefix += delimiter;
    prefix += "Content-Disposition: form-data; name=\"reqtype\"\r\n\r\n";
    prefix += "fileupload\r\n";
    prefix += delimiter;
    prefix += "Content-Disposition: form-data; name=\"time\"\r\n\r\n";
    prefix += "72h\r\n";
    prefix += delimiter;
    prefix += "Content-Disposition: form-data; name=\"fileToUpload\"; filename=\"";
  } else if (shape == UploadShape::kCatbox) {
    // Two parts: reqtype, then the file in `fileToUpload`. Catbox stores
    // permanently, so unlike litterbox there is no time field.
    prefix += delimiter;
    prefix += "Content-Disposition: form-data; name=\"reqtype\"\r\n\r\n";
    prefix += "fileupload\r\n";
    prefix += delimiter;
    prefix += "Content-Disposition: form-data; name=\"fileToUpload\"; filename=\"";
  } else {
    // One part only, in `files[]`, with no extra fields.
    prefix += delimiter;
    prefix += "Content-Disposition: form-data; name=\"files[]\"; filename=\"";
  }
  prefix += FilenameForMime(mime);
  prefix += "\"\r\nContent-Type: ";
  prefix += mime;
  prefix += "\r\n\r\n";
  return prefix;
}

std::string MultipartSuffix(const std::string& boundary) {
  return "\r\n--" + boundary + "--\r\n";
}

std::string BuildMultipartBody(const std::string& boundary, const std::string& mime,
                               const std::vector<unsigned char>& image,
                               UploadShape shape = UploadShape::kLitterbox) {
  std::string body = MultipartPrefix(boundary, mime, shape);
  if (!image.empty()) {
    body.append(reinterpret_cast<const char*>(image.data()), image.size());
  }
  body += MultipartSuffix(boundary);
  return body;
}

// --- Google Drive multipart/related body ------------------------------------
//
// Drive's upload endpoint takes a multipart/related body with exactly two
// parts: a JSON metadata part (name + parent folder), then the raw bytes under
// their real content type. Both parts share one boundary, and the request's own
// Content-Type is "multipart/related; boundary=<boundary>" - not form-data.
// The name and folder id are both hash/id shapes, so no JSON escaping is needed.

std::string DriveMultipartContentType(const std::string& boundary) {
  return "multipart/related; boundary=" + boundary;
}

std::string BuildDriveMetadataPart(const std::string& name, const std::string& folder_id) {
  return "{\"name\":\"" + name + "\",\"parents\":[\"" + folder_id + "\"]}";
}

std::string BuildDriveMultipartBody(const std::string& boundary, const std::string& name,
                                    const std::string& folder_id, const std::string& mime,
                                    const std::vector<unsigned char>& image) {
  const std::string delimiter = "--" + boundary + "\r\n";
  std::string body;
  body += delimiter;
  body += "Content-Type: application/json; charset=UTF-8\r\n\r\n";
  body += BuildDriveMetadataPart(name, folder_id);
  body += "\r\n";
  body += delimiter;
  body += "Content-Type: " + mime + "\r\n\r\n";
  if (!image.empty()) {
    body.append(reinterpret_cast<const char*>(image.data()), image.size());
  }
  body += "\r\n";
  body += "--" + boundary + "--\r\n";
  return body;
}

std::string MakeBoundary() {
  static const char kAlphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  const uint64_t seed = static_cast<uint64_t>(std::random_device()()) ^ static_cast<uint64_t>(GetTickCount64()) ^
                        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  std::mt19937_64 generator(seed);
  std::uniform_int_distribution<size_t> pick(0, sizeof(kAlphabet) - 2);
  std::string boundary = "----";
  while (boundary.size() < kBoundaryLength) {
    boundary.push_back(kAlphabet[pick(generator)]);
  }
  return boundary;
}

// --- Response classification ------------------------------------------------

// `shape` names the host that answered, so the 200 body must be that host's own
// bare URL. The default is litterbox, which keeps the strict two-argument form
// (and its callers) clamped to the original host.
Result ClassifyUploadResponse(DWORD status, const std::string& body,
                              UploadShape shape = UploadShape::kLitterbox) {
  const std::string text = TrimWhitespace(body);
  if (status == 200) {
    if (text.empty()) {
      return Failure("empty-body");
    }
    if (LooksLikeHostUrl(text, UploadHost{L"", L"", L"", shape})) {
      Result result;
      result.ok = true;
      result.url = text;
      return result;
    }
    return Failure("http-200-unexpected-body");
  }
  if (status == 412) {
    return Failure(text.empty() ? std::string("http-412") : text);
  }
  return Failure("http-" + std::to_string(status));
}

// --- GitHub contents API ----------------------------------------------------
//
// The GitHub host is the owner's own permanent repo. A cover is written once,
// named by the SHA-256 of the uploaded bytes, so re-uploading the same image is
// idempotent: the contents API answers 422 (or 409) when the file is already
// present, and that is treated as success via the raw URL. The access token is
// only ever placed in the Authorization header of a request to api.github.com;
// it never reaches a Result, the cache, or a log line.

std::string Base64Encode(const unsigned char* data, size_t length) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string encoded;
  encoded.reserve(((length + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= length) {
    const uint32_t chunk = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) |
                           static_cast<uint32_t>(data[i + 2]);
    encoded.push_back(kAlphabet[(chunk >> 18) & 0x3f]);
    encoded.push_back(kAlphabet[(chunk >> 12) & 0x3f]);
    encoded.push_back(kAlphabet[(chunk >> 6) & 0x3f]);
    encoded.push_back(kAlphabet[chunk & 0x3f]);
    i += 3;
  }
  const size_t remaining = length - i;
  if (remaining == 1) {
    const uint32_t chunk = static_cast<uint32_t>(data[i]) << 16;
    encoded.push_back(kAlphabet[(chunk >> 18) & 0x3f]);
    encoded.push_back(kAlphabet[(chunk >> 12) & 0x3f]);
    encoded += "==";
  } else if (remaining == 2) {
    const uint32_t chunk = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8);
    encoded.push_back(kAlphabet[(chunk >> 18) & 0x3f]);
    encoded.push_back(kAlphabet[(chunk >> 12) & 0x3f]);
    encoded.push_back(kAlphabet[(chunk >> 6) & 0x3f]);
    encoded.push_back('=');
  }
  return encoded;
}

// A small, dependency-free JSON string field reader in the same style as
// album_art.cpp's extractor: find the quoted key, then read the quoted string
// that follows it. GitHub's URLs never contain escapes, but a backslash is
// consumed so a surprising response cannot desync the scan.
std::string ExtractJsonStringField(const std::string& json, const std::string& key) {
  const size_t position = json.find("\"" + key + "\"");
  if (position == std::string::npos) {
    return std::string();
  }
  size_t cursor = position + key.size() + 2;
  const auto skip_space = [&]() {
    while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t' ||
                                    json[cursor] == '\r' || json[cursor] == '\n')) {
      ++cursor;
    }
  };
  skip_space();
  if (cursor >= json.size() || json[cursor] != ':') {
    return std::string();
  }
  ++cursor;
  skip_space();
  if (cursor >= json.size() || json[cursor] != '"') {
    return std::string();
  }
  ++cursor;
  std::string value;
  while (cursor < json.size() && json[cursor] != '"') {
    if (json[cursor] == '\\' && cursor + 1 < json.size()) {
      ++cursor;
    }
    value.push_back(json[cursor]);
    ++cursor;
  }
  return value;
}

std::string GitHubRawUrl(const std::wstring& owner, const std::wstring& repo,
                         const std::string& name) {
  return "https://raw.githubusercontent.com/" + Utils::ToString(owner) + "/" +
         Utils::ToString(repo) + "/main/covers/" + name + ".jpg";
}

// The exact request the GitHub host sends. Split out as a pure builder so the
// method, target, headers and JSON body are testable without a network.
struct GitHubRequest {
  std::wstring method;
  std::wstring host;
  std::wstring path;
  std::wstring headers;
  std::string body;
};

GitHubRequest BuildGitHubRequest(const std::string& token, const std::wstring& owner,
                                 const std::wstring& repo, const std::string& name,
                                 const std::vector<unsigned char>& image) {
  GitHubRequest request;
  request.method = L"PUT";
  request.host = L"api.github.com";
  request.path =
      L"/repos/" + owner + L"/" + repo + L"/contents/covers/" + Utils::ToWString(name) + L".jpg";
  request.headers = L"Authorization: Bearer " + Utils::ToWString(token) + L"\r\n" +
                    L"Accept: application/vnd.github+json\r\n" +
                    L"User-Agent: AIMP-Discord-Presence\r\n" +
                    L"X-GitHub-Api-Version: 2022-11-28\r\n" +
                    L"Content-Type: application/json\r\n";
  const std::string encoded =
      Base64Encode(image.empty() ? nullptr : image.data(), image.size());
  const std::string short_sha = name.size() >= 7 ? name.substr(0, 7) : name;
  request.body = "{\"message\":\"add cover " + short_sha + "\",\"content\":\"" + encoded +
                 "\",\"branch\":\"main\"}";
  return request;
}

// Classifies a GitHub contents PUT. The success body is JSON and the URL is
// content.download_url, pinned to this exact owner and repo; a body without
// that field is never adopted as a URL. 422/409 mean the immutable file is
// already there, so the constructed raw URL is returned and the caller's health
// check proves it retrievable.
Result ClassifyGitHubResponse(DWORD status, const std::string& body, const std::wstring& owner,
                              const std::wstring& repo, const std::string& name) {
  if (status == 200 || status == 201) {
    const std::string text = TrimWhitespace(body);
    if (text.empty()) {
      return Failure("empty-body");
    }
    const std::string download = ExtractJsonStringField(text, "download_url");
    const std::string prefix = "https://raw.githubusercontent.com/" + Utils::ToString(owner) +
                               "/" + Utils::ToString(repo) + "/";
    if (download.size() > prefix.size() && download.compare(0, prefix.size(), prefix) == 0 &&
        LooksLikeHostUrl(download, UploadHost{L"", L"", L"", UploadShape::kGitHub})) {
      Result result;
      result.ok = true;
      result.url = download;
      return result;
    }
    return Failure("http-" + std::to_string(status) + "-unexpected-body");
  }
  if (status == 422 || status == 409) {
    Result result;
    result.ok = true;
    result.url = GitHubRawUrl(owner, repo, name);
    return result;
  }
  // 401/403 (bad or expired token) and every other status name only the status;
  // the response body is never echoed, so nothing it contains can leak.
  return Failure("http-" + std::to_string(status));
}

// --- Google Drive API -------------------------------------------------------
//
// The Drive host is the owner's own storage. The file is named by the SHA-256
// of the uploaded bytes, matching the GitHub host's convention and the cache
// key, and lives in one bootstrap folder made publicly readable once; children
// inherit that, so no per-file permission call is ever made. The bearer token
// only ever reaches googleapis.com, in a request header or the token body.

bool LooksLikeDriveFileId(const std::string& id) {
  if (id.size() < 10 || id.size() > 200) {
    return false;
  }
  for (char c : id) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' ||
          c == '_')) {
      return false;
    }
  }
  return true;
}

// The URL Discord renders for a Drive file. Kept short (host + 2 + id) for the
// presence payload's 300-character budget.
std::string DriveLh3Url(const std::string& file_id) {
  return std::string(kDriveLh3UrlPrefix) + file_id;
}

// reads a bare JSON number field (expires_in). 0 when absent or malformed.
long long ExtractJsonNumberField(const std::string& json, const std::string& key) {
  const size_t position = json.find("\"" + key + "\"");
  if (position == std::string::npos) {
    return 0;
  }
  size_t cursor = position + key.size() + 2;
  while (cursor < json.size() &&
         (json[cursor] == ' ' || json[cursor] == '\t' || json[cursor] == '\r' || json[cursor] == '\n')) {
    ++cursor;
  }
  if (cursor >= json.size() || json[cursor] != ':') {
    return 0;
  }
  ++cursor;
  while (cursor < json.size() &&
         (json[cursor] == ' ' || json[cursor] == '\t' || json[cursor] == '\r' || json[cursor] == '\n')) {
    ++cursor;
  }
  long long value = 0;
  bool any = false;
  while (cursor < json.size() && json[cursor] >= '0' && json[cursor] <= '9') {
    value = value * 10 + (json[cursor] - '0');
    any = true;
    ++cursor;
  }
  return any ? value : 0;
}

// The first file id in a list response (`{"files":[{"id":"..."}]}`), or the id
// of a create response. Empty when the body carries no plausible id.
std::string FirstDriveFileId(const std::string& json) {
  const size_t files = json.find("\"files\"");
  const std::string scope = files == std::string::npos ? json : json.substr(files);
  const std::string id = ExtractJsonStringField(scope, "id");
  return LooksLikeDriveFileId(id) ? id : std::string();
}

// Classifies the upload response: 200/201 with a plausible file id, else a
// failure that names the stage. 403 is Drive's general refusal (quota, sharing
// policy, disabled API); 401 is a rejected bearer token. The body is never
// echoed, so nothing a response contains can leak into a log line.
Result ClassifyDriveUpload(DWORD status, const std::string& body) {
  if (status == 200 || status == 201) {
    const std::string id = ExtractJsonStringField(TrimWhitespace(body), "id");
    if (!LooksLikeDriveFileId(id)) {
      return Failure("drive-http-" + std::to_string(status) + "-unexpected-body");
    }
    Result result;
    result.ok = true;
    result.url = DriveLh3Url(id);
    return result;
  }
  if (status == 401 || status == 403) {
    return Failure("drive-http-" + std::to_string(status));
  }
  if (status == 429) {
    return Failure("drive-quota");
  }
  return Failure("drive-http-" + std::to_string(status));
}

// The folder lookup. Encoded once here so the request target is testable
// without a network: name, mimeType and trashed are the three filters the API
// needs, and only ids come back.
std::string BuildDriveFolderQuery() {
  const std::string query = "name='" + std::string(kDriveFolderName) +
                            "' and mimeType='application/vnd.google-apps.folder' and trashed=false";
  return "/drive/v3/files?q=" + Utils::UriEncode(query) +
         "&spaces=drive&pageSize=10&fields=files(id)";
}

std::string BuildDriveFolderMetadata() {
  return std::string("{\"name\":\"") + kDriveFolderName +
         "\",\"mimeType\":\"application/vnd.google-apps.folder\"}";
}

// The public-read grant for the bootstrap folder: one call, once, and every
// child inherits it.
std::string BuildDrivePermissionMetadata() { return "{\"role\":\"reader\",\"type\":\"anyone\"}"; }

// What a public-read grant on the bootstrap folder means. Only an explicit
// already-exists answer counts as success: persisting the folder as bootstrapped
// after a refusal would cache an unfetchable lh3 URL for every later cover.
enum class DrivePermissionVerdict {
  kGranted,        // 200/201: the grant was created now
  kAlreadyShared,  // 409, or a 4xx (other than 400/403) whose body says it exists
  kDenied,         // 403: the API refused to share (org policy, sharing disabled)
  kRejected,       // 400 and every other status: a malformed request or a fault
};

// Case-insensitive search of the two wordings Drive has used for an existing
// grant: the API error text ("The permission already exists for the file") and
// the reason token ("alreadyExists"), plus the older "already shared".
bool BodySaysPermissionExists(const std::string& body) {
  std::string lower;
  lower.reserve(body.size() < 4096 ? body.size() : 4096);
  for (size_t i = 0; i < body.size() && lower.size() < 4096; ++i) {
    const char c = body[i];
    lower.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
  }
  return lower.find("already exists") != std::string::npos ||
         lower.find("alreadyexists") != std::string::npos ||
         lower.find("already shared") != std::string::npos;
}

// Pure, so the decision is testable without a network. A 400 is a malformed
// request and a 403 is a refusal; neither is evidence that the folder is
// readable, so neither is treated as "already shared" whatever the body says.
DrivePermissionVerdict ClassifyDrivePermission(DWORD status, const std::string& body) {
  if (status >= 200 && status < 300) {
    return DrivePermissionVerdict::kGranted;
  }
  if (status == 400) {
    return DrivePermissionVerdict::kRejected;
  }
  if (status == 403) {
    return DrivePermissionVerdict::kDenied;
  }
  if (status == 409) {
    return DrivePermissionVerdict::kAlreadyShared;
  }
  if (status >= 400 && status < 500 && BodySaysPermissionExists(body)) {
    return DrivePermissionVerdict::kAlreadyShared;
  }
  return DrivePermissionVerdict::kRejected;
}

// --- SHA-256 (cache keys) ---------------------------------------------------

constexpr uint32_t kSha256RoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

constexpr uint32_t kSha256InitialState[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                             0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

uint32_t RotateRight(uint32_t value, uint32_t bits) {
  return (value >> bits) | (value << (32 - bits));
}

void Sha256Transform(uint32_t state[8], const unsigned char block[64]) {
  uint32_t schedule[64];
  for (size_t i = 0; i < 16; ++i) {
    schedule[i] = static_cast<uint32_t>(block[i * 4]) << 24 | static_cast<uint32_t>(block[i * 4 + 1]) << 16 |
                  static_cast<uint32_t>(block[i * 4 + 2]) << 8 | static_cast<uint32_t>(block[i * 4 + 3]);
  }
  for (size_t i = 16; i < 64; ++i) {
    const uint32_t s0 =
        RotateRight(schedule[i - 15], 7) ^ RotateRight(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3);
    const uint32_t s1 =
        RotateRight(schedule[i - 2], 17) ^ RotateRight(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10);
    schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
  }
  uint32_t a = state[0];
  uint32_t b = state[1];
  uint32_t c = state[2];
  uint32_t d = state[3];
  uint32_t e = state[4];
  uint32_t f = state[5];
  uint32_t g = state[6];
  uint32_t h = state[7];
  for (size_t i = 0; i < 64; ++i) {
    const uint32_t sigma1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
    const uint32_t choice = (e & f) ^ (~e & g);
    const uint32_t temp1 = h + sigma1 + choice + kSha256RoundConstants[i] + schedule[i];
    const uint32_t sigma0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
    const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = sigma0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

class Sha256 {
 public:
  Sha256() { std::copy(kSha256InitialState, kSha256InitialState + 8, state_); }

  void Update(const unsigned char* data, size_t length) {
    if (data == nullptr || length == 0) {
      return;
    }
    total_bytes_ += length;
    size_t offset = 0;
    if (buffer_size_ > 0) {
      const size_t needed = sizeof(buffer_) - buffer_size_;
      const size_t take = length < needed ? length : needed;
      std::memcpy(buffer_ + buffer_size_, data, take);
      buffer_size_ += take;
      offset = take;
      if (buffer_size_ == sizeof(buffer_)) {
        Sha256Transform(state_, buffer_);
        buffer_size_ = 0;
      }
    }
    while (length - offset >= sizeof(buffer_)) {
      Sha256Transform(state_, data + offset);
      offset += sizeof(buffer_);
    }
    if (offset < length) {
      std::memcpy(buffer_, data + offset, length - offset);
      buffer_size_ = length - offset;
    }
  }

  void Final(unsigned char digest[32]) {
    const uint64_t total_bits = total_bytes_ * 8;
    unsigned char padding[72] = {};
    padding[0] = 0x80;
    const size_t pad_length = buffer_size_ < 56 ? 56 - buffer_size_ : 120 - buffer_size_;
    Update(padding, pad_length);
    unsigned char length_bytes[8];
    for (size_t i = 0; i < 8; ++i) {
      length_bytes[i] = static_cast<unsigned char>(total_bits >> (56 - i * 8));
    }
    Update(length_bytes, 8);
    for (size_t i = 0; i < 8; ++i) {
      digest[i * 4] = static_cast<unsigned char>(state_[i] >> 24);
      digest[i * 4 + 1] = static_cast<unsigned char>(state_[i] >> 16);
      digest[i * 4 + 2] = static_cast<unsigned char>(state_[i] >> 8);
      digest[i * 4 + 3] = static_cast<unsigned char>(state_[i]);
    }
  }

 private:
  uint32_t state_[8] = {};
  unsigned char buffer_[64] = {};
  size_t buffer_size_ = 0;
  uint64_t total_bytes_ = 0;
};

std::string Sha256Hex(const std::vector<unsigned char>& bytes) {
  unsigned char digest[32] = {};
  Sha256 hasher;
  hasher.Update(bytes.empty() ? nullptr : bytes.data(), bytes.size());
  hasher.Final(digest);
  static const char kHex[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(64);
  for (unsigned char byte : digest) {
    hex.push_back(kHex[byte >> 4]);
    hex.push_back(kHex[byte & 0x0f]);
  }
  return hex;
}

// --- PKCE, OAuth state, and base64url --------------------------------------
//
// RFC 7636 S256: a 43-character base64url verifier (32 random bytes) and its
// challenge, the base64url SHA-256 of the verifier. Neither value is secret in
// the sense of a credential - the challenge travels in the browser URL and the
// verifier only in the token POST - but both are unguessable.

std::string Base64UrlEncode(const unsigned char* data, size_t length) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string encoded;
  encoded.reserve((length * 4 + 2) / 3);
  size_t i = 0;
  while (i + 3 <= length && data != nullptr) {
    const uint32_t chunk = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) |
                           static_cast<uint32_t>(data[i + 2]);
    encoded.push_back(kAlphabet[(chunk >> 18) & 0x3f]);
    encoded.push_back(kAlphabet[(chunk >> 12) & 0x3f]);
    encoded.push_back(kAlphabet[(chunk >> 6) & 0x3f]);
    encoded.push_back(kAlphabet[chunk & 0x3f]);
    i += 3;
  }
  const size_t remaining = length - i;
  if (data != nullptr && remaining == 1) {
    const uint32_t chunk = static_cast<uint32_t>(data[i]) << 16;
    encoded.push_back(kAlphabet[(chunk >> 18) & 0x3f]);
    encoded.push_back(kAlphabet[(chunk >> 12) & 0x3f]);
  } else if (data != nullptr && remaining == 2) {
    const uint32_t chunk =
        (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8);
    encoded.push_back(kAlphabet[(chunk >> 18) & 0x3f]);
    encoded.push_back(kAlphabet[(chunk >> 12) & 0x3f]);
    encoded.push_back(kAlphabet[(chunk >> 6) & 0x3f]);
  }
  return encoded;
}

// std::random_device is the system RNG on MSVC (rand_s), which is what a token
// here needs; it is seeded per call, so no generator state is shared between
// threads.
std::string RandomToken(size_t byte_count) {
  std::random_device source;
  std::vector<unsigned char> bytes(byte_count);
  for (size_t i = 0; i < byte_count; ++i) {
    bytes[i] = static_cast<unsigned char>(source() & 0xffu);
  }
  return Base64UrlEncode(bytes.data(), bytes.size());
}

std::string PkceCodeVerifier() { return RandomToken(32); }

std::string PkceCodeChallenge(const std::string& verifier) {
  unsigned char digest[32] = {};
  Sha256 hasher;
  hasher.Update(reinterpret_cast<const unsigned char*>(verifier.data()), verifier.size());
  hasher.Final(digest);
  return Base64UrlEncode(digest, sizeof(digest));
}

std::string OAuthStateToken() { return RandomToken(16); }

// The full consent URL. `client_secret` is deliberately not a parameter: it
// must never reach a browser. The redirect is the loopback listener, and the
// challenge, not the verifier, travels here.
std::string BuildDriveAuthUrl(const std::string& client_id, const std::string& redirect_uri,
                              const std::string& verifier, const std::string& state) {
  std::string url = kDriveAuthEndpoint;
  url += "?client_id=";
  url += Utils::UriEncode(client_id);
  url += "&redirect_uri=";
  url += Utils::UriEncode(redirect_uri);
  url += "&response_type=code";
  url += "&scope=";
  url += Utils::UriEncode(std::string(kDriveScope));
  url += "&access_type=offline";
  url += "&prompt=consent";
  url += "&code_challenge=";
  url += PkceCodeChallenge(verifier);
  url += "&code_challenge_method=S256";
  url += "&state=";
  url += Utils::UriEncode(state);
  return url;
}

// --- Image preparation (WIC) ------------------------------------------------

// Compression is a best-effort transfer-size reduction, never a gate: any WIC
// failure returns the original bytes untouched. The sizing decision is split
// out as a pure function so it is testable without an encoder.
//
// Discord crops the large image to a centre square, so the re-encode first
// centre-crops to the shorter side and then scales that square down to at most
// kMaxImageDimension (512). The existing skip rule still holds: an image that
// is already at or below 512 px on both sides and at most 256 KB is not touched.
struct CompressionPlan {
  bool reencode = false;
  UINT width = 0;
  UINT height = 0;
};

CompressionPlan PlanCompression(UINT width, UINT height, size_t byte_size) {
  CompressionPlan plan;
  if (width == 0 || height == 0) {
    return plan;
  }
  if (width <= kMaxImageDimension && height <= kMaxImageDimension &&
      byte_size <= kSkipReencodeMaxBytes) {
    return plan;
  }
  plan.reencode = true;
  const UINT shorter = width < height ? width : height;
  const UINT side = shorter > kMaxImageDimension ? kMaxImageDimension : shorter;
  plan.width = side;
  plan.height = side;
  return plan;
}

struct PreparedImage {
  std::vector<unsigned char> bytes;  // exactly what gets uploaded
  std::string mime;                  // announces those bytes
  std::string note;                  // one-line size/dimension record for the log
  bool reencoded = false;
};

std::string HexHResult(HRESULT hr) {
  char buffer[16] = {};
  std::snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned int>(hr));
  return buffer;
}

std::string ImageNote(size_t input_bytes, size_t output_bytes, UINT source_width, UINT source_height,
                      UINT output_width, UINT output_height, const char* mode) {
  return "input=" + std::to_string(input_bytes) + " output=" + std::to_string(output_bytes) +
         " dims=" + std::to_string(source_width) + "x" + std::to_string(source_height) + "->" +
         std::to_string(output_width) + "x" + std::to_string(output_height) + " mode=" + mode;
}

PreparedImage OriginalImage(const std::vector<unsigned char>& source, const char* mime, UINT width, UINT height,
                            const std::string& wic_error) {
  PreparedImage prepared;
  prepared.bytes = source;
  prepared.mime = mime != nullptr ? std::string(mime) : std::string();
  prepared.note = ImageNote(source.size(), source.size(), width, height, width, height, "original");
  if (!wic_error.empty()) {
    prepared.note += " wic-failed=" + wic_error;
  }
  return prepared;
}

PreparedImage PrepareImageForUpload(const std::vector<unsigned char>& source, const char* mime) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool own_com = SUCCEEDED(com);
  PreparedImage prepared;
  UINT source_width = 0;
  UINT source_height = 0;
  std::string error;

  {
    // Nested scope so every COM object is released before CoUninitialize.
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    Microsoft::WRL::ComPtr<IWICStream> input_stream;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.GetAddressOf()));
    if (SUCCEEDED(hr)) {
      hr = factory->CreateStream(&input_stream);
    }
    if (SUCCEEDED(hr)) {
      hr = input_stream->InitializeFromMemory(const_cast<BYTE*>(source.data()), static_cast<DWORD>(source.size()));
    }
    if (SUCCEEDED(hr)) {
      hr = factory->CreateDecoderFromStream(input_stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    }
    if (SUCCEEDED(hr)) {
      hr = decoder->GetFrame(0, &frame);
    }
    if (SUCCEEDED(hr)) {
      hr = frame->GetSize(&source_width, &source_height);
    }
    if (FAILED(hr)) {
      error = "decode:" + HexHResult(hr);
    }

    const CompressionPlan plan = PlanCompression(source_width, source_height, source.size());
    if (error.empty() && !plan.reencode) {
      prepared = OriginalImage(source, mime, source_width, source_height, std::string());
    } else if (error.empty()) {
      Microsoft::WRL::ComPtr<IWICBitmapClipper> clipper;
      Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
      Microsoft::WRL::ComPtr<IStream> output_stream;
      Microsoft::WRL::ComPtr<IWICStream> wic_output;
      Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
      Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> encoder_frame;
      Microsoft::WRL::ComPtr<IPropertyBag2> properties;

      // Centre-crop to the shorter side first, so the encode is square exactly
      // as Discord frames it; the scaler then brings that square within 512 px.
      hr = factory->CreateBitmapClipper(&clipper);
      if (SUCCEEDED(hr)) {
        const UINT shorter = source_width < source_height ? source_width : source_height;
        WICRect crop = {};
        crop.X = static_cast<INT>((source_width - shorter) / 2);
        crop.Y = static_cast<INT>((source_height - shorter) / 2);
        crop.Width = static_cast<INT>(shorter);
        crop.Height = static_cast<INT>(shorter);
        hr = clipper->Initialize(frame.Get(), &crop);
      }
      if (SUCCEEDED(hr)) {
        hr = factory->CreateBitmapScaler(&scaler);
      }
      if (SUCCEEDED(hr)) {
        hr = scaler->Initialize(clipper.Get(), plan.width, plan.height, WICBitmapInterpolationModeFant);
      }
      if (SUCCEEDED(hr)) {
        hr = CreateStreamOnHGlobal(nullptr, TRUE, output_stream.GetAddressOf());
      }
      if (SUCCEEDED(hr)) {
        hr = factory->CreateStream(&wic_output);
      }
      if (SUCCEEDED(hr)) {
        hr = wic_output->InitializeFromIStream(output_stream.Get());
      }
      if (SUCCEEDED(hr)) {
        hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
      }
      if (SUCCEEDED(hr)) {
        hr = encoder->Initialize(wic_output.Get(), WICBitmapEncoderNoCache);
      }
      if (SUCCEEDED(hr)) {
        hr = encoder->CreateNewFrame(&encoder_frame, &properties);
      }
      if (SUCCEEDED(hr)) {
        wchar_t quality_name[] = L"ImageQuality";
        PROPBAG2 option = {};
        option.pstrName = quality_name;
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = kJpegQuality;
        hr = properties->Write(1, &option, &value);
        VariantClear(&value);
      }
      if (SUCCEEDED(hr)) {
        hr = encoder_frame->Initialize(properties.Get());
      }
      if (SUCCEEDED(hr)) {
        hr = encoder_frame->SetSize(plan.width, plan.height);
      }
      WICPixelFormatGUID pixel_format = GUID_WICPixelFormat24bppBGR;
      if (SUCCEEDED(hr)) {
        hr = encoder_frame->SetPixelFormat(&pixel_format);
      }
      if (SUCCEEDED(hr)) {
        hr = encoder_frame->WriteSource(scaler.Get(), nullptr);
      }
      if (SUCCEEDED(hr)) {
        hr = encoder_frame->Commit();
      }
      if (SUCCEEDED(hr)) {
        hr = encoder->Commit();
      }

      HGLOBAL memory = nullptr;
      const void* data = nullptr;
      SIZE_T size = 0;
      if (SUCCEEDED(hr)) {
        hr = GetHGlobalFromStream(output_stream.Get(), &memory);
      }
      if (SUCCEEDED(hr) && memory != nullptr) {
        data = GlobalLock(memory);
        size = GlobalSize(memory);
        if (data == nullptr || size == 0) {
          hr = E_FAIL;
        }
      } else if (SUCCEEDED(hr)) {
        hr = E_FAIL;
      }
      if (SUCCEEDED(hr)) {
        prepared.bytes.assign(static_cast<const unsigned char*>(data),
                              static_cast<const unsigned char*>(data) + size);
        prepared.mime = "image/jpeg";
        prepared.note = ImageNote(source.size(), prepared.bytes.size(), source_width, source_height, plan.width,
                                  plan.height, "jpeg-q85");
        prepared.reencoded = true;
      } else {
        error = "encode:" + HexHResult(hr);
      }
      if (data != nullptr) {
        GlobalUnlock(memory);
      }
    }
  }

  if (own_com) {
    CoUninitialize();
  }
  if (!error.empty()) {
    return OriginalImage(source, mime, source_width, source_height, error);
  }
  return prepared;
}

// The cache key is the hash of the bytes that are actually uploaded. Hashing
// the source instead would let two different originals that compress alike
// collide, and would hide a changed encoder behind a stale key.
std::string UploadCacheKey(const PreparedImage& image) { return Sha256Hex(image.bytes); }

// --- Cache ------------------------------------------------------------------

struct CacheEntry {
  bool ok = false;
  std::string payload;  // URL when ok, failure reason otherwise
  long long expires_at = 0;
};

std::mutex g_cache_mutex;
std::string g_cache_path;
std::map<std::string, CacheEntry> g_cache;

bool IsCacheKey(const std::string& key) {
  if (key.size() != 64) {
    return false;
  }
  for (char c : key) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool ParseLongLong(const std::string& text, long long* value) {
  if (text.empty() || text.size() > 18) {
    return false;
  }
  long long parsed = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    parsed = parsed * 10 + (c - '0');
  }
  *value = parsed;
  return true;
}

std::string SanitizeCacheText(const std::string& text) {
  std::string clean;
  clean.reserve(std::min<size_t>(text.size(), 200));
  for (char c : text) {
    if (clean.size() >= 200) {
      break;
    }
    clean.push_back(c == '\t' || c == '\r' || c == '\n' ? ' ' : c);
  }
  return clean;
}

bool ParseCacheLine(const std::string& line, std::string* key, CacheEntry* entry) {
  if (line.empty() || line[0] == '#') {
    return false;
  }
  const size_t first = line.find('\t');
  if (first == std::string::npos || !IsCacheKey(line.substr(0, first))) {
    return false;
  }
  const size_t second = line.find('\t', first + 1);
  if (second == std::string::npos) {
    return false;
  }
  const std::string state = line.substr(first + 1, second - first - 1);
  const std::string payload = line.substr(second + 1);
  CacheEntry parsed;
  if (state == "ok") {
    if (!LooksLikeUploadUrl(payload)) {
      return false;
    }
    parsed.ok = true;
    parsed.payload = payload;
  } else if (state == "fail") {
    const size_t third = payload.find('\t');
    if (third == std::string::npos || !ParseLongLong(payload.substr(0, third), &parsed.expires_at) ||
        parsed.expires_at <= 0) {
      return false;
    }
    parsed.ok = false;
    parsed.payload = payload.substr(third + 1);
  } else {
    return false;
  }
  *key = line.substr(0, first);
  *entry = parsed;
  return true;
}

std::string FormatCacheLine(const std::string& key, const CacheEntry& entry) {
  if (entry.ok) {
    return key + "\tok\t" + entry.payload;
  }
  return key + "\tfail\t" + std::to_string(entry.expires_at) + "\t" + entry.payload;
}

bool ReadWholeFile(const std::string& path, std::string* out) {
  out->clear();
  const std::wstring wide = Utils::ToWString(path);
  if (wide.empty()) {
    return false;
  }
  HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  char buffer[4096];
  bool ok = true;
  for (;;) {
    DWORD read = 0;
    if (ReadFile(file, buffer, static_cast<DWORD>(sizeof(buffer)), &read, nullptr) == FALSE) {
      ok = false;
      break;
    }
    if (read == 0) {
      break;
    }
    out->append(buffer, read);
    if (out->size() > kMaxCacheBytes) {
      ok = false;
      break;
    }
  }
  CloseHandle(file);
  if (!ok) {
    out->clear();
  }
  return ok;
}

bool WriteWholeFile(const std::string& path, const std::string& content) {
  const std::wstring wide = Utils::ToWString(path);
  if (wide.empty()) {
    return false;
  }
  const std::wstring temp = wide + L".tmp";
  HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  bool ok = true;
  const char* data = content.data();
  size_t remaining = content.size();
  while (remaining > 0) {
    const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 1 << 20));
    DWORD written = 0;
    if (WriteFile(file, data, chunk, &written, nullptr) == FALSE || written != chunk) {
      ok = false;
      break;
    }
    data += chunk;
    remaining -= chunk;
  }
  CloseHandle(file);
  if (!ok || MoveFileExW(temp.c_str(), wide.c_str(), MOVEFILE_REPLACE_EXISTING) == FALSE) {
    DeleteFileW(temp.c_str());
    return false;
  }
  return true;
}

void LoadCacheLocked() {
  g_cache.clear();
  std::string content;
  if (!ReadWholeFile(g_cache_path, &content)) {
    return;
  }
  size_t start = 0;
  for (;;) {
    const size_t end = content.find('\n', start);
    const std::string line =
        end == std::string::npos ? content.substr(start) : content.substr(start, end - start);
    std::string key;
    CacheEntry entry;
    if (ParseCacheLine(line, &key, &entry)) {
      g_cache[key] = entry;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
}

void SaveCacheLocked() {
  std::string content = "# cover publisher cache v1\n";
  for (const auto& item : g_cache) {
    content += FormatCacheLine(item.first, item.second);
    content += "\n";
  }
  WriteWholeFile(g_cache_path, content);  // best effort: unwritable paths degrade to memory only
}

bool CacheLookup(const std::string& key, long long now, Result* out) {
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  if (g_cache_path.empty()) {
    return false;
  }
  const auto found = g_cache.find(key);
  if (found == g_cache.end()) {
    return false;
  }
  if (!found->second.ok && found->second.expires_at <= now) {
    g_cache.erase(found);
    SaveCacheLocked();
    return false;
  }
  Result result;
  if (found->second.ok) {
    result.ok = true;
    result.url = found->second.payload;
    // Name the host that served this URL, so a cache hit reports its real host
    // ("catbox.moe", "litter.catbox.moe", ...) instead of an empty field the
    // caller logs as "unknown".
    result.host = HostNameForUrl(result.url);
  } else {
    result.reason = found->second.payload;
  }
  *out = result;
  return true;
}

void CacheStore(const std::string& key, const Result& result, long long now) {
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  if (g_cache_path.empty()) {
    return;
  }
  CacheEntry entry;
  if (result.ok && LooksLikeUploadUrl(result.url)) {
    entry.ok = true;
    entry.payload = result.url;
  } else {
    entry.ok = false;
    entry.expires_at = now + kNegativeCacheTtlSeconds;
    entry.payload = SanitizeCacheText(result.ok ? std::string("unexpected-body") : result.reason);
  }
  g_cache[key] = entry;
  SaveCacheLocked();
}

// --- Drive sidecar (refresh token + bootstrap folder) -----------------------
//
// The sidecar is a tiny key=value text file. It holds the refresh token and the
// bootstrap folder id and nothing else: no access token (in memory only), no
// client secret, no image bytes, never a log line. The parser is strict so a
// truncated, clobbered or foreign file reads as "not authorized" rather than as
// a token that would be sent somewhere.

struct DriveState {
  std::string refresh_token;
  std::string folder_id;
};

// Tokens are opaque but always printable and URL-safe. Anything with
// whitespace, control characters, quotes or an absurd length is corrupt.
bool IsDriveRefreshToken(const std::string& token) {
  if (token.size() < 16 || token.size() > 2048) {
    return false;
  }
  for (char c : token) {
    if (c <= 0x20 || c >= 0x7f || c == '"' || c == '\\' || c == '\'') {
      return false;
    }
  }
  return true;
}

bool ParseDriveState(const std::string& content, DriveState* state) {
  DriveState parsed;
  size_t start = 0;
  for (;;) {
    const size_t end = content.find('\n', start);
    std::string line =
        end == std::string::npos ? content.substr(start) : content.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!line.empty() && line[0] != '#') {
      const size_t equals = line.find('=');
      if (equals == std::string::npos || equals == 0) {
        return false;  // a line that is not key=value: corrupt
      }
      const std::string key = line.substr(0, equals);
      const std::string value = line.substr(equals + 1);
      if (key == "refresh_token") {
        parsed.refresh_token = value;
      } else if (key == "folder_id") {
        // The folder id is only a bootstrap cache: an unusable value is dropped
        // (the next publish finds or creates the folder again) rather than
        // throwing away a perfectly good refresh token.
        parsed.folder_id = LooksLikeDriveFileId(value) ? value : std::string();
      } else {
        return false;  // an unknown key means this is not our file
      }
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  // A file without a usable refresh token is "not authorized", which is the
  // same outcome as no file at all: run consent again on the next publish.
  if (!IsDriveRefreshToken(parsed.refresh_token)) {
    return false;
  }
  *state = parsed;
  return true;
}

std::string FormatDriveState(const DriveState& state) {
  std::string content = "# aimp-discord-presence drive state v1\n";
  content += "refresh_token=" + state.refresh_token + "\n";
  if (!state.folder_id.empty()) {
    content += "folder_id=" + state.folder_id + "\n";
  }
  return content;
}

bool LoadDriveState(const std::string& path, DriveState* state) {
  if (path.empty()) {
    return false;
  }
  std::string content;
  if (!ReadWholeFile(path, &content)) {
    return false;
  }
  return ParseDriveState(content, state);
}

// Best effort: a sidecar that cannot be written only costs a repeat consent on
// the next start, so it never fails the publish that just succeeded.
bool SaveDriveState(const std::string& path, const DriveState& state) {
  if (path.empty()) {
    return false;
  }
  return WriteWholeFile(path, FormatDriveState(state));
}

// --- WinHTTP plumbing -------------------------------------------------------

class WinHttpHandle {
 public:
  explicit WinHttpHandle(HINTERNET handle = nullptr) : handle_(handle) {}
  ~WinHttpHandle() {
    if (handle_ != nullptr) {
      WinHttpCloseHandle(handle_);
    }
  }
  WinHttpHandle(const WinHttpHandle&) = delete;
  WinHttpHandle& operator=(const WinHttpHandle&) = delete;
  WinHttpHandle(WinHttpHandle&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
  WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
    if (this != &other) {
      if (handle_ != nullptr) {
        WinHttpCloseHandle(handle_);
      }
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }
  HINTERNET get() const { return handle_; }
  explicit operator bool() const { return handle_ != nullptr; }

 private:
  HINTERNET handle_;
};

class Deadline {
 public:
  explicit Deadline(DWORD budget_ms)
      : end_(std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms)) {}

  bool Expired() const { return std::chrono::steady_clock::now() >= end_; }

  DWORD RemainingMs() const {
    const auto now = std::chrono::steady_clock::now();
    if (now >= end_) {
      return 1;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(end_ - now).count();
    if (remaining < 1) {
      return 1;
    }
    if (remaining > static_cast<long long>(kTotalTimeoutMs)) {
      return kTotalTimeoutMs;
    }
    return static_cast<DWORD>(remaining);
  }

 private:
  std::chrono::steady_clock::time_point end_;
};

WinHttpHandle OpenSession(const Deadline& deadline) {
  WinHttpHandle session(WinHttpOpen(kAgentName, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session) {
    return session;
  }
  const int timeout = static_cast<int>(deadline.RemainingMs());
  WinHttpSetTimeouts(session.get(), timeout, timeout, timeout, timeout);
  return session;
}

bool QueryStatusCode(HINTERNET request, DWORD* status, DWORD* error) {
  DWORD size = sizeof(DWORD);
  *status = 0;
  *error = 0;
  if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, status, &size, WINHTTP_NO_HEADER_INDEX) == FALSE) {
    *error = GetLastError();
    return false;
  }
  return true;
}

bool QueryContentType(HINTERNET request, std::wstring* content_type) {
  DWORD size = 0;
  WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
                      WINHTTP_NO_HEADER_INDEX);
  if (size == 0) {
    return false;
  }
  std::wstring buffer(size / sizeof(wchar_t), L'\0');
  if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &size,
                          WINHTTP_NO_HEADER_INDEX) == FALSE) {
    return false;
  }
  buffer.resize(size / sizeof(wchar_t));
  while (!buffer.empty() && buffer.back() == L'\0') {
    buffer.pop_back();
  }
  *content_type = buffer;
  return true;
}

bool StartsWithImageType(const std::wstring& content_type) {
  const wchar_t kPrefix[] = L"image/";
  if (content_type.size() < 6) {
    return false;
  }
  for (size_t i = 0; i < 6; ++i) {
    wchar_t c = content_type[i];
    if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    if (c != kPrefix[i]) {
      return false;
    }
  }
  return true;
}

// Names the exact failing call and carries GetLastError from it, so a body-read
// failure is never reported as the generic "receive-failed" again.
struct ReadOutcome {
  bool ok = false;
  const char* tag = "read-failed";
  DWORD error = 0;
};

ReadOutcome ReadResponseBody(HINTERNET request, const Deadline& deadline, std::string* out,
                             size_t max_bytes = kMaxResponseBytes) {
  out->clear();
  ReadOutcome outcome;
  for (;;) {
    if (deadline.Expired()) {
      outcome.tag = "read-timeout";
      outcome.error = ERROR_WINHTTP_TIMEOUT;  // our own budget ran out
      return outcome;
    }
    DWORD available = 0;
    if (WinHttpQueryDataAvailable(request, &available) == FALSE) {
      outcome.tag = "data-available-failed";
      outcome.error = GetLastError();
      return outcome;
    }
    if (available == 0) {
      outcome.ok = true;
      return outcome;
    }
    if (available > max_bytes) {
      available = static_cast<DWORD>(max_bytes);
    }
    const size_t offset = out->size();
    out->resize(offset + available);
    DWORD read = 0;
    if (WinHttpReadData(request, out->data() + offset, available, &read) == FALSE) {
      outcome.tag = "read-data-failed";
      outcome.error = GetLastError();
      return outcome;
    }
    out->resize(offset + read);
    if (read == 0 || out->size() >= max_bytes) {
      outcome.ok = true;
      return outcome;
    }
  }
}

// A failure counts as a transport failure only when it happened on the wire
// (connect/send/receive/timeout). Host answers - a 412, an empty body, any HTTP
// status - are real answers and are never retried.
struct UploadAttempt {
  Result result;
  bool transport_failure = false;
};

std::string TransportReason(const char* tag, const char* phase, DWORD error) {
  const char* category = error == ERROR_WINHTTP_TIMEOUT ? "timeout" : phase;
  return std::string(tag) + " phase=" + category + " winhttp=" + std::to_string(error);
}

UploadAttempt TransportFailure(const char* tag, const char* phase, DWORD error) {
  UploadAttempt attempt;
  attempt.result = Failure(TransportReason(tag, phase, error));
  attempt.transport_failure = true;
  return attempt;
}

UploadAttempt AttemptResult(const Result& result) {
  UploadAttempt attempt;
  attempt.result = result;
  return attempt;
}

UploadAttempt UploadMultipartBody(const UploadHost& upload_host, const std::string& body,
                                  const std::string& boundary) {
  if (body.size() > 0xffffffffull) {
    return AttemptResult(Failure("body-too-large"));
  }
  const Deadline deadline(kTotalTimeoutMs);
  WinHttpHandle session = OpenSession(deadline);
  if (!session) {
    return TransportFailure("session-open-failed", "connect", GetLastError());
  }
  WinHttpHandle connection(WinHttpConnect(session.get(), upload_host.host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
  if (!connection) {
    return TransportFailure("connect-failed", "connect", GetLastError());
  }
  WinHttpHandle request(WinHttpOpenRequest(connection.get(), L"POST", upload_host.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
  if (!request) {
    return TransportFailure("request-open-failed", "send", GetLastError());
  }
  const std::wstring headers = L"Content-Type: multipart/form-data; boundary=" + Utils::ToWString(boundary);
  const DWORD body_size = static_cast<DWORD>(body.size());
  // totalLen == optionalLen == the exact body size, so the request carries a
  // Content-Length and never falls back to chunked transfer encoding.
  if (WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(-1), const_cast<char*>(body.data()),
                         body_size, body_size, 0) == FALSE) {
    return TransportFailure("send-failed", "send", GetLastError());
  }
  if (WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
    return TransportFailure("receive-failed", "receive", GetLastError());
  }
  DWORD status = 0;
  DWORD status_error = 0;
  if (!QueryStatusCode(request.get(), &status, &status_error)) {
    // The host already answered; a retry would upload a second copy.
    return AttemptResult(Failure("status-query-failed phase=receive winhttp=" + std::to_string(status_error)));
  }
  std::string response;
  const ReadOutcome read = ReadResponseBody(request.get(), deadline, &response);
  if (!read.ok) {
    if (status == 200 || status == 412) {
      return AttemptResult(ClassifyUploadResponse(status, response, upload_host.shape));
    }
    return TransportFailure(read.tag, "receive", read.error);
  }
  return AttemptResult(ClassifyUploadResponse(status, response, upload_host.shape));
}

// Sends the GitHub contents PUT. The token is read from the guarded config and
// placed only in this request's Authorization header; the request and its
// headers are stack-local and are never logged. The success response is parsed
// for content.download_url; 422/409 is the immutable file already being there.
UploadAttempt UploadGitHubBody(const std::wstring& owner, const std::wstring& repo,
                               const std::string& name,
                               const std::vector<unsigned char>& image) {
  std::string token;
  {
    std::lock_guard<std::mutex> lock(g_host_mutex);
    token = g_github_token;
  }
  if (token.empty()) {
    return AttemptResult(Failure("github-no-token"));
  }
  const GitHubRequest github = BuildGitHubRequest(token, owner, repo, name, image);
  if (github.body.size() > 0xffffffffull) {
    return AttemptResult(Failure("body-too-large"));
  }
  const Deadline deadline(kTotalTimeoutMs);
  WinHttpHandle session = OpenSession(deadline);
  if (!session) {
    return TransportFailure("session-open-failed", "connect", GetLastError());
  }
  WinHttpHandle connection(
      WinHttpConnect(session.get(), github.host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
  if (!connection) {
    return TransportFailure("connect-failed", "connect", GetLastError());
  }
  WinHttpHandle request(WinHttpOpenRequest(connection.get(), github.method.c_str(),
                                           github.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
  if (!request) {
    return TransportFailure("request-open-failed", "send", GetLastError());
  }
  const DWORD body_size = static_cast<DWORD>(github.body.size());
  if (WinHttpSendRequest(request.get(), github.headers.c_str(), static_cast<DWORD>(-1),
                         const_cast<char*>(github.body.data()), body_size, body_size, 0) == FALSE) {
    return TransportFailure("send-failed", "send", GetLastError());
  }
  if (WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
    return TransportFailure("receive-failed", "receive", GetLastError());
  }
  DWORD status = 0;
  DWORD status_error = 0;
  if (!QueryStatusCode(request.get(), &status, &status_error)) {
    return AttemptResult(Failure("status-query-failed phase=receive winhttp=" +
                                 std::to_string(status_error)));
  }
  std::string response;
  const ReadOutcome read =
      ReadResponseBody(request.get(), deadline, &response, kMaxGitHubResponseBytes);
  if (!read.ok) {
    if (status == 200 || status == 201 || status == 422 || status == 409 || status == 401 ||
        status == 403) {
      return AttemptResult(ClassifyGitHubResponse(status, response, owner, repo, name));
    }
    return TransportFailure(read.tag, "receive", read.error);
  }
  return AttemptResult(ClassifyGitHubResponse(status, response, owner, repo, name));
}

// --- Google Drive transport -------------------------------------------------
//
// Every request here is built and sent on the cover worker thread. Nothing in
// this section runs on AIMP's message thread, and every wait is bounded: the
// transport by kTotalTimeoutMs, the interactive consent by the consent window
// (and by CancelDriveConsent at unload).

struct DriveConfig {
  std::string client_id;
  std::string client_secret;
  std::string state_path;
};

DriveConfig SnapshotDriveConfig() {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  DriveConfig config;
  config.client_id = g_drive_client_id;
  config.client_secret = g_drive_client_secret;
  config.state_path = g_drive_state_path;
  return config;
}

bool DriveConfigured(const DriveConfig& config) {
  return !config.client_id.empty() && !config.client_secret.empty() && !config.state_path.empty();
}

struct HttpResponse {
  bool transport_ok = false;
  DWORD status = 0;
  std::string body;
  std::string transport_reason;
};

// One HTTPS request with an explicit method, target, headers and body. Kept
// separate from the form-data upload helpers so the working keyless chain and
// the GitHub path are untouched by this host.
HttpResponse SendHttp(const std::wstring& method, const std::wstring& host, const std::wstring& path,
                      const std::wstring& headers, const std::string& body,
                      size_t max_bytes = kMaxDriveResponseBytes) {
  HttpResponse response;
  if (body.size() > 0xffffffffull) {
    response.transport_reason = "body-too-large";
    return response;
  }
  const Deadline deadline(kTotalTimeoutMs);
  WinHttpHandle session = OpenSession(deadline);
  if (!session) {
    response.transport_reason = TransportReason("session-open-failed", "connect", GetLastError());
    return response;
  }
  WinHttpHandle connection(WinHttpConnect(session.get(), host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
  if (!connection) {
    response.transport_reason = TransportReason("connect-failed", "connect", GetLastError());
    return response;
  }
  WinHttpHandle request(WinHttpOpenRequest(connection.get(), method.c_str(), path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE));
  if (!request) {
    response.transport_reason = TransportReason("request-open-failed", "send", GetLastError());
    return response;
  }
  const DWORD body_size = static_cast<DWORD>(body.size());
  if (WinHttpSendRequest(request.get(),
                         headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                         headers.empty() ? 0 : static_cast<DWORD>(-1),
                         body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                         body_size, body_size, 0) == FALSE) {
    response.transport_reason = TransportReason("send-failed", "send", GetLastError());
    return response;
  }
  if (WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
    response.transport_reason = TransportReason("receive-failed", "receive", GetLastError());
    return response;
  }
  DWORD status_error = 0;
  if (!QueryStatusCode(request.get(), &response.status, &status_error)) {
    // The host answered; the status is unreadable, so no retry (a retry could
    // create a second file).
    response.transport_ok = true;
    response.status = 0;
    response.transport_reason = "status-query-failed phase=receive winhttp=" +
                                std::to_string(status_error);
    return response;
  }
  const ReadOutcome read = ReadResponseBody(request.get(), deadline, &response.body, max_bytes);
  if (!read.ok) {
    response.transport_reason = TransportReason(read.tag, "receive", read.error);
    return response;
  }
  response.transport_ok = true;
  return response;
}

std::string FormEncode(const std::vector<std::pair<std::string, std::string>>& fields) {
  std::string body;
  for (const auto& field : fields) {
    if (!body.empty()) {
      body += "&";
    }
    body += Utils::UriEncode(field.first);
    body += "=";
    body += Utils::UriEncode(field.second);
  }
  return body;
}

// The token endpoint's answer. `refresh_token` is present only for an
// authorization_code grant (and, defensively, if Google ever re-issues one).
struct DriveTokenGrant {
  std::string access_token;
  std::string refresh_token;
  long long expires_in = 3600;
};

bool ParseDriveTokenGrant(const std::string& body, DriveTokenGrant* grant) {
  const std::string text = TrimWhitespace(body);
  const std::string access = ExtractJsonStringField(text, "access_token");
  if (access.empty()) {
    return false;
  }
  DriveTokenGrant parsed;
  parsed.access_token = access;
  const std::string refresh = ExtractJsonStringField(text, "refresh_token");
  if (!refresh.empty()) {
    parsed.refresh_token = refresh;
  }
  const long long expires = ExtractJsonNumberField(text, "expires_in");
  if (expires > 0) {
    parsed.expires_in = expires;
  }
  *grant = parsed;
  return true;
}

// Posts one grant to oauth2.googleapis.com. The client secret is in the form
// body and nowhere else; a failure names only the status, never the body.
bool PostDriveToken(const std::vector<std::pair<std::string, std::string>>& fields,
                    DriveTokenGrant* grant, std::string* reason) {
  const HttpResponse response =
      SendHttp(L"POST", L"oauth2.googleapis.com", L"/token",
               L"Content-Type: application/x-www-form-urlencoded\r\n", FormEncode(fields));
  if (!response.transport_ok) {
    *reason = response.transport_reason.empty() ? std::string("token-transport-failed")
                                                : "token-" + response.transport_reason;
    return false;
  }
  if (response.status != 200) {
    *reason = "token-http-" + std::to_string(response.status);
    return false;
  }
  if (!ParseDriveTokenGrant(response.body, grant)) {
    *reason = "token-unexpected-body";
    return false;
  }
  return true;
}

// The access token, refreshed or obtained by consent. Guarded together with the
// cached access token: only the cover worker ever calls this, and the lock also
// keeps a second concurrent publish (should one ever exist) from consenting
// twice.
std::mutex g_drive_session_mutex;
std::string g_drive_access_token;
long long g_drive_access_expires_at = 0;
std::string g_drive_folder_id;

// The consent wait's abort flag, set by CancelDriveConsent (plugin unload) and
// cleared at the start of every consent. The cooldown timestamp stops an
// abandoned or declined consent from popping a browser on every track.
std::atomic<bool> g_drive_consent_cancel{false};
std::atomic<long long> g_drive_consent_failed_at{0};

// --- Loopback consent listener ----------------------------------------------

bool EnsureWinsock(std::string* reason) {
  static std::once_flag once;
  static bool started = false;
  std::call_once(once, []() {
    WSADATA data = {};
    started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
  });
  if (!started) {
    *reason = "consent-winsock-unavailable";
  }
  return started;
}

struct LoopbackParams {
  std::string code;
  std::string state;
  std::string error;
};

std::string QueryParameter(const std::string& query, const std::string& key) {
  size_t start = 0;
  for (;;) {
    const size_t end = query.find('&', start);
    const std::string pair =
        end == std::string::npos ? query.substr(start) : query.substr(start, end - start);
    const size_t equals = pair.find('=');
    const std::string name = equals == std::string::npos ? pair : pair.substr(0, equals);
    if (name == key) {
      return Utils::UriDecode(equals == std::string::npos ? std::string() : pair.substr(equals + 1));
    }
    if (end == std::string::npos) {
      return std::string();
    }
    start = end + 1;
  }
}

// The request target of a loopback callback, e.g. "/?state=abc&code=xyz".
LoopbackParams ParseLoopbackTarget(const std::string& target) {
  LoopbackParams params;
  const size_t question = target.find('?');
  if (question == std::string::npos) {
    return params;
  }
  const std::string query = target.substr(question + 1);
  params.code = QueryParameter(query, "code");
  params.state = QueryParameter(query, "state");
  params.error = QueryParameter(query, "error");
  return params;
}

// The request target (the second token of the request line). Empty when the
// request line is malformed.
std::string RequestTarget(const std::string& request) {
  const size_t line_end = request.find("\r\n");
  const std::string line = line_end == std::string::npos ? request : request.substr(0, line_end);
  const size_t first = line.find(' ');
  if (first == std::string::npos) {
    return std::string();
  }
  const size_t second = line.find(' ', first + 1);
  if (second == std::string::npos) {
    return std::string();
  }
  return line.substr(first + 1, second - first - 1);
}

std::string ConsentPage(const char* message) {
  const std::string body =
      std::string("<!doctype html><html><head><meta charset=\"utf-8\">"
                  "<title>AIMP Discord Presence</title></head><body><p>") +
      message + "</p></body></html>";
  return "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
         std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

struct ConsentCallback {
  bool ok = false;
  std::string code;
  std::string error;  // "cancelled", "timeout", "unexpected-request", or Google's error code
};

// --- Consent deadline arithmetic -------------------------------------------
//
// The whole consent operation (browser hand-off plus loopback wait) shares one
// absolute deadline, and every blocking call is clamped to what remains. The
// arithmetic is pure, so the hard bound is testable without a socket.

long long NowMillis() {
  return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
}

// Milliseconds left before `deadline_ms`: 0 once it has passed, never negative,
// so a caller can clamp a blocking call to it without an unsigned wraparound.
long long RemainingMillis(long long deadline_ms, long long now_ms) {
  return now_ms < deadline_ms ? deadline_ms - now_ms : 0;
}

// How long one blocking call may wait: the remaining budget capped at `cap_ms`,
// and 0 when the deadline has already passed (the caller must not block at all).
long long ReceiveSliceMillis(long long deadline_ms, long long now_ms, long long cap_ms) {
  const long long remaining = RemainingMillis(deadline_ms, now_ms);
  if (remaining <= 0 || cap_ms <= 0) {
    return 0;
  }
  return remaining < cap_ms ? remaining : cap_ms;
}

// Accepts at most this many loopback connections: enough for a browser's silent
// preconnect followed by the real callback, and a hard cap on everything else.
constexpr size_t kDriveConsentMaxConnections = 2;

// Closes its socket on every exit path, including an early return or an unwind.
class SocketHandle {
 public:
  explicit SocketHandle(SOCKET socket = INVALID_SOCKET) : socket_(socket) {}
  ~SocketHandle() { Reset(); }
  SocketHandle(const SocketHandle&) = delete;
  SocketHandle& operator=(const SocketHandle&) = delete;

  SOCKET get() const { return socket_; }
  bool valid() const { return socket_ != INVALID_SOCKET; }

  void Reset(SOCKET socket = INVALID_SOCKET) {
    if (socket_ != INVALID_SOCKET) {
      closesocket(socket_);
    }
    socket_ = socket;
  }

 private:
  SOCKET socket_;
};

// What the accept loop does next, decided from one select() result. Pure, so the
// wait's bound is testable without sockets: a select() timeout (0) is only a
// poll tick and must never end the wait - counting polls as connections is what
// collapsed the window to two 250 ms slices - while a ready socket (> 0)
// proceeds to accept. Only the cancel flag or the absolute deadline ends it.
enum class ConsentStep { kKeepWaiting, kAccept, kCancelled, kExpired };

ConsentStep DecideConsentStep(int select_result, bool cancelled, long long now_ms,
                              long long deadline_ms) {
  if (cancelled) {
    return ConsentStep::kCancelled;
  }
  if (RemainingMillis(deadline_ms, now_ms) <= 0) {
    return ConsentStep::kExpired;
  }
  return select_result > 0 ? ConsentStep::kAccept : ConsentStep::kKeepWaiting;
}

// The callback's error text is attacker-controllable: it arrives as a query
// parameter, so it can carry newlines, quotes or anything else a crafted request
// likes. It is never echoed - it is mapped onto a closed set of fixed tokens, and
// everything else becomes "error".
std::string ConsentErrorToken(const std::string& error) {
  if (error == "access_denied") {
    return "access_denied";
  }
  if (error == "invalid_scope") {
    return "invalid_scope";
  }
  if (error == "invalid_request") {
    return "invalid_request";
  }
  return "error";
}

// Accepts loopback connections until the matching reply arrives, the absolute
// deadline passes, or the wait is cancelled. Every blocking call is bounded
// twice - by SO_RCVTIMEO on the accepted socket and by a deadline check before
// each receive - so a local client that connects and sends nothing, or sends a
// byte at a time, can never extend the wait or hold the publisher worker join.
// Only an actually accepted socket consumes the connection budget, so idle
// polling never shortens the window. The listener is only ever accepted from
// here, one connection at a time.
ConsentCallback WaitForConsentCallback(SOCKET listener, const std::string& expected_state,
                                       long long deadline_ms) {
  ConsentCallback callback;
  const std::string waiting_page = ConsentPage("Waiting for the authorization reply. Return to AIMP.");
  size_t accepted = 0;
  for (;;) {
    if (accepted >= kDriveConsentMaxConnections) {
      callback.error = "timeout";
      return callback;
    }
    const long long wait_ms = ReceiveSliceMillis(deadline_ms, NowMillis(), kDriveConsentPollMs);
    if (wait_ms <= 0) {
      callback.error = "timeout";
      return callback;
    }
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(listener, &read_set);
    timeval slice = {};
    slice.tv_sec = static_cast<long>(wait_ms / 1000);
    slice.tv_usec = static_cast<long>((wait_ms % 1000) * 1000);
    const int ready = select(0, &read_set, nullptr, nullptr, &slice);
    const ConsentStep step =
        DecideConsentStep(ready, g_drive_consent_cancel.load(), NowMillis(), deadline_ms);
    if (step == ConsentStep::kKeepWaiting) {
      if (ready < 0) {
        Sleep(kDriveConsentPollMs);  // a select error must not spin the worker
      }
      continue;  // a poll tick or a spurious wakeup: nothing has been consumed
    }
    if (step == ConsentStep::kCancelled) {
      callback.error = "cancelled";
      return callback;
    }
    if (step == ConsentStep::kExpired) {
      callback.error = "timeout";
      return callback;
    }
    SocketHandle client(accept(listener, nullptr, nullptr));
    if (!client.valid()) {
      continue;  // the connection died before accept: no client was served
    }
    ++accepted;  // only a socket actually accepted consumes the budget

    const long long receive_ms =
        ReceiveSliceMillis(deadline_ms, NowMillis(), kDriveReceiveTimeoutMs);
    if (receive_ms <= 0) {
      callback.error = "timeout";
      return callback;  // the client socket is closed by the guard
    }
    DWORD receive_timeout = static_cast<DWORD>(receive_ms);
    setsockopt(client.get(), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&receive_timeout), sizeof(receive_timeout));

    std::string request;
    char buffer[1024];
    for (;;) {
      if (g_drive_consent_cancel.load()) {
        callback.error = "cancelled";
        return callback;
      }
      if (ReceiveSliceMillis(deadline_ms, NowMillis(), kDriveReceiveTimeoutMs) <= 0) {
        callback.error = "timeout";
        return callback;
      }
      const int received = recv(client.get(), buffer, static_cast<int>(sizeof(buffer)), 0);
      if (received <= 0) {
        break;  // closed, or the receive timeout fired: drop this connection
      }
      request.append(buffer, static_cast<size_t>(received));
      if (request.find("\r\n\r\n") != std::string::npos ||
          request.size() >= kMaxLoopbackRequestBytes) {
        break;
      }
    }
    if (g_drive_consent_cancel.load()) {
      callback.error = "cancelled";
      return callback;
    }
    if (request.find("\r\n\r\n") == std::string::npos) {
      continue;  // a silent preconnect or a truncated request: drop it, keep waiting
    }

    const LoopbackParams params = ParseLoopbackTarget(RequestTarget(request));
    const bool answers_this_consent =
        params.state == expected_state && (!params.code.empty() || !params.error.empty());
    const std::string page =
        answers_this_consent
            ? ConsentPage(params.error.empty()
                              ? "Google Drive is connected. You can close this tab."
                              : "Authorization was not granted. You can close this tab.")
            : waiting_page;
    send(client.get(), page.data(), static_cast<int>(page.size()), 0);
    client.Reset();  // close before answering, so the browser's page is complete

    if (answers_this_consent) {
      callback.ok = params.error.empty();
      callback.code = params.code;
      // The raw callback text never leaves this function: only a fixed token.
      callback.error =
          params.error.empty() ? std::string() : ConsentErrorToken(params.error);
      return callback;
    }
    // One request has been handled: stop listening rather than serve further
    // connections. A reply that is not ours cannot complete this consent.
    callback.error = "unexpected-request";
    return callback;
  }
}

// The one interactive step: browser, loopback, PKCE, then persist the refresh
// token. Returns false with a reason on every failure, so the caller falls
// through to the next host instead of throwing or hanging.
bool RunDriveConsent(const DriveConfig& config, DriveState* state, std::string* reason) {
  // One absolute deadline for the whole operation: socket setup, the browser
  // hand-off, the loopback wait and the token exchange all draw from it, so no
  // client behaviour can extend it.
  const long long consent_deadline_ms = NowMillis() + static_cast<long long>(kDriveConsentWindowMs);
  std::string winsock_reason;
  if (!EnsureWinsock(&winsock_reason)) {
    *reason = winsock_reason;
    return false;
  }
  // No reset here: the cancel flag is set once, by unload, and must stay set so
  // a consent that starts after the unload began cannot hold the worker join
  // open. ConfigureDrive clears it at load.
  if (g_drive_consent_cancel.load()) {
    *reason = "consent-cancelled";
    return false;
  }

  SocketHandle listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  if (!listener.valid()) {
    *reason = "consent-socket-failed";
    return false;
  }
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;  // ephemeral port, chosen by the OS
  bool listening =
      bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
      listen(listener.get(), 1) == 0;
  unsigned short port = 0;
  if (listening) {
    sockaddr_in bound = {};
    int bound_length = static_cast<int>(sizeof(bound));
    if (getsockname(listener.get(), reinterpret_cast<sockaddr*>(&bound), &bound_length) == 0) {
      port = ntohs(bound.sin_port);
    }
    listening = port != 0;
  }
  if (!listening) {
    *reason = "consent-listen-failed";
    return false;  // the socket is closed by the guard
  }

  const std::string redirect_uri = "http://127.0.0.1:" + std::to_string(port) + "/";
  const std::string verifier = PkceCodeVerifier();
  const std::string oauth_state = OAuthStateToken();
  const std::string auth_url = BuildDriveAuthUrl(config.client_id, redirect_uri, verifier, oauth_state);

  // ShellExecuteA is what opens the default browser; a failure here is a real
  // failure, not a transport one, so the chain moves on.
  const HINSTANCE launched =
      ShellExecuteA(nullptr, "open", auth_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(launched) <= 32) {
    *reason = "consent-browser-open-failed";
    return false;
  }
  EmitLog("local-cover drive auth=consent port=" + std::to_string(port));

  const ConsentCallback callback =
      WaitForConsentCallback(listener.get(), oauth_state, consent_deadline_ms);
  listener.Reset();  // stop listening before the token exchange
  if (!callback.ok) {
    *reason = callback.error.empty() ? std::string("consent-failed") : "consent-" + callback.error;
    EmitLog("local-cover drive auth=declined reason=" + *reason);
    return false;
  }
  // The exchange still has to fit the same window. Requiring a full transport
  // budget means the whole consent - wait plus exchange - can never exceed the
  // window, even when the code arrives on its last second.
  if (RemainingMillis(consent_deadline_ms, NowMillis()) <
      static_cast<long long>(kTotalTimeoutMs)) {
    *reason = "consent-timeout";
    EmitLog("local-cover drive auth=declined reason=" + *reason);
    return false;
  }

  std::vector<std::pair<std::string, std::string>> fields;
  fields.emplace_back("code", callback.code);
  fields.emplace_back("client_id", config.client_id);
  fields.emplace_back("client_secret", config.client_secret);
  fields.emplace_back("redirect_uri", redirect_uri);
  fields.emplace_back("grant_type", "authorization_code");
  fields.emplace_back("code_verifier", verifier);
  DriveTokenGrant grant;
  if (!PostDriveToken(fields, &grant, reason)) {
    EmitLog("local-cover drive auth=exchange-failed reason=" + *reason);
    return false;
  }
  if (!IsDriveRefreshToken(grant.refresh_token)) {
    *reason = "consent-no-refresh-token";
    EmitLog("local-cover drive auth=failed reason=" + *reason);
    return false;
  }
  DriveState consented;
  consented.refresh_token = grant.refresh_token;
  consented.folder_id = state->folder_id;  // a re-consent keeps the known folder
  if (!SaveDriveState(config.state_path, consented)) {
    // The token is usable right now; only the next start will ask again.
    EmitLog("local-cover drive auth=consented state=unwritten");
  } else {
    EmitLog("local-cover drive auth=consented");
  }
  *state = consented;
  return true;
}

// The bearer token for this publish: the cached access token while it is fresh,
// otherwise a refresh (or a consent when no usable sidecar exists).
bool DriveAccessToken(const DriveConfig& config, std::string* token, std::string* reason) {
  std::lock_guard<std::mutex> lock(g_drive_session_mutex);
  const long long now = static_cast<long long>(std::time(nullptr));
  if (!g_drive_access_token.empty() &&
      now + kDriveAccessTokenSkewSeconds < g_drive_access_expires_at) {
    *token = g_drive_access_token;
    return true;
  }

  DriveState state;
  if (!LoadDriveState(config.state_path, &state)) {
    const long long tick = static_cast<long long>(GetTickCount64());
    const long long failed_at = g_drive_consent_failed_at.load();
    if (failed_at != 0 && tick - failed_at >= 0 &&
        tick - failed_at < static_cast<long long>(kDriveConsentCooldownMs)) {
      *reason = "drive-not-authorized";
      return false;
    }
    if (!RunDriveConsent(config, &state, reason)) {
      g_drive_consent_failed_at.store(static_cast<long long>(GetTickCount64()));
      return false;
    }
    g_drive_consent_failed_at.store(0);
  }

  std::vector<std::pair<std::string, std::string>> fields;
  fields.emplace_back("client_id", config.client_id);
  fields.emplace_back("client_secret", config.client_secret);
  fields.emplace_back("refresh_token", state.refresh_token);
  fields.emplace_back("grant_type", "refresh_token");
  DriveTokenGrant grant;
  if (!PostDriveToken(fields, &grant, reason)) {
    EmitLog("local-cover drive auth=refresh-failed reason=" + *reason);
    // invalid_grant (400/401) is terminal: forget the sidecar so the next
    // publish re-consents instead of retrying a dead token forever. The
    // cooldown gates that consent, so a broken setup cannot pop a browser on
    // every track.
    if (*reason == "token-http-400" || *reason == "token-http-401") {
      const std::wstring wide = Utils::ToWString(config.state_path);
      if (!wide.empty()) {
        DeleteFileW(wide.c_str());
      }
      g_drive_consent_failed_at.store(static_cast<long long>(GetTickCount64()));
      *reason = "drive-reauth-required";
    }
    return false;
  }
  g_drive_access_token = grant.access_token;
  const long long lifetime = grant.expires_in > 0 ? grant.expires_in : 3600;
  g_drive_access_expires_at = now + lifetime;
  EmitLog("local-cover drive auth=refreshed");
  *token = grant.access_token;
  return true;
}

// The folder id, cached in memory and in the sidecar so the lookup runs once
// ever. A folder that already carries a public-read grant answers an
// already-exists style error, which is success.
bool SaveDriveFolderId(const DriveConfig& config, const std::string& folder_id) {
  DriveState state;
  if (!LoadDriveState(config.state_path, &state)) {
    return false;  // never write a state file without a token
  }
  if (state.folder_id == folder_id) {
    return true;
  }
  state.folder_id = folder_id;
  return SaveDriveState(config.state_path, state);
}

bool EnsureDriveFolder(const DriveConfig& config, const std::string& access_token,
                       std::string* folder_id, std::string* reason) {
  {
    std::lock_guard<std::mutex> lock(g_drive_session_mutex);
    if (LooksLikeDriveFileId(g_drive_folder_id)) {
      *folder_id = g_drive_folder_id;
      return true;
    }
  }
  DriveState state;
  if (LoadDriveState(config.state_path, &state) && LooksLikeDriveFileId(state.folder_id)) {
    std::lock_guard<std::mutex> lock(g_drive_session_mutex);
    g_drive_folder_id = state.folder_id;
    *folder_id = state.folder_id;
    return true;
  }

  const std::wstring headers =
      L"Authorization: Bearer " + Utils::ToWString(access_token) + L"\r\n";
  const HttpResponse list =
      SendHttp(L"GET", L"www.googleapis.com", Utils::ToWString(BuildDriveFolderQuery()), headers,
               std::string());
  if (!list.transport_ok) {
    *reason = list.transport_reason.empty() ? std::string("folder-transport-failed")
                                            : "folder-" + list.transport_reason;
    return false;
  }
  if (list.status != 200) {
    *reason = "folder-http-" + std::to_string(list.status);
    return false;
  }

  std::string id = FirstDriveFileId(list.body);
  if (id.empty()) {
    const HttpResponse created =
        SendHttp(L"POST", L"www.googleapis.com", L"/drive/v3/files?fields=id",
                 headers + L"Content-Type: application/json\r\n", BuildDriveFolderMetadata());
    if (!created.transport_ok) {
      *reason = created.transport_reason.empty() ? std::string("folder-create-transport-failed")
                                                 : "folder-create-" + created.transport_reason;
      return false;
    }
    if (created.status != 200 && created.status != 201) {
      *reason = "folder-create-http-" + std::to_string(created.status);
      return false;
    }
    id = ExtractJsonStringField(created.body, "id");
    if (!LooksLikeDriveFileId(id)) {
      *reason = "folder-create-unexpected-body";
      return false;
    }
  }

  // Public read, once. Children inherit it, so no per-file permission call is
  // ever made. Only an explicit already-exists answer is success; a refusal or a
  // bad request fails the host and leaves the folder id unpersisted, so a later
  // publish retries instead of reusing a folder nobody can fetch from.
  const std::wstring permission_path =
      L"/drive/v3/files/" + Utils::ToWString(id) + L"/permissions";
  const HttpResponse permission =
      SendHttp(L"POST", L"www.googleapis.com", permission_path,
               headers + L"Content-Type: application/json\r\n", BuildDrivePermissionMetadata());
  if (!permission.transport_ok) {
    *reason = permission.transport_reason.empty() ? std::string("permission-transport-failed")
                                                  : "permission-" + permission.transport_reason;
    return false;
  }
  const DrivePermissionVerdict verdict =
      ClassifyDrivePermission(permission.status, permission.body);
  if (verdict == DrivePermissionVerdict::kDenied) {
    EmitLog("local-cover drive folder permission=denied status=" +
            std::to_string(permission.status));
    *reason = "permission-denied";
    return false;
  }
  if (verdict == DrivePermissionVerdict::kRejected) {
    EmitLog("local-cover drive folder permission=rejected status=" +
            std::to_string(permission.status));
    *reason = permission.status == 400 ? std::string("permission-bad-request")
                                       : "permission-http-" + std::to_string(permission.status);
    return false;
  }
  if (verdict == DrivePermissionVerdict::kAlreadyShared) {
    EmitLog("local-cover drive folder permission=already status=" +
            std::to_string(permission.status));
  }

  SaveDriveFolderId(config, id);  // best effort: the memory cache survives the session
  {
    std::lock_guard<std::mutex> lock(g_drive_session_mutex);
    g_drive_folder_id = id;
  }
  *folder_id = id;
  return true;
}

// One Drive upload attempt. Authorisation, folder bootstrap and the upload are
// all inside, because they are one host's round trip; a transport failure on
// the upload itself is the only leg the caller retries (a retried token POST or
// folder create would be wasted, and both are safe to repeat anyway).
UploadAttempt UploadDriveImage(const std::string& name, const std::string& mime,
                               const std::vector<unsigned char>& image) {
  const DriveConfig config = SnapshotDriveConfig();
  if (!DriveConfigured(config)) {
    return AttemptResult(Failure("drive-not-configured"));
  }
  std::string access_token;
  std::string reason;
  if (!DriveAccessToken(config, &access_token, &reason)) {
    return AttemptResult(Failure(reason.empty() ? std::string("drive-auth-failed") : reason));
  }
  std::string folder_id;
  if (!EnsureDriveFolder(config, access_token, &folder_id, &reason)) {
    return AttemptResult(Failure(reason));
  }

  const std::string boundary = MakeBoundary();
  const std::string body =
      BuildDriveMultipartBody(boundary, name, folder_id, SanitizeMime(mime.c_str()), image);
  if (body.size() > 0xffffffffull) {
    return AttemptResult(Failure("body-too-large"));
  }
  const std::wstring headers =
      L"Authorization: Bearer " + Utils::ToWString(access_token) + L"\r\n" +
      L"Content-Type: " + Utils::ToWString(DriveMultipartContentType(boundary)) + L"\r\n";
  const HttpResponse response =
      SendHttp(L"POST", L"www.googleapis.com",
               L"/upload/drive/v3/files?uploadType=multipart&fields=id", headers, body);
  if (!response.transport_ok) {
    UploadAttempt attempt;
    attempt.result = Failure(response.transport_reason.empty()
                                 ? std::string("upload-transport-failed")
                                 : "upload-" + response.transport_reason);
    attempt.transport_failure = true;
    return attempt;
  }
  if (response.status == 0) {
    // The host answered but the status was unreadable: a real answer, so no
    // retry (it may already have created the file).
    return AttemptResult(Failure("upload-" + response.transport_reason));
  }
  return AttemptResult(ClassifyDriveUpload(response.status, response.body));
}

// Dispatches one attempt to the transport that matches the host's shape. The
// GitHub host needs the owner/repo (from the guarded config) and the cover name;
// the keyless hosts build their multipart body here.
UploadAttempt AttemptHostUpload(const UploadHost& upload_host, const PreparedImage& prepared) {
  if (upload_host.shape == UploadShape::kDrive) {
    // The file is named by the hash of the uploaded bytes, matching the cache
    // key and the GitHub host's convention, so the existing cache dedupes.
    return UploadDriveImage(UploadCacheKey(prepared), prepared.mime, prepared.bytes);
  }
  if (upload_host.shape == UploadShape::kGitHub) {
    std::wstring owner;
    std::wstring repo;
    {
      std::lock_guard<std::mutex> lock(g_host_mutex);
      owner = g_github_owner;
      repo = g_github_repo;
    }
    return UploadGitHubBody(owner, repo, UploadCacheKey(prepared), prepared.bytes);
  }
  const std::string content_type = SanitizeMime(prepared.mime.c_str());
  const std::string boundary = MakeBoundary();
  const std::string body = BuildMultipartBody(boundary, content_type, prepared.bytes, upload_host.shape);
  return UploadMultipartBody(upload_host, body, boundary);
}

bool CrackUrl(const std::string& url, std::wstring* host, std::wstring* path) {
  const std::wstring wide = Utils::ToWString(url);
  if (wide.empty()) {
    return false;
  }
  URL_COMPONENTS components = {};
  components.dwStructSize = sizeof(components);
  components.dwSchemeLength = static_cast<DWORD>(-1);
  components.dwHostNameLength = static_cast<DWORD>(-1);
  components.dwUrlPathLength = static_cast<DWORD>(-1);
  components.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &components) == FALSE) {
    return false;
  }
  if (components.nScheme != INTERNET_SCHEME_HTTPS || components.lpszHostName == nullptr ||
      components.dwHostNameLength == 0) {
    return false;
  }
  *host = std::wstring(components.lpszHostName, components.dwHostNameLength);
  std::wstring whole_path;
  if (components.lpszUrlPath != nullptr && components.dwUrlPathLength > 0) {
    whole_path.assign(components.lpszUrlPath, components.dwUrlPathLength);
  }
  if (components.lpszExtraInfo != nullptr && components.dwExtraInfoLength > 0) {
    whole_path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
  }
  if (whole_path.empty()) {
    whole_path = L"/";
  }
  *path = whole_path;
  return true;
}

struct HealthCheck {
  bool ok = false;
  std::string reason;
};

// Proves the upload is actually retrievable: a bare GET (no added headers)
// must answer 200 with an image/* content type.
HealthCheck HealthCheckUrl(const std::string& url) {
  HealthCheck check;
  check.reason = "health-check-failed";
  const Deadline deadline(kTotalTimeoutMs);
  std::wstring host;
  std::wstring path;
  if (!CrackUrl(url, &host, &path)) {
    check.reason = "health-check-crack-url-failed";
    return check;
  }
  WinHttpHandle session = OpenSession(deadline);
  if (!session) {
    check.reason = TransportReason("health-check-session-open-failed", "connect", GetLastError());
    return check;
  }
  WinHttpHandle connection(WinHttpConnect(session.get(), host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
  if (!connection) {
    check.reason = TransportReason("health-check-connect-failed", "connect", GetLastError());
    return check;
  }
  WinHttpHandle request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
  if (!request) {
    check.reason = TransportReason("health-check-request-open-failed", "send", GetLastError());
    return check;
  }
  if (WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) == FALSE) {
    check.reason = TransportReason("health-check-send-failed", "send", GetLastError());
    return check;
  }
  if (WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
    check.reason = TransportReason("health-check-receive-failed", "receive", GetLastError());
    return check;
  }
  DWORD status = 0;
  DWORD status_error = 0;
  if (!QueryStatusCode(request.get(), &status, &status_error)) {
    check.reason = "health-check-status-query-failed phase=receive winhttp=" + std::to_string(status_error);
    return check;
  }
  if (status != 200) {
    check.reason = "health-check-status=" + std::to_string(status);
    return check;
  }
  std::wstring content_type;
  if (!QueryContentType(request.get(), &content_type)) {
    check.reason = "health-check-content-type-missing";
    return check;
  }
  if (!StartsWithImageType(content_type)) {
    check.reason = "health-check-content-type=" + Utils::ToString(content_type);
    return check;
  }
  check.ok = true;
  check.reason.clear();
  return check;
}

// Tries one host: builds that host's own multipart shape, uploads, retries once
// on a transport failure only, and - on success - proves the returned URL is
// retrievable. Records the attempt count and a log marker naming the host. A
// host answer (403/412/empty body) is final for this host; the caller moves on.
struct HostOutcome {
  Result result;
  int attempts = 0;
};

HostOutcome TryUploadHost(const UploadHost& upload_host, const PreparedImage& prepared) {
  HostOutcome outcome;
  UploadAttempt attempt = AttemptHostUpload(upload_host, prepared);
  outcome.attempts = 1;
  if (attempt.transport_failure) {
    Sleep(kRetryBackoffMs);
    attempt = AttemptHostUpload(upload_host, prepared);
    outcome.attempts = 2;
  }
  outcome.result = attempt.result;
  if (outcome.result.ok) {
    const HealthCheck health = HealthCheckUrl(outcome.result.url);
    if (!health.ok) {
      outcome.result = Failure(health.reason);
    }
  }
  outcome.result.host = HostNameUtf8(upload_host);
  outcome.result.attempts = outcome.attempts;
  return outcome;
}

// std::exception::what() can carry arbitrary text; keep a log line single-line
// and short. Image bytes never reach an exception message.
std::string ExceptionText(const char* what) {
  if (what == nullptr) {
    return "unknown";
  }
  std::string text;
  for (const char* c = what; *c != '\0' && text.size() < 120; ++c) {
    text.push_back((*c == '\r' || *c == '\n' || *c == '\t') ? ' ' : *c);
  }
  return text.empty() ? std::string("unknown") : text;
}

// A host attempt that throws must not end the chain: the cause is logged, that
// host counts as failed, and the caller moves on to the next host. Used by the
// fresh chain and by the cache-upgrade legs, so a throw in an upgrade host still
// falls back to revalidating the cached URL.
HostOutcome TryUploadHostGuarded(const UploadHost& upload_host, const PreparedImage& prepared) {
  try {
    return TryUploadHost(upload_host, prepared);
  } catch (const std::exception& error) {
    EmitLog("local-cover host=" + HostNameUtf8(upload_host) + " exception=" +
            ExceptionText(error.what()));
  } catch (...) {
    EmitLog("local-cover host=" + HostNameUtf8(upload_host) + " exception=unknown");
  }
  HostOutcome outcome;
  outcome.result = Failure("exception");
  outcome.result.host = HostNameUtf8(upload_host);
  outcome.attempts = 1;
  return outcome;
}

// Uploads to the permanent GitHub host only. Answers github-not-configured
// without touching the network when the host is not in the chain.
HostOutcome TryGitHubUpload(const PreparedImage& prepared) {
  const std::vector<UploadHost> hosts = ActiveHosts();
  for (size_t i = 0; i < hosts.size(); ++i) {
    if (hosts[i].shape == UploadShape::kGitHub) {
      return TryUploadHostGuarded(hosts[i], prepared);
    }
  }
  HostOutcome outcome;
  outcome.result = Failure("github-not-configured");
  return outcome;
}

// The same for the permanent Drive host; drive-not-configured when the policy
// does not put it in the chain (or its credentials are absent).
HostOutcome TryDriveUpload(const PreparedImage& prepared) {
  const std::vector<UploadHost> hosts = ActiveHosts();
  for (size_t i = 0; i < hosts.size(); ++i) {
    if (hosts[i].shape == UploadShape::kDrive) {
      return TryUploadHostGuarded(hosts[i], prepared);
    }
  }
  HostOutcome outcome;
  outcome.result = Failure("drive-not-configured");
  return outcome;
}

// The permanent hosts the current policy allows a cache upgrade to use, in
// chain order. Read under the same lock as ActiveHosts(), so the cache path and
// a fresh upload can never disagree about the policy.
std::vector<PermanentHost> CurrentUpgradeHosts() {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  return PlanCacheUpgrade(g_cover_host, DriveConfiguredLocked(), GitHubConfiguredLocked());
}

std::string CurrentPolicyName() {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  return PolicyName(g_cover_host);
}

// Re-publishes prepared bytes to the policy's permanent hosts, in chain order,
// stopping at the first success. A keyless policy answers "no-permanent-host"
// without touching the network. The caller revalidates the cached URL when this
// fails, so a dead ephemeral URL is never sent.
HostOutcome UpgradeCachedUrl(const PreparedImage& prepared) {
  const std::vector<PermanentHost> order = CurrentUpgradeHosts();
  HostOutcome outcome;
  outcome.result = Failure("no-permanent-host");
  for (size_t i = 0; i < order.size(); ++i) {
    outcome = order[i] == PermanentHost::kDrive ? TryDriveUpload(prepared)
                                                : TryGitHubUpload(prepared);
    if (outcome.result.ok) {
      return outcome;
    }
    EmitLog("local-cover upgrade-host-failed host=" + outcome.result.host + " reason=" +
            outcome.result.reason);
  }
  return outcome;
}

// Every configured host, in order, in one comma-separated token for the failure
// marker: "github.com,catbox.moe,uguu.se,litter.catbox.moe".
std::string HostListToken() {
  const std::vector<UploadHost> hosts = ActiveHosts();
  std::string token;
  for (size_t i = 0; i < hosts.size(); ++i) {
    if (i != 0) {
      token += ",";
    }
    token += Utils::ToString(hosts[i].name);
  }
  return token;
}

// The publisher's diagnostic sink; never image bytes. One small line per publish
// so a host choice - and the fall-through to online - is greppable in DebugLog.
void (*g_logger)(const std::string&) = nullptr;

void EmitLog(const std::string& line) {
  if (g_logger != nullptr) {
    g_logger(line);
  }
}

// Names the GitHub host's authentication state, never the token: a configured
// token logs "auth=present", an empty one "auth=absent". A 401/403 from GitHub
// logs "auth=rejected" with the status only.
void LogGitHubAuthState() {
  EmitLog(std::string("local-cover github auth=") + (GitHubTokenPresent() ? "present" : "absent"));
}

// Names the Drive host's state, never a token or the secret: whether the host is
// in this chain at all, and (below) why one attempt failed.
void LogDriveHostState() {
  EmitLog(std::string("local-cover drive host=") + (DriveUploadActive() ? "active" : "inactive"));
}

void LogGitHubAuthRejected(const std::string& status) {
  EmitLog("local-cover github auth=rejected status=" + status);
}

void LogHostSuccess(const Result& result) {
  EmitLog("local-cover published host=" + result.host + " url=" + result.url);
}

void LogHostFailure(const std::string& reason, const std::string& host_list, int attempts,
                    const PreparedImage& prepared) {
  EmitLog("local-cover failed host=" + host_list + " reason=" +
          (reason.empty() ? std::string("unknown") : reason) + " attempts=" +
          std::to_string(attempts) + " detail=" + prepared.note);
}

// The publisher is healthy when at least one configured host is reachable - a
// single dead host is never the whole publisher's verdict. Pure, so the rule is
// testable without a network: `reachable` is parallel to the configured hosts.
bool AnyHostHealthy(const bool* reachable, size_t count) {
  if (reachable == nullptr || count == 0) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    if (reachable[i]) {
      return true;
    }
  }
  return false;
}

size_t ConfiguredHostCount() { return ActiveHosts().size(); }

std::string ConfiguredHostName(size_t index) {
  const std::vector<UploadHost> hosts = ActiveHosts();
  return index < hosts.size() ? Utils::ToString(hosts[index].name) : std::string();
}

// The last line of defence: a throw outside one host attempt (image
// preparation, the cache) still logs the reason, records the failure for the
// existing negative TTL and answers the caller. It never rethrows into AIMP's
// thread. The entry is keyed by the source bytes because the prepared bytes may
// not exist; a later success replaces it under the real (prepared) key.
Result PublishException(const std::vector<unsigned char>& image_bytes, const char* what) {
  Result result = Failure("exception");
  try {
    EmitLog("local-cover failed reason=exception text=" + ExceptionText(what));
    const std::string key = Sha256Hex(image_bytes);
    CacheStore(key, result, static_cast<long long>(std::time(nullptr)));
    result.detail = "key=" + key + " attempts=0 host=exception";
  } catch (...) {
    // Nothing further may escape: the caller is AIMP's publisher worker.
  }
  return result;
}

}  // namespace

// --- Public interface -------------------------------------------------------

namespace CoverPublisher {

void SetLogger(void (*logger)(const std::string& line)) {
  g_logger = logger;
}

void Configure(const std::string& cache_path) {
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  g_cache_path = cache_path;
  g_cache.clear();
  if (!cache_path.empty()) {
    LoadCacheLocked();
  }
}

void ConfigureAuth(const std::string& token, const std::string& repo) {
  std::wstring owner;
  std::wstring name;
  const std::wstring configured = repo.empty() ? std::wstring(kDefaultGitHubRepo) : Utils::ToWString(repo);
  ParseOwnerRepo(configured, &owner, &name);
  std::lock_guard<std::mutex> lock(g_host_mutex);
  g_github_token = token;
  g_github_owner = owner;
  g_github_repo = name;
}

void ConfigureHost(const std::string& value) {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  g_cover_host = ParseCoverHostPolicy(value);
}

void LogHostConfiguration() {
  HostConfigSummary summary;
  {
    std::lock_guard<std::mutex> lock(g_host_mutex);
    const bool github_token = !g_github_token.empty();
    const bool github_repo = !g_github_owner.empty() && !g_github_repo.empty();
    summary.policy = g_cover_host;
    summary.drive_usable = DriveConfiguredLocked();
    summary.github_usable = github_token && github_repo;
    summary.drive_reason = DriveUnavailableReason(
        !g_drive_client_id.empty(), !g_drive_client_secret.empty(), !g_drive_state_path.empty());
    summary.github_reason = GitHubUnavailableReason(github_token, github_repo);
  }
  // Host names, the policy and fixed tokens only: no token or secret value,
  // and not even a prefix or a length of one.
  EmitLog(FormatHostConfigLine(summary));
}

void ConfigureDrive(const std::string& client_id, const std::string& client_secret,
                    const std::string& state_path) {
  // A previous configuration's access token and folder id no longer apply, and
  // a cancel left over from an earlier unload is cleared here (this runs at
  // plugin load, before any publish).
  g_drive_consent_cancel.store(false);
  {
    std::lock_guard<std::mutex> session(g_drive_session_mutex);
    g_drive_access_token.clear();
    g_drive_access_expires_at = 0;
    g_drive_folder_id.clear();
  }
  g_drive_consent_failed_at.store(0);
  std::lock_guard<std::mutex> lock(g_host_mutex);
  g_drive_client_id = client_id;
  g_drive_client_secret = client_secret;
  g_drive_state_path = state_path;
}

void CancelDriveConsent() { g_drive_consent_cancel.store(true); }

Result Publish(const std::vector<unsigned char>& image_bytes, const char* mime) {
  try {
    if (image_bytes.empty()) {
      return Failure("empty-image");
    }
    const PreparedImage prepared = PrepareImageForUpload(image_bytes, mime);
    // The key covers exactly the bytes that go on the wire (compressed bytes
    // when compression happened), so the cache can never answer for content the
    // host has not actually seen.
    const std::string key = UploadCacheKey(prepared);
    const long long now = static_cast<long long>(std::time(nullptr));
    Result cached;
    if (CacheLookup(key, now, &cached)) {
      if (!cached.ok) {
        cached.detail = prepared.note + " key=" + key + " cache=hit";
        return cached;
      }
      // A hit on an ephemeral keyless URL is never trusted blindly: with a
      // permanent host selected by the policy (Drive and/or GitHub, in chain
      // order) the local bytes are re-published there and the cache is updated;
      // with none the stored URL is revalidated and a dead entry falls through
      // to a fresh keyless upload below, which overwrites it. Either way a dead
      // cached URL is never sent, and CoverHost=keyless never reaches an
      // authenticated host.
      const std::vector<PermanentHost> upgrade_hosts = CurrentUpgradeHosts();
      const CachedUrlAction action = PlanCachedUrl(cached.url, !upgrade_hosts.empty());
      if (action == CachedUrlAction::kUse) {
        cached.detail = prepared.note + " key=" + key + " cache=hit";
        return cached;
      }
      if (action == CachedUrlAction::kUpgradeToPermanent) {
        const HostOutcome upgrade = UpgradeCachedUrl(prepared);
        if (upgrade.result.ok) {
          Result result = upgrade.result;
          LogHostSuccess(result);
          result.detail = prepared.note + " key=" + key +
                          " cache=upgraded-from-ephemeral attempts=" +
                          std::to_string(upgrade.attempts) + " host=" + result.host;
          CacheStore(key, result, now);
          return result;
        }
        EmitLog("local-cover upgrade-failed key=" + key + " reason=" +
                upgrade.result.reason + " host=" + upgrade.result.host);
        // Fall through to revalidation: a failed upgrade must not resurrect a
        // possibly-dead ephemeral URL.
      } else {
        // No permanent host is in the chain for this policy: say so, with the
        // policy, so a cache hit that never tries Drive or GitHub is explained
        // by one line instead of an absence of lines.
        EmitLog("local-cover cache-upgrade-skipped key=" + key +
                " reason=no-permanent-host policy=" + CurrentPolicyName());
      }
      const HealthCheck health = HealthCheckUrl(cached.url);
      if (health.ok) {
        cached.detail = prepared.note + " key=" + key + " cache=hit-revalidated";
        return cached;
      }
      EmitLog("local-cover cache-stale key=" + key + " reason=" + health.reason);
    }

    // Ordered fallback inside one publish: the policy's preferred host first
    // (Drive and/or GitHub), then catbox, then uguu, then the dormant
    // litterbox. All are the same layer - a host failure never skips the online
    // rung. Each host is retried once on a transport failure only, and a host
    // that throws is logged and skipped rather than ending the chain.
    LogGitHubAuthState();
    LogDriveHostState();
    const std::vector<UploadHost> hosts = ActiveHosts();
    Result result;
    int total_attempts = 0;
    for (size_t i = 0; i < hosts.size(); ++i) {
      const HostOutcome outcome = TryUploadHostGuarded(hosts[i], prepared);
      total_attempts += outcome.attempts;
      if (!outcome.result.ok && hosts[i].shape == UploadShape::kGitHub) {
        const std::string& reason = outcome.result.reason;
        if (reason == "http-401" || reason == "http-403") {
          LogGitHubAuthRejected(reason.substr(5));
        }
      }
      if (!outcome.result.ok && hosts[i].shape == UploadShape::kDrive) {
        // Every Drive failure names a reason and then falls through to the
        // next host exactly as any other host failure does.
        EmitLog("local-cover drive failed reason=" +
                (outcome.result.reason.empty() ? std::string("unknown") : outcome.result.reason));
      }
      if (outcome.result.ok) {
        result = outcome.result;
        break;
      }
      result = outcome.result;  // last failure names the final host
    }
    if (result.ok) {
      LogHostSuccess(result);
    } else {
      LogHostFailure(result.reason, HostListToken(), total_attempts, prepared);
    }
    result.detail = prepared.note + " key=" + key + " attempts=" + std::to_string(total_attempts) +
                    " host=" + (result.host.empty() ? HostListToken() : result.host);
    CacheStore(key, result, now);
    return result;
  } catch (const std::exception& error) {
    return PublishException(image_bytes, error.what());
  } catch (...) {
    return PublishException(image_bytes, "unknown");
  }
}

void ClearCache() {
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  g_cache.clear();
  if (!g_cache_path.empty()) {
    const std::wstring wide = Utils::ToWString(g_cache_path);
    if (!wide.empty()) {
      DeleteFileW(wide.c_str());
    }
  }
}

}  // namespace CoverPublisher
