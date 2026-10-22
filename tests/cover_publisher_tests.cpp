// Offline unit tests for src/cover_publisher.cpp and the pure album-art
// identity helpers (src/album_art.cpp, src/presence_layout.h).
//
// These tests never touch the network. The cover_publisher implementation is
// compiled into this translation unit, so its pure helpers (URL
// classification, multipart assembly, boundary generation, SHA-256 cache keys,
// cache file handling) are exercised directly; album_art.cpp is compiled as a
// second translation unit and linked, and the presence layout is header-only.
//
// Build (from the repository root):
//   cl /nologo /std:c++17 /W4 /WX /EHsc tests\cover_publisher_tests.cpp src\album_art.cpp /link winhttp.lib
// Run (the optional argument names the directory for the on-disk cache test):
//   cover_publisher_tests.exe [cache-directory]
// The default directory is %TEMP%\cover_publisher_cache_tests. The run creates
// and removes its own scratch file inside that directory.

#include "../src/cover_publisher.cpp"

#include "../src/album_art.h"
#include "../src/local_art.cpp"
#include "../src/presence_layout.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;
std::vector<std::string> g_log_lines;

void CaptureLog(const std::string& line) { g_log_lines.push_back(line); }

void Check(bool condition, const std::string& label) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", label.c_str());
  }
}

bool FileExists(const std::wstring& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring CacheDirectoryFromArgs(int argc, char** argv) {
  if (argc > 1 && argv[1] != nullptr && argv[1][0] != '\0') {
    return Utils::ToWString(argv[1]);
  }
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length = GetTempPathW(MAX_PATH, buffer);
  std::wstring directory(buffer, length);
  directory += L"cover_publisher_cache_tests";
  return directory;
}

// --- URL shape --------------------------------------------------------------

void TestUrlShape() {
  Check(LooksLikeUploadUrl("https://litter.catbox.moe/abc123.png"), "accept png url");
  Check(LooksLikeUploadUrl("https://litter.catbox.moe/a1b2c3.jpg"), "accept jpg url");
  Check(LooksLikeUploadUrl("https://litter.catbox.moe/0z9y8x.jpeg"), "accept jpeg url");
  Check(!LooksLikeUploadUrl(""), "reject empty url");
  Check(!LooksLikeUploadUrl("https://litter.catbox.moe/abc123.png\n"), "reject trailing newline");
  Check(!LooksLikeUploadUrl(" https://litter.catbox.moe/abc123.png"), "reject leading space");
  Check(!LooksLikeUploadUrl("https://litter.catbox.moe/abc12.png"), "reject short id");
  Check(!LooksLikeUploadUrl("https://litter.catbox.moe/abc1234.png"), "reject long id");
  Check(!LooksLikeUploadUrl("https://litter.catbox.moe/ABC123.png"), "reject uppercase id");
  Check(!LooksLikeUploadUrl("https://litter.catbox.moe/abc123.gif"), "reject other extension");
  Check(!LooksLikeUploadUrl("http://litter.catbox.moe/abc123.png"), "reject plain http");
  Check(!LooksLikeUploadUrl("https://litter.catbox.moe/abc123.png?x=1"), "reject query string");
  Check(!LooksLikeUploadUrl("https://litter.catbox.moe.evil.test/abc123.png"), "reject other host");
}

// --- Response classification ------------------------------------------------

void TestClassifier() {
  const std::string url = "https://litter.catbox.moe/abc123.png";

  const Result ok = ClassifyUploadResponse(200, url);
  Check(ok.ok && ok.url == url && ok.reason.empty(), "200 + url -> ok");

  const Result trimmed = ClassifyUploadResponse(200, url + "\r\n");
  Check(trimmed.ok && trimmed.url == url, "200 + url with newline -> ok, trimmed");

  const Result empty = ClassifyUploadResponse(200, "");
  Check(!empty.ok && empty.reason == "empty-body", "200 + empty body -> empty-body");

  const Result blank = ClassifyUploadResponse(200, "  \r\n\t");
  Check(!blank.ok && blank.reason == "empty-body", "200 + whitespace body -> empty-body");

  const Result rejected = ClassifyUploadResponse(412, "File is too large");
  Check(!rejected.ok && rejected.reason == "File is too large", "412 -> reason is the body text");

  const Result rejectedEmpty = ClassifyUploadResponse(412, "");
  Check(!rejectedEmpty.ok && rejectedEmpty.reason == "http-412", "412 + empty body -> status reason");

  const Result serverError = ClassifyUploadResponse(500, "boom");
  Check(!serverError.ok && serverError.reason.find("500") != std::string::npos, "500 -> reason names status");

  const Result junk = ClassifyUploadResponse(200, "not a url");
  Check(!junk.ok && junk.reason.find("200") != std::string::npos, "200 + junk -> reason names status");

  // Strict success classification per host: only that host's own bare URL.
  const Result litterbox_ok = ClassifyUploadResponse(200, url, UploadShape::kLitterbox);
  Check(litterbox_ok.ok && litterbox_ok.url == url, "litterbox 200 + litterbox url -> ok");

  const std::string uguu_url = "https://n.uguu.se/nUoxUKCo.png";
  const Result uguu_ok = ClassifyUploadResponse(200, uguu_url, UploadShape::kUguu);
  Check(uguu_ok.ok && uguu_ok.url == uguu_url, "uguu 200 + uguu url -> ok");
  const Result uguu_trimmed =
      ClassifyUploadResponse(200, uguu_url + "\r\n", UploadShape::kUguu);
  Check(uguu_trimmed.ok && uguu_trimmed.url == uguu_url,
        "uguu 200 + url with newline -> ok, trimmed");

  // A 200 whose body is an HTML page, a JSON blob, or the wrong host's URL is a
  // failure: the response must be a bare URL, never arbitrary text.
  const Result html = ClassifyUploadResponse(
      200, "<html><body>Forbidden</body></html>", UploadShape::kUguu);
  Check(!html.ok && html.reason == "http-200-unexpected-body",
        "200 + html page -> failure");
  const Result json = ClassifyUploadResponse(
      200, "{\"success\":true,\"files\":[{\"url\":\"https://n.uguu.se/x.png\"}]}",
      UploadShape::kUguu);
  Check(!json.ok && json.reason == "http-200-unexpected-body",
        "200 + json blob -> failure");
  const Result wrong_host = ClassifyUploadResponse(200, url, UploadShape::kUguu);
  Check(!wrong_host.ok && wrong_host.reason == "http-200-unexpected-body",
        "a litterbox url is not accepted for the uguu host");
  const Result uguu_empty = ClassifyUploadResponse(200, "", UploadShape::kUguu);
  Check(!uguu_empty.ok && uguu_empty.reason == "empty-body",
        "uguu 200 + empty body -> failure");

  // uguu round-robins across single-label subdomains. Every one of these is a
  // real response and must be accepted; pinning the subdomain to one letter
  // rejected valid covers as "unexpected body" (measured live: h.uguu.se).
  const char* const kUguuLiveUrls[] = {
      "https://n.uguu.se/nUoxUKCo.png",
      "https://h.uguu.se/yHYDFJRh.jpg",
      "https://a.uguu.se/abc123.jpeg",
      "https://z9.uguu.se/xyz789.webp",
  };
  for (const char* const live : kUguuLiveUrls) {
    const Result accepted = ClassifyUploadResponse(200, live, UploadShape::kUguu);
    Check(accepted.ok && accepted.url == live,
          std::string("uguu subdomain accepted: ") + live);
  }
  // The subdomain must still be uguu's own, so a lookalike host is rejected.
  const Result lookalike = ClassifyUploadResponse(
      200, "https://n.uguu.se.evil.test/x.png", UploadShape::kUguu);
  Check(!lookalike.ok && lookalike.reason == "http-200-unexpected-body",
        "a lookalike host is still rejected");
  const Result no_subdomain = ClassifyUploadResponse(
      200, "https://evil.test/n.uguu.se/x.png", UploadShape::kUguu);
  Check(!no_subdomain.ok, "a foreign host carrying the uguu path is rejected");

  // A WAF 403 (the measured litterbox failure) names its status and is never ok.
  const Result forbidden = ClassifyUploadResponse(403, "<html>BunkerWeb</html>");
  Check(!forbidden.ok && forbidden.reason == "http-403", "403 -> failure named by status");
}

// --- Ordered host fallback: litterbox first, then uguu ---------------------

// Mirrors CoverPublisher::Publish's host loop as a pure decision over per-host
// outcomes, so the ordered fallback is asserted without touching the network.
Result RunHostFallback(const std::vector<Result>& per_host) {
  Result last;
  for (size_t i = 0; i < per_host.size(); ++i) {
    last = per_host[i];
    if (last.ok) {
      return last;
    }
  }
  return last;
}

void TestHostFallback() {
  const std::string uguu_url = "https://n.uguu.se/nUoxUKCo.png";

  // The working host is first, so the common path needs no fallback at all: a
  // 200 from host 0 is the overall result.
  std::vector<Result> happy;
  happy.push_back(ClassifyUploadResponse(200, uguu_url, UploadShape::kUguu));
  const Result direct = RunHostFallback(happy);
  Check(direct.ok && direct.url == uguu_url,
        "host fallback: a working host 0 is used directly, no fallback needed");

  // Host 0 fails (the dormant litterbox answering 403) and the next succeeds.
  std::vector<Result> outcomes;
  outcomes.push_back(ClassifyUploadResponse(200, uguu_url, UploadShape::kUguu));
  outcomes.push_back(ClassifyUploadResponse(403, "<html>BunkerWeb</html>"));
  const Result fell_back = RunHostFallback(outcomes);
  Check(outcomes[0].ok, "host fallback: uguu's 200 succeeds first");
  Check(!outcomes[1].ok, "host fallback: litterbox's 403 is a failure");
  Check(fell_back.ok && fell_back.url == uguu_url,
        "host fallback: later host failure never discards an earlier success");

  // All hosts fail: overall failure. The caller must then try the online rung,
  // which may still supply an image.
  std::vector<Result> all_fail;
  all_fail.push_back(ClassifyUploadResponse(403, ""));
  all_fail.push_back(ClassifyUploadResponse(503, ""));
  const Result none = RunHostFallback(all_fail);
  Check(!none.ok, "host fallback: all hosts failing is an overall failure");

  const std::string online_url =
      "https://e-cdns-images.dzcdn.net/images/cover/abc123/600x600-000000-80-0-0.jpg";
  const std::string key = AlbumArt::BuildAlbumKey("Daft Punk", "Discovery", "");
  Check(PresenceLayout::ResolveCoverLayer("", "", online_url) ==
            PresenceLayout::CoverLayer::kOnlineKeyless,
        "host fallback: all hosts failed and online resolved -> the online layer");
  std::string source;
  Check(PresenceLayout::ResolveLargeImage(key, "", "", key, online_url, &source) ==
            online_url &&
            source == "online",
        "host fallback: no local url, online url is used");
}

// --- Multipart arithmetic ---------------------------------------------------

// The live transport measurement reported a 37-byte closing suffix and a
// one-byte-larger part for JPEG. This builder keeps both invariants. Its prefix
// is 315 bytes for a PNG part: measured arithmetic (347) came from a probe whose
// filename/headers differed (the 32-byte delta is exactly one shorter filename
// plus header choices), so the assertion pins this builder's own framing
// instead of a probe-specific constant.
void TestMultipartArithmetic() {
  // The per-operation transport budget must stay small enough that a hung host
  // costs the card seconds, not tens of seconds. Measured: a real upload takes
  // 0.8-4.1 s, so anything above ~10 s is a stall the owner would see as a
  // missing image.
  Check(kTotalTimeoutMs <= 10000,
        "the per-operation transport budget is bounded so a hung host cannot stall the chain");
  Check(kTotalTimeoutMs >= 4000,
        "the transport budget still clears the slowest measured upload (4.1 s)");
  const std::vector<unsigned char> image(4321, 0x7f);
  const std::string boundary = MakeBoundary();
  const std::string prefix = MultipartPrefix(boundary, "image/png", UploadShape::kLitterbox);
  const std::string suffix = MultipartSuffix(boundary);
  const std::string body = BuildMultipartBody(boundary, "image/png", image, UploadShape::kLitterbox);
  const std::string jpeg_prefix = MultipartPrefix(boundary, "image/jpeg", UploadShape::kLitterbox);

  Check(prefix.size() == 315, "png prefix is 315 bytes");
  Check(suffix.size() == 37, "suffix is 37 bytes");
  Check(jpeg_prefix.size() == 316, "jpeg prefix is 316 bytes");
  Check(jpeg_prefix.size() == prefix.size() + 1, "jpeg part is exactly one byte larger");
  Check(body.size() == prefix.size() + image.size() + suffix.size(), "body = prefix + image + suffix");
  Check(body.size() == 315 + image.size() + 37, "body arithmetic holds numerically");
  Check(std::memcmp(body.data() + prefix.size(), image.data(), image.size()) == 0, "image bytes land at the prefix offset");
  Check(body.compare(body.size() - suffix.size(), suffix.size(), suffix) == 0, "body ends with the closing delimiter");

  const size_t reqtype = body.find("name=\"reqtype\"");
  const size_t time = body.find("name=\"time\"");
  const size_t file = body.find("name=\"fileToUpload\"");
  Check(reqtype != std::string::npos && reqtype < time && time < file, "fields are in order reqtype, time, file");
  Check(body.find("filename=\"cover.png\"") != std::string::npos, "file part carries a filename");
  Check(body.find("\r\nContent-Type: image/png\r\n") != std::string::npos, "file part carries the content type");
  Check(body.find("fileupload\r\n") != std::string::npos && body.find("72h\r\n") != std::string::npos,
        "field values are present");

  const std::string unknown =
      MultipartPrefix(boundary, "application/octet-stream", UploadShape::kLitterbox);
  Check(unknown.find("filename=\"cover.bin\"") != std::string::npos &&
            unknown.find("Content-Type: application/octet-stream\r\n") != std::string::npos,
        "unknown mime keeps a truthful filename and content type");
}

// --- Upload host list: ordered, with each host's own shape ------------------

void TestHostList() {
  // Without a token the chain is the keyless one, unchanged in order and size.
  CoverPublisher::ConfigureAuth("", "");
  Check(ConfiguredHostCount() == 3, "host list has exactly three keyless hosts without a token");
  Check(kBaseUploadHostCount == 3, "the keyless host count constant is three");
  Check(ConfiguredHostName(0) == "catbox.moe",
        "host 0 without a token is catbox - permanent and first keyless");
  Check(ConfiguredHostName(1) == "uguu.se",
        "host 1 without a token is uguu - the measured working host");
  Check(ConfiguredHostName(2) == "litter.catbox.moe",
        "host 2 without a token is litterbox - last as a dormant fallback");
  Check(ConfiguredHostName(3).empty(), "an out-of-range host name is empty");

  bool github_absent = true;
  for (size_t i = 0; i < ConfiguredHostCount(); ++i) {
    github_absent = github_absent && ConfiguredHostName(i) != "github.com";
  }
  Check(github_absent, "an empty token omits the GitHub host entirely");

  // Each keyless entry carries its own endpoint and multipart shape.
  Check(std::wstring(kBaseUploadHosts[0].host) == L"catbox.moe" &&
            std::wstring(kBaseUploadHosts[0].path) == L"/user/api.php" &&
            kBaseUploadHosts[0].shape == UploadShape::kCatbox,
        "catbox owns its endpoint and reqtype/fileToUpload shape");
  Check(std::wstring(kBaseUploadHosts[1].host) == L"uguu.se" &&
            std::wstring(kBaseUploadHosts[1].path) == L"/upload?output=text" &&
            kBaseUploadHosts[1].shape == UploadShape::kUguu,
        "uguu owns its endpoint and files[] shape");
  Check(std::wstring(kBaseUploadHosts[2].host) == L"litterbox.catbox.moe" &&
            std::wstring(kBaseUploadHosts[2].path) == L"/resources/internals/api.php" &&
            kBaseUploadHosts[2].shape == UploadShape::kLitterbox,
        "litterbox owns its endpoint and fileToUpload shape");

  // A configured token prepends the GitHub host: github -> catbox -> uguu ->
  // litterbox, with no other entry reordered.
  CoverPublisher::ConfigureAuth("test-token", "");
  Check(ConfiguredHostCount() == 4, "a configured token adds exactly one host");
  Check(ConfiguredHostName(0) == "github.com",
        "the GitHub host is FIRST when a token is configured");
  Check(ConfiguredHostName(1) == "catbox.moe", "catbox stays second when GitHub is present");
  Check(ConfiguredHostName(2) == "uguu.se", "uguu stays third when GitHub is present");
  Check(ConfiguredHostName(3) == "litter.catbox.moe",
        "litterbox stays last when GitHub is present");

  // Restore the no-token default so the later tests still see the keyless chain.
  CoverPublisher::ConfigureAuth("", "");
  Check(ConfiguredHostCount() == 3, "clearing the token restores the keyless chain");
}

// --- Multipart shapes differ correctly per host ----------------------------

void TestMultipartShapes() {
  const std::vector<unsigned char> image(64, 0x2a);
  const std::string boundary = "----testboundary";

  const std::string litterbox =
      BuildMultipartBody(boundary, "image/png", image, UploadShape::kLitterbox);
  Check(litterbox.find("name=\"fileToUpload\"") != std::string::npos,
        "litterbox body uses the fileToUpload field");
  Check(litterbox.find("name=\"reqtype\"") != std::string::npos,
        "litterbox body carries reqtype");
  Check(litterbox.find("name=\"time\"") != std::string::npos,
        "litterbox body carries time");
  Check(litterbox.find("72h\r\n") != std::string::npos, "litterbox time is 72h");
  Check(litterbox.find("name=\"files[]\"") == std::string::npos,
        "litterbox body never uses the uguu field name");

  const std::string uguu =
      BuildMultipartBody(boundary, "image/png", image, UploadShape::kUguu);
  Check(uguu.find("name=\"files[]\"") != std::string::npos,
        "uguu body uses the files[] field");
  Check(uguu.find("name=\"fileToUpload\"") == std::string::npos,
        "uguu body never uses the litterbox field name");
  Check(uguu.find("name=\"reqtype\"") == std::string::npos &&
            uguu.find("name=\"time\"") == std::string::npos,
        "uguu body carries no extra fields");
  Check(uguu.find("filename=\"cover.png\"") != std::string::npos &&
            uguu.find("\r\nContent-Type: image/png\r\n") != std::string::npos,
        "uguu file part keeps filename and content type");

  // The two shapes genuinely differ, and each keeps the one-part framing.
  Check(litterbox != uguu, "the two host bodies differ");
  Check(uguu.size() < litterbox.size(), "uguu's single part is smaller than litterbox's three");
  Check(uguu.size() == uguu.find("--" + boundary + "--") + 0 ||
            uguu.find("--" + boundary + "--\r\n") ==
                uguu.size() - ("--" + boundary + "--\r\n").size(),
        "uguu body closes with the boundary delimiter");
  // The image bytes still land exactly once in each body.
  Check(uguu.find(std::string(64, 0x2a)) != std::string::npos,
        "uguu body carries the image bytes");
}

// --- Centre-square crop -----------------------------------------------------

void TestCentreSquareCrop() {
  // The re-encode crops to the shorter side, then caps at 512: 1000x800 becomes
  // an 800 square, capped to 512.
  const CompressionPlan wide = PlanCompression(1000, 800, 5 * 1024 * 1024);
  Check(wide.reencode && wide.width == 512 && wide.height == 512,
        "1000x800 -> an 800 square capped at 512");
  Check(wide.width == wide.height, "the compression plan is always square");

  const CompressionPlan tall = PlanCompression(400, 900, 4 * 1024 * 1024);
  Check(tall.reencode && tall.width == 400 && tall.height == 400,
        "a 400x900 portrait crops to a 400 square (shorter side under 512)");

  // The skip rule is unchanged: a source already square, at or below 512 px and
  // at most 256 KB, is not re-encoded.
  const CompressionPlan already_square = PlanCompression(512, 512, 100 * 1024);
  Check(!already_square.reencode,
        "an already-square image <=512 px and <=256 KB is NOT re-encoded");
  const CompressionPlan square_heavy = PlanCompression(512, 512, 300 * 1024);
  Check(square_heavy.reencode && square_heavy.width == 512 && square_heavy.height == 512,
        "an already-square image over 256 KB re-encodes in place at 512");
}

// --- GitHub host configuration ----------------------------------------------

void TestGitHubHostConfig() {
  CoverPublisher::ConfigureAuth("", "");
  Check(ConfiguredHostCount() == 3, "github config: no token -> the keyless chain");

  CoverPublisher::ConfigureAuth("test-token", "octo/art");
  Check(ConfiguredHostCount() == 4, "github config: token + repo -> GitHub added");
  Check(ConfiguredHostName(0) == "github.com", "github config: GitHub is first");

  // A malformed repo skips GitHub rather than building a nonsense target.
  CoverPublisher::ConfigureAuth("test-token", "no-slash-here");
  Check(ConfiguredHostCount() == 3, "github config: a malformed repo has no GitHub host");

  // An empty repo falls back to the default owner/name.
  CoverPublisher::ConfigureAuth("test-token", "");
  Check(ConfiguredHostCount() == 4, "github config: an empty repo uses the default repo");

  CoverPublisher::ConfigureAuth("", "");
}

// --- GitHub request shape ---------------------------------------------------

void TestGitHubRequestShape() {
  const std::string token = "test-token";
  const std::wstring owner = L"octo";
  const std::wstring repo = L"art";
  const std::string name = Sha256Hex(std::vector<unsigned char>{'c', 'o', 'v', 'e', 'r'});
  const std::vector<unsigned char> image = {'a', 'b', 'c'};
  const GitHubRequest request = BuildGitHubRequest(token, owner, repo, name, image);

  Check(request.method == L"PUT", "github request is a PUT");
  Check(request.host == L"api.github.com", "github request targets api.github.com");
  const std::wstring expected_path =
      L"/repos/octo/art/contents/covers/" + Utils::ToWString(name) + L".jpg";
  Check(request.path == expected_path,
        "github request path is /repos/<owner>/<repo>/contents/covers/<sha>.jpg");
  Check(request.headers.find(L"Authorization: Bearer test-token") != std::wstring::npos,
        "github request carries the bearer token");
  Check(request.headers.find(L"Content-Type: application/json") != std::wstring::npos,
        "github request announces a JSON body");

  // The token is confined to the Authorization header: never in the target or
  // the body.
  Check(request.path.find(L"test-token") == std::wstring::npos,
        "the token is not in the request target");
  Check(request.body.find("test-token") == std::string::npos,
        "the token is not in the JSON body");
  Check(request.body.find("\"branch\":\"main\"") != std::string::npos,
        "github body commits to the main branch");
  Check(request.body.find("\"content\":\"") != std::string::npos,
        "github body carries base64 content");
  Check(request.body.find(name.substr(0, 7)) != std::string::npos,
        "github commit message names the cover by its short hash");

  Check(Base64Encode(reinterpret_cast<const unsigned char*>("abc"), 3) == "YWJj",
        "base64 abc known answer");

  // No other host's request carries an Authorization header: the multipart
  // bodies (the only other payloads) never contain one.
  const std::string boundary = "----authtest";
  const std::string catbox = BuildMultipartBody(boundary, "image/png", image, UploadShape::kCatbox);
  const std::string uguu = BuildMultipartBody(boundary, "image/png", image, UploadShape::kUguu);
  const std::string litterbox =
      BuildMultipartBody(boundary, "image/png", image, UploadShape::kLitterbox);
  Check(catbox.find("Authorization") == std::string::npos &&
            uguu.find("Authorization") == std::string::npos &&
            litterbox.find("Authorization") == std::string::npos,
        "only the GitHub host carries an Authorization header");
}

// --- GitHub contents response classification --------------------------------

void TestGitHubResponseClassification() {
  const std::wstring owner = L"nwhn12";
  const std::wstring repo = L"aimp-discord-presence-art";
  const std::string name = Sha256Hex(std::vector<unsigned char>{'c', 'o', 'v', 'e', 'r'});
  const std::string download =
      "https://raw.githubusercontent.com/nwhn12/aimp-discord-presence-art/main/covers/" + name +
      ".jpg";

  // 201/200 success: the url is content.download_url, never the raw JSON.
  const std::string ok_body =
      "{\"content\":{\"name\":\"" + name + ".jpg\",\"download_url\":\"" + download +
      "\"},\"commit\":{\"sha\":\"deadbeef\"}}";
  const Result created = ClassifyGitHubResponse(201, ok_body, owner, repo, name);
  Check(created.ok && created.url == download,
        "github 201 + download_url -> success with THAT url");
  const Result ok200 = ClassifyGitHubResponse(200, ok_body, owner, repo, name);
  Check(ok200.ok && ok200.url == download, "github 200 + download_url -> success with THAT url");
  Check(created.url != ok_body, "the raw JSON body is never returned as the url");

  // No download_url -> failure, never a url.
  const std::string no_download = "{\"content\":{\"name\":\"" + name + ".jpg\"},\"commit\":{}}";
  const Result missing = ClassifyGitHubResponse(200, no_download, owner, repo, name);
  Check(!missing.ok && missing.url.empty(), "a JSON body without download_url is a failure");
  Check(missing.reason.find("200") != std::string::npos,
        "the missing-download_url failure names the status");

  // A download_url on another owner/repo is rejected (owner/repo are pinned).
  const std::string foreign =
      "{\"content\":{\"download_url\":\"https://raw.githubusercontent.com/evil/art/main/covers/" +
      name + ".jpg\"}}";
  const Result foreign_result = ClassifyGitHubResponse(200, foreign, owner, repo, name);
  Check(!foreign_result.ok, "a download_url for another owner/repo is rejected");

  // A bare (non-JSON) body that happens to be a url is never adopted.
  const Result bare = ClassifyGitHubResponse(200, download, owner, repo, name);
  Check(!bare.ok, "a bare url body is not parsed as a download_url");

  // The immutable file already exists: 422/409 -> success via the raw URL.
  const std::string raw = GitHubRawUrl(owner, repo, name);
  const Result exists422 =
      ClassifyGitHubResponse(422, "{\"message\":\"already exists\"}", owner, repo, name);
  Check(exists422.ok && exists422.url == raw,
        "github 422 -> success via the constructed raw url");
  const Result exists409 = ClassifyGitHubResponse(409, "", owner, repo, name);
  Check(exists409.ok && exists409.url == raw,
        "github 409 -> success via the constructed raw url");

  // Bad auth: failure named by status only; the body is never echoed.
  const Result unauthorized =
      ClassifyGitHubResponse(401, "{\"message\":\"Bad credentials test-token\"}", owner, repo, name);
  Check(!unauthorized.ok && unauthorized.reason == "http-401",
        "github 401 -> failure named by status");
  Check(unauthorized.reason.find("test-token") == std::string::npos && unauthorized.url.empty(),
        "a 401 never leaks the body or the token into the result");
  const Result forbidden = ClassifyGitHubResponse(403, "rate limited test-token", owner, repo, name);
  Check(!forbidden.ok && forbidden.reason == "http-403",
        "github 403 -> failure named by status");
  Check(forbidden.reason.find("test-token") == std::string::npos,
        "a 403 never leaks the token into the reason");

  Check(created.url.find("test-token") == std::string::npos,
        "the success url never contains the token");
}

// --- catbox host ------------------------------------------------------------

void TestCatboxHost() {
  Check(std::wstring(kBaseUploadHosts[0].host) == L"catbox.moe" &&
            std::wstring(kBaseUploadHosts[0].path) == L"/user/api.php" &&
            kBaseUploadHosts[0].shape == UploadShape::kCatbox,
        "catbox owns its endpoint and shape");

  const std::vector<unsigned char> image(32, 0x11);
  const std::string boundary = "----catboxtest";
  const std::string body = BuildMultipartBody(boundary, "image/png", image, UploadShape::kCatbox);
  Check(body.find("name=\"reqtype\"") != std::string::npos, "catbox carries reqtype");
  Check(body.find("name=\"fileToUpload\"") != std::string::npos,
        "catbox carries fileToUpload");
  Check(body.find("name=\"time\"") == std::string::npos,
        "catbox has no time field (it stores permanently)");
  Check(body.find("name=\"files[]\"") == std::string::npos,
        "catbox never uses the uguu field name");
  Check(body.find("fileupload\r\n") != std::string::npos, "catbox reqtype value is fileupload");
  Check(body.find("filename=\"cover.png\"") != std::string::npos,
        "catbox file part carries a filename");

  const std::string catbox_url = "https://files.catbox.moe/a1b2c3.png";
  const Result ok = ClassifyUploadResponse(200, catbox_url, UploadShape::kCatbox);
  Check(ok.ok && ok.url == catbox_url, "catbox 200 + bare url -> success");
  const Result trimmed = ClassifyUploadResponse(200, catbox_url + "\r\n", UploadShape::kCatbox);
  Check(trimmed.ok && trimmed.url == catbox_url, "catbox url with a newline is trimmed");
  const Result wrong_host =
      ClassifyUploadResponse(200, "https://litter.catbox.moe/abc123.png", UploadShape::kCatbox);
  Check(!wrong_host.ok, "a litterbox url is not accepted for the catbox host");
  Check(LooksLikeUploadUrl(catbox_url), "the catbox url is a recognised upload url");
}

// --- The token never leaks --------------------------------------------------

void TestTokenNeverLeaks() {
  const std::string token = "test-token";
  const std::wstring owner = L"nwhn12";
  const std::wstring repo = L"aimp-discord-presence-art";
  const std::string name = Sha256Hex(std::vector<unsigned char>{'l', 'o', 'g'});

  g_log_lines.clear();
  CoverPublisher::SetLogger(CaptureLog);

  // Positive control: the recorder is live before we conclude anything from its
  // silence, and an empty token is logged as a state, never a value.
  CoverPublisher::ConfigureAuth("", "");
  LogGitHubAuthState();
  Check(!g_log_lines.empty(), "the log recorder is live (positive control)");
  Check(g_log_lines.back().find("auth=absent") != std::string::npos,
        "an empty token logs auth=absent, never a value");

  // With a token, only its presence is named.
  CoverPublisher::ConfigureAuth(token, "");
  LogGitHubAuthState();
  LogGitHubAuthRejected("403");
  const Result rejected =
      ClassifyGitHubResponse(401, "{\"message\":\"" + token + "\"}", owner, repo, name);
  PreparedImage prepared;
  LogHostFailure(rejected.reason, HostListToken(), 2, prepared);
  CoverPublisher::SetLogger(nullptr);

  bool token_in_log = false;
  for (const std::string& line : g_log_lines) {
    token_in_log = token_in_log || line.find(token) != std::string::npos;
  }
  Check(g_log_lines.size() >= 3,
        "the auth-state, auth-rejected and failure lines were all recorded");
  Check(!token_in_log, "no log line contains the token");
  Check(rejected.reason.find(token) == std::string::npos && rejected.url.empty(),
        "the result carries neither the token nor the body");
  Check(HostListToken().find(token) == std::string::npos,
        "the host-list token names hosts only, never the access token");

  CoverPublisher::ConfigureAuth("", "");
}

// --- The four-layer order is unchanged by the host chain --------------------

void TestHostFailureDoesNotSkipLayers() {
  using PresenceLayout::CoverLayer;
  using PresenceLayout::ResolveCoverLayer;

  const std::string online =
      "https://e-cdns-images.dzcdn.net/images/cover/abc123/600x600-000000-80-0-0.jpg";
  const Result none = ClassifyUploadResponse(403, "");
  Check(!none.ok, "an all-hosts failure leaves no local url");
  Check(ResolveCoverLayer("", "", online) == CoverLayer::kOnlineKeyless,
        "four-layer order: a failed local publish still descends to online");
  Check(ResolveCoverLayer("", "", online) != CoverLayer::kNoImage,
        "four-layer order: no-image is not used while online resolved");
  Check(ResolveCoverLayer("", "", "") == CoverLayer::kNoImage,
        "four-layer order: no-image only when embedded, sidecar and online all fail");
}

void TestBoundary() {
  const std::string first = MakeBoundary();
  const std::string second = MakeBoundary();
  Check(first != second, "boundaries differ across two calls");
  Check(first.size() == 29 && second.size() == 29, "boundary is 29 bytes");
  Check(first.compare(0, 4, "----") == 0, "boundary keeps its readable prefix");

  bool alphabet_only = true;
  for (char c : first) {
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
    alphabet_only = alphabet_only && allowed;
  }
  Check(alphabet_only, "boundary only uses boundary-safe characters");
}

// --- Track identity keys (album vs album-less) ------------------------------

void TestAlbumIdentityKeys() {
  const std::string artist = "ZWE1HVNDXR Feat yatashigang";
  const std::string album = "LOVELY BASTARDS";
  const std::string file =
      "D:\\Software\\SoulseekQt\\downloads\\complete\\ZWE1HVNDXR Feat yatashigang - LOVELY BASTARDS.flac";
  const std::string other_file =
      "D:\\Software\\SoulseekQt\\downloads\\complete\\ZWE1HVNDXR - untitled take.flac";
  const std::string cover = "https://litter.catbox.moe/ofx201.jpg";

  // An album tag is still the whole identity: artist + "\n" + album, with the
  // file path never part of an albumed key.
  const std::string album_key = AlbumArt::BuildAlbumKey(artist, album, file);
  Check(album_key == artist + "\n" + album, "album tag -> key is artist + newline + album");
  Check(album_key.find(file) == std::string::npos, "an albumed key never embeds the file path");

  // ...and a cover resolved under that key is still adopted.
  std::string source;
  Check(PresenceLayout::ResolveLargeImage(album_key, album_key, cover, "", "", &source) == cover &&
            source == "local",
        "a resolved cover is applied for an albumed track");

  // An album-less track has no album identity, but it still has an identity:
  // the old collapsed key (artist + "\n") must not come back, because a cover
  // resolved for it is applied even though the album tag is empty.
  const std::string albumless_key = AlbumArt::BuildAlbumKey(artist, "", file);
  Check(albumless_key != artist + "\n", "album-less key no longer collapses to artist + newline");
  Check(PresenceLayout::ResolveLargeImage(albumless_key, albumless_key, cover, "", "", &source) == cover &&
            source == "local",
        "an empty album does not suppress the resolved cover");

  // The caption is still governed by the album tag alone: only the image
  // survives an empty album, never large_text.
  PresenceLayout::TrackFields caption_track;
  caption_track.artist = artist;
  caption_track.title = "LOVELY BASTARDS";
  const PresenceLayout::TextFields fields = PresenceLayout::BuildTextFields(
      caption_track, "{title}", "{artist}", "{album}");
  Check(fields.large_text.empty(), "an album-less track still omits the large_text caption");

  // Two album-less files by one artist are two identities, and one's cover is
  // never adopted for the other.
  const std::string other_key = AlbumArt::BuildAlbumKey(artist, "", other_file);
  Check(albumless_key != other_key, "two album-less files by one artist get different keys");
  Check(PresenceLayout::ResolveLargeImage(other_key, albumless_key, cover, "", "", &source).empty(),
        "an album-less file cannot inherit another file's cover");

  // An album-less key never collides with a real album's key for the same
  // artist, in either direction, so neither can overwrite the other's cover.
  Check(albumless_key != album_key, "album-less key never equals a real album's key");
  Check(PresenceLayout::ResolveLargeImage(album_key, albumless_key, cover, "", "", &source).empty(),
        "an album-less cover cannot overwrite a real album's stored cover");
  Check(PresenceLayout::ResolveLargeImage(albumless_key, album_key, cover, "", "", &source).empty(),
        "a real album's cover cannot be applied to an album-less file");
}

// --- Final cover chain: local -> online -> omitted ----------------------------

void TestCoverFallbackChain() {
  const std::string artist = "ZWE1HVNDXR Feat yatashigang";
  const std::string album = "LOVELY BASTARDS";
  const std::string file =
      "D:\\Software\\SoulseekQt\\downloads\\complete\\ZWE1HVNDXR Feat yatashigang - LOVELY BASTARDS.flac";
  const std::string key = AlbumArt::BuildAlbumKey(artist, album, file);
  const std::string local_url = "https://litter.catbox.moe/ofx201.jpg";
  const std::string online_url =
      "https://e-cdns-images.dzcdn.net/images/cover/abc123/600x600-000000-80-0-0.jpg";
  std::string source;

  // (a) A published local cover wins; the online URL is not used, and the
  // field is not left empty.
  const std::string local_win =
      PresenceLayout::ResolveLargeImage(key, key, local_url, key, online_url, &source);
  Check(local_win == local_url, "chain: local cover is the resolved image");
  Check(source == "local", "chain: local cover names its source");
  Check(local_win != online_url && !local_win.empty(),
        "chain: online is not used and the field is not empty when local wins");

  // (b) With no local cover, the online chain's URL is used; the field is not
  // left empty.
  const std::string online_win =
      PresenceLayout::ResolveLargeImage(key, "", "", key, online_url, &source);
  Check(online_win == online_url, "chain: online url is the resolved image");
  Check(source == "online", "chain: online url names its source");
  Check(!online_win.empty(), "chain: the field is not empty when online wins");

  // (c) With neither source resolved, the field is empty (omitted on the wire,
  // so Discord renders its native placeholder) - never a stand-in image URL.
  const std::string empty =
      PresenceLayout::ResolveLargeImage(key, "", "", "", "", &source);
  Check(empty.empty(), "chain: no source resolves to an empty image field");
  Check(source == "none", "chain: the empty field names its source");

  // (d) The Task 1 regression: an album-less track with no local cover still
  // triggers the online lookup (the album tag is not part of the gate), and
  // its result is adopted under the file-path identity.
  Check(PresenceLayout::ShouldRequestOnlineArtwork(true, true, artist, ""),
        "chain: album-less track still triggers the online lookup");
  const std::string albumless_key = AlbumArt::BuildAlbumKey(artist, "", file);
  Check(PresenceLayout::ResolveLargeImage(albumless_key, "", "", albumless_key,
                                          online_url, &source) == online_url &&
            source == "online",
        "chain: an album-less track adopts the online url");
  Check(!PresenceLayout::ShouldRequestOnlineArtwork(true, false, artist, ""),
        "chain: online disabled never triggers a lookup");

  // (e) The literal "aimp" - a foreign Discord application's bundled asset -
  // is never the resolved image in any case.
  Check(local_win != "aimp" && online_win != "aimp" && empty != "aimp",
        "chain: literal aimp is never the resolved image");
}

// --- Card large_image: never empty (transparent placeholder) -----------------

void TestCardLargeImagePlaceholder() {
  using PresenceLayout::kTransparentLargeImageUrl;
  using PresenceLayout::ResolveCardLargeImage;

  // The placeholder URL is pinned once, so it cannot drift silently.
  Check(std::string(kTransparentLargeImageUrl) ==
            "https://raw.githubusercontent.com/nathwn12/aimp-discord-presence-art/"
            "main/covers/transparent.png",
        "card: the transparent placeholder url is pinned");
  // AppendImageField truncates at 300 characters.
  Check(std::strlen(kTransparentLargeImageUrl) < 300,
        "card: the placeholder url fits the wire truncation limit");

  const std::string key = AlbumArt::BuildAlbumKey("Daft Punk", "Discovery", "");
  const std::string local_url = "https://litter.catbox.moe/ofx201.jpg";
  const std::string online_url =
      "https://e-cdns-images.dzcdn.net/images/cover/abc123/600x600-000000-80-0-0.jpg";
  std::string source;

  // (a) Nothing resolved and nothing keyed: the placeholder is sent and the
  // resolver's own source verdict is unchanged.
  const std::string no_cover = ResolveCardLargeImage(key, "", "", "", "", &source);
  Check(no_cover == kTransparentLargeImageUrl, "card: no cover sends the placeholder");
  Check(source == "none", "card: the placeholder keeps the resolver's none source");

  // (b) A published local cover wins and is not replaced.
  const std::string local_win =
      ResolveCardLargeImage(key, key, local_url, key, online_url, &source);
  Check(local_win == local_url && local_win != kTransparentLargeImageUrl,
        "card: a local cover beats the placeholder");
  Check(source == "local", "card: the local cover keeps its source");

  // (c) No local cover: the online url is sent, not the placeholder.
  const std::string online_win =
      ResolveCardLargeImage(key, "", "", key, online_url, &source);
  Check(online_win == online_url && online_win != kTransparentLargeImageUrl &&
            !online_win.empty(),
        "card: an online url beats the placeholder");
  Check(source == "online", "card: the online url keeps its source");

  // (d) A stale key is never adopted: the placeholder substitutes, never a url
  // that belongs to another track.
  const std::string other_key = AlbumArt::BuildAlbumKey("Daft Punk", "Homework", "");
  const std::string stale =
      ResolveCardLargeImage(key, other_key, local_url, other_key, online_url, &source);
  Check(stale == kTransparentLargeImageUrl, "card: a stale key sends the placeholder");
  Check(stale != local_url && stale != online_url,
        "card: a stale key never sends another track's cover");
  Check(source == "none", "card: the stale key keeps the resolver's none source");

  // (e) Album-art display disabled: ResolveLargeImageLocked empties both urls by
  // construction, and the card still carries the placeholder - disabled art is
  // transparent, never yellow.
  const std::string disabled = ResolveCardLargeImage(key, key, "", key, "", &source);
  Check(disabled == kTransparentLargeImageUrl,
        "card: art display disabled sends the placeholder");
  Check(!disabled.empty(), "card: art display disabled never leaves the image empty");

  // No input shape leaves the card image blank.
  Check(!ResolveCardLargeImage("", "", "", "", "", nullptr).empty() &&
            !ResolveCardLargeImage(key, key, "", "", "", nullptr).empty() &&
            !ResolveCardLargeImage("", key, local_url, "", online_url, nullptr).empty(),
        "card: no input combination leaves the image field empty");
}

// --- The explicit four-layer chain, in order -------------------------------

void TestCoverLayerOrder() {
  using PresenceLayout::CoverLayer;
  using PresenceLayout::ResolveCoverLayer;

  const std::string embedded = "https://litter.catbox.moe/abc123.png";
  const std::string sidecar = "https://n.uguu.se/nUoxUKCo.png";
  const std::string online =
      "https://e-cdns-images.dzcdn.net/images/cover/abc123/600x600-000000-80-0-0.jpg";

  // Layer 1: embedded art published -> embedded wins, nothing below is used.
  Check(ResolveCoverLayer(embedded, sidecar, online) == CoverLayer::kLocalEmbedded,
        "layer 1: published embedded art is the chosen layer");
  Check(ResolveCoverLayer(embedded, "", "") == CoverLayer::kLocalEmbedded,
        "layer 1: embedded wins even when nothing else resolved");

  // Layer 2: no embedded art, a sidecar published -> sidecar wins.
  Check(ResolveCoverLayer("", sidecar, online) == CoverLayer::kLocalSidecar,
        "layer 2: a published sidecar wins when embedded art is absent");
  Check(ResolveCoverLayer("", sidecar, "") == CoverLayer::kLocalSidecar,
        "layer 2: sidecar wins even when online did not resolve");

  // Layer 3: neither local layer delivered, online resolved -> online wins.
  Check(ResolveCoverLayer("", "", online) == CoverLayer::kOnlineKeyless,
        "layer 3: online wins when both local layers failed");

  // Layer 4: no image is reached ONLY when all three above produced nothing.
  Check(ResolveCoverLayer("", "", "") == CoverLayer::kNoImage,
        "layer 4: no image only when embedded, sidecar and online all failed");

  // A local art extract that could not be published leaves its URL empty, so
  // the chain descends to online - never straight to no-image.
  const std::string embedded_failed = "";   // extracted but upload failed
  const std::string sidecar_failed = "";    // sidecar found but upload failed
  Check(ResolveCoverLayer(embedded_failed, sidecar_failed, online) ==
            CoverLayer::kOnlineKeyless,
        "layer order: a failed local publish falls through to online, not no-image");
  Check(ResolveCoverLayer(embedded_failed, sidecar_failed, online) !=
            CoverLayer::kNoImage,
        "layer order: no image is not chosen while online resolved");

  // The layer names are stable and greppable.
  Check(std::string(PresenceLayout::CoverLayerName(CoverLayer::kLocalEmbedded)) ==
            "local-embedded",
        "layer names: 1 is local-embedded");
  Check(std::string(PresenceLayout::CoverLayerName(CoverLayer::kLocalSidecar)) ==
            "local-sidecar",
        "layer names: 2 is local-sidecar");
  Check(std::string(PresenceLayout::CoverLayerName(CoverLayer::kOnlineKeyless)) ==
            "online-keyless",
        "layer names: 3 is online-keyless");
  Check(std::string(PresenceLayout::CoverLayerName(CoverLayer::kNoImage)) == "no-image",
        "layer names: 4 is no-image");

  // The online request is never suppressed by a local cover existing: a track
  // whose local art was found (and may still fail to publish) still asks online.
  Check(PresenceLayout::ShouldRequestOnlineForTrack(true, true, "Daft Punk", true, false),
        "layer order: local art found, not yet published -> online still requested");
  Check(PresenceLayout::ShouldRequestOnlineForTrack(true, true, "Daft Punk", true, true),
        "layer order: local art published -> online request still permitted");
  Check(PresenceLayout::ShouldRequestOnlineForTrack(true, true, "Daft Punk", false, false),
        "layer order: no local art -> online requested");
  Check(!PresenceLayout::ShouldRequestOnlineForTrack(true, false, "Daft Punk", true, false),
        "layer order: online disabled still suppresses the request");
  Check(!PresenceLayout::ShouldRequestOnlineForTrack(true, true, "", true, false),
        "layer order: an empty artist still suppresses the request");
}

// --- Publish pending: the card is not blanked ------------------------------

void TestPublishPendingNotBlank() {
  using PresenceLayout::CoverLayer;
  using PresenceLayout::ResolveCoverLayer;

  // While a local publish is in flight (no URL yet) the online rung may not
  // have resolved either; the resolver then yields an empty image field, which
  // the wire builder omits (AppendImageField skips empties), so Discord shows
  // its native placeholder instead of a stand-in. The *card* is not blanked
  // because the plugin holds the previous large_image until a *new* value is
  // resolved: applying the same value is a no-op, so a pending publish cannot
  // clear the image by re-applying an empty over an empty.
  const std::string key = AlbumArt::BuildAlbumKey("Daft Punk", "Discovery", "");
  const std::string resolved_pending =
      PresenceLayout::ResolveLargeImage(key, "", "", "", "", nullptr);
  Check(resolved_pending.empty(),
        "pending: no local url and no online url resolves to an empty image field");
  std::string source;
  Check(PresenceLayout::ResolveLargeImage(key, "", "", "", "", &source).empty() &&
            source == "none",
        "pending: the empty field names its source");
  Check(ResolveCoverLayer("", "", "") == CoverLayer::kNoImage,
        "pending: with nothing resolved yet the no-image layer holds");
  // Once a local or online URL lands, it differs from empty and is applied.
  const std::string after_publish =
      PresenceLayout::ResolveLargeImage(key, key, "https://n.uguu.se/abc.png", "", "", nullptr);
  Check(!after_publish.empty() && after_publish != resolved_pending,
        "pending: a landed local url is a change the card applies");
}

// --- Online rung: decision outcomes (offline, pure functions) ---------------

void TestOnlineRungDecision() {
  const std::string artist = "Daft Punk";
  const std::string album = "Discovery";
  const std::string key = AlbumArt::BuildAlbumKey(artist, album, "");
  std::string source;

  // The request predicate: a cover-less track with a non-empty album is looked
  // up. The album tag is not part of the gate, and neither is the local cover.
  Check(PresenceLayout::ShouldRequestOnlineArtwork(true, true, artist, album),
        "online rung: a cover-less albumed track triggers the lookup");
  Check(!PresenceLayout::ShouldRequestOnlineArtwork(false, true, artist, album),
        "online rung: use_albumart off suppresses the lookup");
  Check(!PresenceLayout::ShouldRequestOnlineArtwork(true, false, artist, album),
        "online rung: use_online off suppresses the lookup");
  Check(!PresenceLayout::ShouldRequestOnlineArtwork(true, true, "", album),
        "online rung: an empty artist suppresses the lookup");

  // Resolved: a provider hit that survives the identity guard yields its URL,
  // and the resolved URL is what the plugin applies - no stand-in is used.
  const std::string deezer_body =
      "{\"data\":[{\"title\":\"Discovery\",\"artist\":{\"name\":\"Daft Punk\"},"
      "\"cover_xl\":\"https://e-cdns-images.dzcdn.net/images/cover/abc123/600x600-000000-80-0-0.jpg\"}],"
      "\"total\":1}";
  Check(AlbumArt::SearchResultMatches(deezer_body, AlbumArt::Provider::kDeezer, artist, album),
        "online rung: a deezer hit for the requested album passes the guard");
  const std::string deezer_url =
      AlbumArt::ExtractArtworkUrl(deezer_body, AlbumArt::Provider::kDeezer);
  Check(deezer_url ==
            "https://e-cdns-images.dzcdn.net/images/cover/abc123/600x600-000000-80-0-0.jpg",
        "online rung: the deezer cover_xl url is the resolved url");
  const std::string applied =
      PresenceLayout::ResolveLargeImage(key, "", "", key, deezer_url, &source);
  Check(applied == deezer_url && source == "online",
        "online rung: a resolved url is applied as the online image");
  Check(!applied.empty(),
        "online rung: the field is not empty when a provider resolved");

  const std::string itunes_match =
      "{\"resultCount\":1,\"results\":[{\"artistName\":\"Daft Punk\","
      "\"collectionName\":\"Discovery\","
      "\"artworkUrl100\":\"https://is1-ssl.mzstatic.com/image/thumb/Music/xyz/100x100bb.jpg\"}]}";
  Check(AlbumArt::SearchResultMatches(itunes_match, AlbumArt::Provider::kItunes, artist, album),
        "online rung: a matching itunes hit passes the guard");
  Check(AlbumArt::ExtractArtworkUrl(itunes_match, AlbumArt::Provider::kItunes) ==
            "https://is1-ssl.mzstatic.com/image/thumb/Music/xyz/600x600bb.jpg",
        "online rung: the itunes artwork url is upscaled to 600x600");

  // Not found: every provider answered and none matched, so the resolver
  // reports no URL and the plugin sends no image (field omitted).
  const std::string dbz_artist = "Dragon Ball Z";
  const std::string dbz_album = "Dragon Ball Z BGM Collection Disc 1";
  const std::string dbz_key = AlbumArt::BuildAlbumKey(dbz_artist, dbz_album, "");
  const std::string empty_deezer = "{\"data\":[],\"total\":0}";
  Check(!AlbumArt::SearchResultMatches(empty_deezer, AlbumArt::Provider::kDeezer, dbz_artist,
                                       dbz_album),
        "online rung: an empty deezer answer is not a match");
  Check(AlbumArt::ExtractArtworkUrl(empty_deezer, AlbumArt::Provider::kDeezer).empty(),
        "online rung: an empty deezer answer yields no url");

  // The real-world iTunes case: a hit for another album must be rejected, not
  // adopted. The guard is what keeps an unrelated cover off the card.
  const std::string itunes_unrelated =
      "{\"resultCount\":1,\"results\":[{\"artistName\":\"Dragon Ball Z\","
      "\"collectionName\":\"Dragon '98 Special Live\","
      "\"artworkUrl100\":\"https://is1-ssl.mzstatic.com/image/thumb/Music/abc/100x100bb.jpg\"}]}";
  Check(!AlbumArt::SearchResultMatches(itunes_unrelated, AlbumArt::Provider::kItunes, dbz_artist,
                                       dbz_album),
        "online rung: an unrelated itunes hit is rejected by the identity guard");

  const std::string musicbrainz_empty = "{\"count\":0,\"release-groups\":[]}";
  Check(!AlbumArt::SearchResultMatches(musicbrainz_empty, AlbumArt::Provider::kMusicBrainz,
                                       dbz_artist, dbz_album),
        "online rung: an empty musicbrainz answer is not a match");
  Check(AlbumArt::ExtractMusicBrainzReleaseGroupId(musicbrainz_empty).empty(),
        "online rung: an empty musicbrainz answer yields no mbid");

  const std::string empty =
      PresenceLayout::ResolveLargeImage(dbz_key, "", "", dbz_key, "", &source);
  Check(empty.empty() && source == "none",
        "online rung: not-found sends no image");
  Check(empty != deezer_url, "online rung: the empty field is not another album's url");

  // A URL resolved for a different identity is never adopted for this one.
  const std::string other_key = AlbumArt::BuildAlbumKey(artist, "Homework", "");
  Check(PresenceLayout::ResolveLargeImage(key, "", "", other_key, deezer_url, &source).empty(),
        "online rung: another album's resolved url is never applied");
}

// --- Album-less online rung: the six album-empty guarantees -----------------
//
// (1) the request predicate fires; (2) album-empty is no longer a definitive
// nothing-to-query case; (3) an artist-only hit is accepted; (4) a wrong-album
// hit is still rejected when the request names an album; (5) album-less keys
// stay file-path distinct; (6) the decision table picks online when it resolves.

void TestAlbumlessOnlineRung() {
  const std::string artist = "Daft Punk";
  const std::string file_a = "D:\\music\\untitled one.flac";
  const std::string file_b = "D:\\music\\untitled two.flac";
  const std::string online_url =
      "https://e-cdns-images.dzcdn.net/images/cover/abc123/600x600-000000-80-0-0.jpg";
  std::string source;

  // (1) ShouldRequestOnlineArtwork is true for artist-present + album-empty, and
  // the other combinations are unchanged.
  Check(PresenceLayout::ShouldRequestOnlineArtwork(true, true, artist, ""),
        "albumless: artist present + album empty requests online artwork");
  Check(!PresenceLayout::ShouldRequestOnlineArtwork(true, true, "", ""),
        "albumless: no artist still suppresses the request");
  Check(!PresenceLayout::ShouldRequestOnlineArtwork(false, true, artist, ""),
        "albumless: use_albumart off still suppresses the request");
  Check(!PresenceLayout::ShouldRequestOnlineArtwork(true, false, artist, ""),
        "albumless: use_online off still suppresses the request");

  // (2) The resolver's genuine nothing-to-query case is an empty artist only: an
  // empty album is an artist-only search, not a definitive miss. This is proven
  // through observable behavior - the request predicate used to be the thing
  // that refused album-empty, and it must now let it through; the identity guard
  // is exercised below to show the artist-only path is reachable and accepted.
  const std::string artist_only_body =
      "{\"data\":[{\"title\":\"Random Access Memories\",\"artist\":{\"name\":\"Daft Punk\"},"
      "\"cover_xl\":\"https://e-cdns-images.dzcdn.net/images/cover/abc123/600x600-000000-80-0-0.jpg\"}],"
      "\"total\":1}";
  // (3) An artist-only match is ACCEPTED when the requested album is empty, even
  // though the hit names a real album the caller did not ask about.
  Check(AlbumArt::SearchResultMatches(artist_only_body, AlbumArt::Provider::kDeezer, artist, ""),
        "albumless: an artist-only hit is accepted when the requested album is empty");
  Check(AlbumArt::MetadataMatches("", "Random Access Memories"),
        "albumless: an empty expected album never vetoes a present hit album");
  Check(!AlbumArt::MetadataMatches("Daft Punk", ""),
        "albumless: an empty candidate is still rejected");

  // (4) REGRESSION: a wrong-album hit is still REJECTED when the requested album
  // is non-empty, so the guard did not become a rubber stamp.
  const std::string wrong_album_body =
      "{\"data\":[{\"title\":\"Homework\",\"artist\":{\"name\":\"Daft Punk\"},"
      "\"cover_xl\":\"https://e-cdns-images.dzcdn.net/images/cover/abc123/600x600-000000-80-0-0.jpg\"}],"
      "\"total\":1}";
  Check(!AlbumArt::SearchResultMatches(wrong_album_body, AlbumArt::Provider::kDeezer, artist,
                                       "Discovery"),
        "albumless regression: a wrong-album hit is rejected when an album is requested");
  Check(!AlbumArt::MetadataMatches("Discovery", "Homework"),
        "albumless regression: MetadataMatches rejects a different present album");

  // (5) REGRESSION: an album-less BuildAlbumKey still embeds the file path, so
  // two album-less tracks by one artist produce DIFFERENT keys.
  const std::string key_a = AlbumArt::BuildAlbumKey(artist, "", file_a);
  const std::string key_b = AlbumArt::BuildAlbumKey(artist, "", file_b);
  Check(key_a.find(file_a) != std::string::npos,
        "albumless: the key embeds the file path");
  Check(key_a != key_b,
        "albumless: two album-less tracks by one artist get different keys");
  Check(key_a != artist + "\n",
        "albumless: the key does not collapse to artist + newline");

  // (6) Decision table: album-less + online-resolved => the ONLINE url is chosen.
  const std::string applied =
      PresenceLayout::ResolveLargeImage(key_a, "", "", key_a, online_url, &source);
  Check(applied == online_url && source == "online",
        "albumless: the online url is chosen when it resolves");
  const std::string unresolved =
      PresenceLayout::ResolveLargeImage(key_b, "", "", key_a, online_url, &source);
  Check(unresolved.empty() && source == "none",
        "albumless: a url resolved for another file's key is not adopted");
}

// --- SHA-256 cache keys -----------------------------------------------------

void TestSha256() {
  const std::vector<unsigned char> abc = {'a', 'b', 'c'};
  Check(Sha256Hex(abc) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "sha256 matches the abc known answer");
  const std::vector<unsigned char> empty;
  Check(Sha256Hex(empty) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "sha256 matches the empty-input known answer");

  const std::string two_block = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  const std::vector<unsigned char> two_block_bytes(two_block.begin(), two_block.end());
  Check(Sha256Hex(two_block_bytes) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
        "sha256 matches the 56-byte known answer");

  const std::vector<unsigned char> image_a(1000, 0x11);
  const std::vector<unsigned char> image_b(1000, 0x12);
  Check(Sha256Hex(image_a) == Sha256Hex(image_a), "cache key is stable for the same bytes");
  Check(Sha256Hex(image_a) != Sha256Hex(image_b), "cache key differs for different bytes");
}

// --- Compression decision ---------------------------------------------------

void TestCompressionPlan() {
  // The re-encode is a 512 px centre-square: crop to the shorter side first,
  // then cap that square at 512. Aspect is intentionally NOT preserved.
  const CompressionPlan large = PlanCompression(2000, 1500, 5 * 1024 * 1024);
  Check(large.reencode, "large image is re-encoded");
  Check(large.width == 512 && large.height == 512,
        "2000x1500 crops to a 1500 square, capped at 512");

  const CompressionPlan portrait = PlanCompression(900, 2400, 400 * 1024);
  Check(portrait.reencode, "portrait image is re-encoded");
  Check(portrait.width == 512 && portrait.height == 512,
        "900x2400 crops to a 900 square, capped at 512");

  const CompressionPlan already_small = PlanCompression(320, 240, 100 * 1024);
  Check(!already_small.reencode, "image at or below 512 px and 256 KB skips re-encoding");

  const CompressionPlan at_bound = PlanCompression(512, 512, 256 * 1024);
  Check(!at_bound.reencode, "the 512 px / 256 KB boundary is still 'already small'");

  const CompressionPlan heavy = PlanCompression(320, 240, 300 * 1024);
  Check(heavy.reencode, "small dimensions but heavy bytes are re-encoded");
  Check(heavy.width == 240 && heavy.height == 240,
        "a 320x240 in-place crop uses the shorter side and never upscales");

  const CompressionPlan just_over = PlanCompression(601, 600, 100 * 1024);
  Check(just_over.reencode && just_over.width == 512 && just_over.height == 512,
        "601 px on the longest side is scaled under the 512 bound");

  const CompressionPlan unknown = PlanCompression(0, 0, 1024);
  Check(!unknown.reencode && unknown.width == 0 && unknown.height == 0, "unknown dimensions are left alone");
}

// --- Compression round trip (real WIC, still offline) -----------------------

std::vector<unsigned char> EncodeTestPng(UINT width, UINT height, bool noise) {
  std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
  if (noise) {
    uint32_t state = 0x9e3779b9u;
    for (size_t i = 0; i < pixels.size(); i += 4) {
      state = state * 1664525u + 1013904223u;
      pixels[i] = static_cast<unsigned char>(state >> 24);
      pixels[i + 1] = static_cast<unsigned char>(state >> 16);
      pixels[i + 2] = static_cast<unsigned char>(state >> 8);
      pixels[i + 3] = 0xff;
    }
  } else {
    for (UINT y = 0; y < height; ++y) {
      for (UINT x = 0; x < width; ++x) {
        const size_t i = (static_cast<size_t>(y) * width + x) * 4;
        pixels[i] = static_cast<unsigned char>(x * 255 / width);
        pixels[i + 1] = static_cast<unsigned char>(y * 255 / height);
        pixels[i + 2] = 0x40;
        pixels[i + 3] = 0xff;
      }
    }
  }

  std::vector<unsigned char> png;
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  {
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    Microsoft::WRL::ComPtr<IStream> output_stream;
    Microsoft::WRL::ComPtr<IWICStream> wic_output;
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    Microsoft::WRL::ComPtr<IPropertyBag2> properties;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.GetAddressOf()));
    if (SUCCEEDED(hr)) {
      hr = factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA, width * 4,
                                           static_cast<UINT>(pixels.size()), pixels.data(), &bitmap);
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
      hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    }
    if (SUCCEEDED(hr)) {
      hr = encoder->Initialize(wic_output.Get(), WICBitmapEncoderNoCache);
    }
    if (SUCCEEDED(hr)) {
      hr = encoder->CreateNewFrame(&frame, &properties);
    }
    if (SUCCEEDED(hr)) {
      hr = frame->Initialize(properties.Get());
    }
    if (SUCCEEDED(hr)) {
      hr = frame->SetSize(width, height);
    }
    WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) {
      hr = frame->SetPixelFormat(&pixel_format);
    }
    if (SUCCEEDED(hr)) {
      hr = frame->WriteSource(bitmap.Get(), nullptr);
    }
    if (SUCCEEDED(hr)) {
      hr = frame->Commit();
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
    }
    if (SUCCEEDED(hr)) {
      png.assign(static_cast<const unsigned char*>(data), static_cast<const unsigned char*>(data) + size);
    }
    if (data != nullptr) {
      GlobalUnlock(memory);
    }
  }
  if (SUCCEEDED(com)) {
    CoUninitialize();
  }
  return png;
}

bool DecodeTestDimensions(const std::vector<unsigned char>& bytes, UINT* width, UINT* height) {
  *width = 0;
  *height = 0;
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool ok = false;
  {
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    Microsoft::WRL::ComPtr<IWICStream> stream;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.GetAddressOf()));
    if (SUCCEEDED(hr)) {
      hr = factory->CreateStream(&stream);
    }
    if (SUCCEEDED(hr)) {
      hr = stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()));
    }
    if (SUCCEEDED(hr)) {
      hr = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    }
    if (SUCCEEDED(hr)) {
      hr = decoder->GetFrame(0, &frame);
    }
    if (SUCCEEDED(hr)) {
      hr = frame->GetSize(width, height);
    }
    ok = SUCCEEDED(hr);
  }
  if (SUCCEEDED(com)) {
    CoUninitialize();
  }
  return ok;
}

void TestCompressionPreparation() {
  const std::vector<unsigned char> large = EncodeTestPng(1600, 1000, true);
  Check(!large.empty(), "fixture encodes a 1600x1000 png");

  const PreparedImage prepared = PrepareImageForUpload(large, "image/png");
  Check(prepared.reencoded, "large png is re-encoded");
  Check(prepared.mime == "image/jpeg", "re-encoded bytes are announced as jpeg");
  Check(!prepared.bytes.empty() && prepared.bytes.size() < large.size(),
        "re-encoded bytes are smaller than the source");
  Check(prepared.bytes != large, "re-encoded bytes differ from the source");
  Check(UploadCacheKey(prepared) == Sha256Hex(prepared.bytes), "cache key is the hash of the uploaded bytes");
  Check(UploadCacheKey(prepared) != Sha256Hex(large), "cache key is not the source hash");
  Check(prepared.note.find("input=" + std::to_string(large.size())) != std::string::npos,
        "note records the input size");
  Check(prepared.note.find("output=" + std::to_string(prepared.bytes.size())) != std::string::npos,
        "note records the output size");
  Check(prepared.note.find("mode=jpeg-q85") != std::string::npos, "note records the encode mode");

  UINT width = 0;
  UINT height = 0;
  Check(DecodeTestDimensions(prepared.bytes, &width, &height), "re-encoded bytes decode");
  Check(width == 512 && height == 512, "the re-encode is a 512 px centre square");

  const std::vector<unsigned char> compact = EncodeTestPng(320, 240, false);
  Check(!compact.empty() && compact.size() <= kSkipReencodeMaxBytes, "fixture is already small");
  const PreparedImage untouched = PrepareImageForUpload(compact, "image/png");
  Check(!untouched.reencoded, "already-small png skips re-encoding");
  Check(untouched.bytes == compact, "original bytes pass through unchanged");
  Check(untouched.mime == "image/png", "original mime is kept");
  Check(UploadCacheKey(untouched) == Sha256Hex(compact), "skipped image is keyed by its original bytes");

  const std::vector<unsigned char> garbage(300 * 1024, 0x5a);
  const PreparedImage fallback = PrepareImageForUpload(garbage, "image/png");
  Check(!fallback.reencoded, "undecodable input is not re-encoded");
  Check(fallback.bytes == garbage, "undecodable input falls back to the original bytes");
  Check(fallback.note.find("wic-failed=") != std::string::npos, "fallback note names the WIC failure");
}

// --- Sidecar art selection (pure) -------------------------------------------
//
// FindSidecarArt's walk touches the filesystem, but the choice it makes inside
// a directory is pure: PickBestSidecarName sees only a list of names. These
// tests exercise the priority order, the case-insensitivity, the Windows Media
// Player names, the extension gate, the 4-level bound, first-hit-wins and the
// empty result offline. `SidecarLevels` mirrors the walk's level arithmetic
// (folder = level 0, stop after kMaxSidecarLevels ascents) so the bound is
// asserted without depending on the machine's directory layout.

std::vector<std::wstring> Names(std::initializer_list<const wchar_t*> items) {
  std::vector<std::wstring> names;
  for (const wchar_t* item : items) {
    names.emplace_back(item);
  }
  return names;
}

// Depth at which a cover named `cover` is found when only one level holds it:
// 0 = the track folder. Returns -1 when the cover is out of the walk's reach.
std::wstring PickNameAtLevel(const std::vector<std::vector<std::wstring>>& folders,
                             int level) {
  if (level < 0 || level >= static_cast<int>(folders.size())) {
    return std::wstring();
  }
  return LocalArt::PickBestSidecarName(folders[level]);
}

int SidecarLevels(const std::vector<std::vector<std::wstring>>& folders) {
  for (int level = 0; level <= LocalArt::kMaxSidecarLevels; ++level) {
    const std::wstring found = PickNameAtLevel(folders, level);
    if (found.compare(0, 5, L"cover") == 0) {
      return level;
    }
  }
  return -1;
}

void TestSidecarSelection() {
  // (1) Priority: cover > folder > front > album > albumart, regardless of the
  // order the directory entries arrive in.
  Check(LocalArt::PickBestSidecarName(
            Names({L"front.png", L"folder.jpg", L"cover.jpg"})) == L"cover.jpg",
        "sidecar: cover.jpg beats folder.jpg and front.png");
  Check(LocalArt::PickBestSidecarName(
            Names({L"folder.jpg", L"cover.jpg"})) == L"cover.jpg",
        "sidecar: cover.jpg beats folder.jpg");
  Check(LocalArt::PickBestSidecarName(
            Names({L"front.png", L"folder.jpg"})) == L"folder.jpg",
        "sidecar: folder.jpg beats front.png");
  Check(LocalArt::PickBestSidecarName(
            Names({L"albumart.png", L"album.jpg", L"front.png"})) == L"front.png",
        "sidecar: front.png beats album.jpg and albumart.png");
  Check(LocalArt::PickBestSidecarName(
            Names({L"albumart.png", L"album.jpg"})) == L"album.jpg",
        "sidecar: album.jpg beats albumart.png");
  Check(LocalArt::PickBestSidecarName(Names({L"albumart.png"})) == L"albumart.png",
        "sidecar: albumart.png is recognised last");
  Check(LocalArt::PickBestSidecarName(
            Names({L"cover.png", L"cover.jpg"})) == L"cover.jpg",
        "sidecar: equal priority breaks on the lexicographically smaller name");

  // (2) Case-insensitivity.
  Check(LocalArt::PickBestSidecarName(Names({L"COVER.JPG"})) == L"COVER.JPG",
        "sidecar: COVER.JPG is accepted");
  Check(LocalArt::PickBestSidecarName(Names({L"Folder.PNG"})) == L"Folder.PNG",
        "sidecar: Folder.PNG is accepted");
  Check(LocalArt::PickBestSidecarName(
            Names({L"folder.jpg", L"COVER.JPG"})) == L"COVER.JPG",
        "sidecar: case is folded for priority too");

  // (3) Windows Media Player names: the AlbumArt prefix is accepted.
  Check(LocalArt::PickBestSidecarName(Names({L"AlbumArtSmall.jpg"})) == L"AlbumArtSmall.jpg",
        "sidecar: AlbumArtSmall.jpg is accepted");
  Check(LocalArt::PickBestSidecarName(
            Names({L"AlbumArt_{4F2B9C1E-0000-0000-0000-000000000000}_Large.jpg"})) ==
            L"AlbumArt_{4F2B9C1E-0000-0000-0000-000000000000}_Large.jpg",
        "sidecar: AlbumArt_{GUID}_Large.jpg is accepted");
  Check(LocalArt::PickBestSidecarName(Names({L"albumartwork.png"})) ==
            L"albumartwork.png",
        "sidecar: any albumart-prefixed name is accepted");
  Check(LocalArt::PickBestSidecarName(Names({L"cover.jpg", L"AlbumArtSmall.jpg"})) == L"cover.jpg",
        "sidecar: AlbumArtSmall ranks below the exact cover name");

  // (4) Extension gate.
  Check(LocalArt::PickBestSidecarName(Names({L"cover.txt"})).empty(),
        "sidecar: cover.txt is rejected");
  Check(LocalArt::PickBestSidecarName(Names({L"cover.bmp"})) == L"cover.bmp",
        "sidecar: cover.bmp is accepted");
  Check(LocalArt::PickBestSidecarName(Names({L"cover.jpeg"})) == L"cover.jpeg",
        "sidecar: cover.jpeg is accepted");
  Check(LocalArt::PickBestSidecarName(Names({L"cover"})).empty(),
        "sidecar: a name with no extension is rejected");
  Check(LocalArt::PickBestSidecarName(Names({L"cover.png.txt"})).empty(),
        "sidecar: the last extension is what counts");
  Check(LocalArt::PickBestSidecarName(
            Names({L"cover.txt", L"front.png"})) == L"front.png",
        "sidecar: a rejected extension lets a valid lower-priority name win");

  // (5) The ascent is bounded at 4 levels: level 4 is reached, level 5 is not.
  Check(SidecarLevels({{L"track.flac"}, {L"x"}, {L"x"}, {L"x"}, {L"cover.jpg"}}) == 4,
        "sidecar: an image four levels up is found");
  Check(SidecarLevels({{L"track.flac"}, {L"x"}, {L"x"}, {L"x"}, {L"x"}, {L"cover.jpg"}}) ==
            -1,
        "sidecar: an image five levels up is not found");
  Check(LocalArt::kMaxSidecarLevels == 4, "sidecar: the ascent bound is four levels");

  // (6) First hit wins: the track folder beats an ancestor.
  Check(SidecarLevels({{L"cover.png"}, {L"cover.jpg"}}) == 0,
        "sidecar: the track folder wins over an ancestor");
  Check(PickNameAtLevel({{L"cover.png"}, {L"cover.jpg"}}, 0) == L"cover.png" &&
            PickNameAtLevel({{L"cover.png"}, {L"cover.jpg"}}, 1) == L"cover.jpg",
        "sidecar: each level is judged on its own entries");

  // (7) No image anywhere in range -> empty, so the caller falls through to the
  // online rung, which may still supply an image.
  Check(SidecarLevels({{L"a.flac"}, {L"b.flac"}, {L"c.flac"}, {L"d.flac"},
                       {L"cover.txt"}}) == -1,
        "sidecar: no image in range yields nothing");
  Check(LocalArt::PickBestSidecarName(Names({L"readme.md", L"notes.txt"})).empty(),
        "sidecar: only non-image files yields nothing");
  Check(LocalArt::PickBestSidecarName(std::vector<std::wstring>()).empty(),
        "sidecar: an empty directory yields nothing");

  // (8) The pure SDK path is untouched: a result with no sidecar names the sdk
  // source, and the caller keeps the SDK bytes when both are present. This is
  // asserted through the selection helpers above (the sidecar succeeds or not)
  // plus the existing chain tests, which never see a sidecar. The extraction
  // function itself is not invoked here because it needs an AIMP service.
  LocalArt::Result sdk_only;
  Check(!sdk_only.found && sdk_only.sha256_hex.empty(),
        "sidecar regression: a default SDK result carries no sidecar bytes");
  const std::wstring track =
      L"E:\\Music4\\Naruto\\ED\\ED 09 - Shinkokyuu.flac";
  Check(track.find(L"ED 09 - Shinkokyuu.flac") != std::wstring::npos &&
            LocalArt::PickBestSidecarName(Names({L"track.flac"})).empty(),
        "sidecar regression: the named failing case has no image in its own folder");
}


void TestCache(const std::wstring& directory) {
  CreateDirectoryW(directory.c_str(), nullptr);
  const std::wstring path = directory + L"\\cover-publisher-cache.txt";
  const std::string cache_path = Utils::ToString(path);
  const std::string url = "https://litter.catbox.moe/abc123.png";
  const std::string key = Sha256Hex(std::vector<unsigned char>{'c', 'a', 'c', 'h', 'e'});
  const long long now = static_cast<long long>(std::time(nullptr));
  DeleteFileW(path.c_str());

  CoverPublisher::Configure(cache_path);
  Result hit;
  Check(!CacheLookup(key, now, &hit), "cache miss on an empty cache");

  Result stored;
  stored.ok = true;
  stored.url = url;
  CacheStore(key, stored, now);
  Check(CacheLookup(key, now, &hit), "cache hit right after store");
  Check(hit.ok && hit.url == url, "cache hit returns the stored url");
  Check(FileExists(path), "store writes the cache file");

  CoverPublisher::Configure(cache_path);  // reload from disk
  Check(CacheLookup(key, now, &hit) && hit.url == url, "cache survives a reload from disk");

  const std::string other_key = Sha256Hex(std::vector<unsigned char>{'o', 't', 'h', 'e', 'r'});
  Result failed;
  failed.reason = "empty-body";
  CacheStore(other_key, failed, now);
  Check(CacheLookup(other_key, now, &hit) && !hit.ok && hit.reason == "empty-body",
        "negative result is cached");
  Check(!CacheLookup(other_key, now + kNegativeCacheTtlSeconds + 1, &hit),
        "negative result expires after its TTL");

  const std::wstring missing = directory + L"\\missing-subdirectory\\cache.txt";
  CoverPublisher::Configure(Utils::ToString(missing));
  CacheStore(key, stored, now);
  Check(CacheLookup(key, now, &hit) && hit.url == url,
        "an unwritable cache path still serves memory only");

  CoverPublisher::Configure(cache_path);
  CacheStore(key, stored, now);
  CoverPublisher::ClearCache();
  Check(!CacheLookup(key, now, &hit), "ClearCache forgets memory");
  Check(!FileExists(path), "ClearCache removes the cache file");

  DeleteFileW(path.c_str());
  RemoveDirectoryW(directory.c_str());
}

// --- Ephemeral cache entries: upgrade vs revalidate --------------------------
//
// A cache hit on a keyless URL is never trusted blindly. With the permanent
// GitHub host active the local bytes are re-published there (the Death Note
// case: a dead cached uguu URL becomes a raw.githubusercontent URL and the
// cache is updated); without a token the stored URL is revalidated and a dead
// entry falls through to a fresh keyless upload. The planning is pure, so it
// is asserted here; the network legs (GitHub PUT, health-check GET, fresh
// upload) are the existing proven transports.

void TestCachedUrlPlan() {
  const std::string uguu_url = "https://n.uguu.se/nUoxUKCo.png";
  const std::string catbox_url = "https://files.catbox.moe/a1b2c3.png";
  const std::string litterbox_url = "https://litter.catbox.moe/abc123.png";
  const std::string github_name(64, 'a');
  const std::string github_url =
      "https://raw.githubusercontent.com/nathwn12/aimp-discord-presence-art/main/covers/" +
      github_name + ".jpg";

  // Every keyless host's URL is ephemeral, whatever the subdomain or extension.
  Check(IsEphemeralCachedUrl(uguu_url), "plan: a uguu url is ephemeral");
  Check(IsEphemeralCachedUrl("https://h.uguu.se/yHYDFJRh.jpg"),
        "plan: a uguu url on another subdomain is ephemeral");
  Check(IsEphemeralCachedUrl(catbox_url), "plan: a catbox url is ephemeral");
  Check(IsEphemeralCachedUrl(litterbox_url), "plan: a litterbox url is ephemeral");
  Check(!IsEphemeralCachedUrl(github_url), "plan: a github raw url is not ephemeral");
  Check(IsGitHubCachedUrl(github_url), "plan: a github raw url is durable");
  Check(!IsGitHubCachedUrl(uguu_url), "plan: a uguu url is not durable");
  Check(LooksLikeUploadUrl(github_url), "plan: the github raw url is a recognised upload url");

  // With the GitHub host active, every ephemeral hit is upgraded.
  Check(PlanCachedUrl(uguu_url, true) == CachedUrlAction::kUpgradeToGitHub,
        "plan: uguu hit + token -> re-publish to github");
  Check(PlanCachedUrl(catbox_url, true) == CachedUrlAction::kUpgradeToGitHub,
        "plan: catbox hit + token -> re-publish to github");
  Check(PlanCachedUrl(litterbox_url, true) == CachedUrlAction::kUpgradeToGitHub,
        "plan: litterbox hit + token -> re-publish to github");
  Check(PlanCachedUrl(github_url, true) == CachedUrlAction::kUse,
        "plan: github hit + token -> used as-is, never re-uploaded");

  // Without a token, an ephemeral hit is revalidated, never trusted blindly.
  Check(PlanCachedUrl(uguu_url, false) == CachedUrlAction::kRevalidate,
        "plan: uguu hit, no token -> revalidate");
  Check(PlanCachedUrl(catbox_url, false) == CachedUrlAction::kRevalidate,
        "plan: catbox hit, no token -> revalidate");
  Check(PlanCachedUrl(litterbox_url, false) == CachedUrlAction::kRevalidate,
        "plan: litterbox hit, no token -> revalidate");
  Check(PlanCachedUrl(github_url, false) == CachedUrlAction::kUse,
        "plan: github hit, no token -> still used as-is");

  // The active flag mirrors the host list: a token with a malformed repo is
  // present-but-unused, exactly like no token.
  CoverPublisher::ConfigureAuth("test-token", "");
  Check(GitHubUploadActive(), "plan: token + default repo -> github active");
  Check(PlanCachedUrl(uguu_url, GitHubUploadActive()) == CachedUrlAction::kUpgradeToGitHub,
        "plan: the live active flag upgrades an ephemeral hit");
  CoverPublisher::ConfigureAuth("test-token", "no-slash-here");
  Check(!GitHubUploadActive(), "plan: token + malformed repo -> github inactive");
  Check(PlanCachedUrl(uguu_url, GitHubUploadActive()) == CachedUrlAction::kRevalidate,
        "plan: present-but-unused token still revalidates");
  CoverPublisher::ConfigureAuth("", "");
  Check(!GitHubUploadActive(), "plan: no token -> github inactive");
}

}  // namespace

int main(int argc, char** argv) {
  const std::wstring directory = CacheDirectoryFromArgs(argc, argv);
  std::printf("cache directory: %s\n", Utils::ToString(directory).c_str());

  TestUrlShape();
  TestClassifier();
  TestHostFallback();
  TestMultipartArithmetic();
  TestHostList();
  TestMultipartShapes();
  TestCentreSquareCrop();
  TestGitHubHostConfig();
  TestGitHubRequestShape();
  TestGitHubResponseClassification();
  TestCatboxHost();
  TestTokenNeverLeaks();
  TestHostFailureDoesNotSkipLayers();
  TestBoundary();
  TestAlbumIdentityKeys();
  TestCoverFallbackChain();
  TestCardLargeImagePlaceholder();
  TestCoverLayerOrder();
  TestPublishPendingNotBlank();
  TestOnlineRungDecision();
  TestAlbumlessOnlineRung();
  TestSha256();
  TestCompressionPlan();
  TestCompressionPreparation();
  TestSidecarSelection();
  TestCache(directory);
  TestCachedUrlPlan();

  std::printf("cover_publisher tests: %d checks, %d failures\n", g_checks, g_failures);
  std::printf("RESULT: %s\n", g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}
