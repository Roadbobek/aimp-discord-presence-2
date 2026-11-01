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

#include "aimp_discord_presence.h"

#include <chrono>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "aimp_core.h"
#include "aimp_filemanager.h"
#include "aimp_messages.h"
#include "aimp_player.h"
#include "cover_publisher.h"
#include "presence_layout.h"
#include "presence_rate_limit.h"
#include "presence_throttle_plan.h"
#include "utils.h"

namespace {

// The publisher logs through a plain function pointer (it owns no plugin
// object). This trampoline forwards to the live instance's LogCover, which
// routes to the DebugLog client. Set once in Load(), cleared in Unload().
AimpDiscordPresence* g_publisher_log_target = nullptr;

void PublisherLogTrampoline(const std::string& line) {
  if (g_publisher_log_target != nullptr) {
    g_publisher_log_target->LogCover(line);
  }
}

constexpr int kPlayerStateStopped = 0;
constexpr int kPlayerStatePaused = 1;
constexpr int kPlayerStatePlaying = 2;

// A position difference larger than this means the user seeked and the
// timestamp Discord is counting down locally has to be corrected.
constexpr double kSeekDriftSeconds = 3.0;

// Input cap only: a cover above this size is skipped before the publisher sees
// it (a pathological source is not compressed into submission). Anything that
// passes is downscaled and re-encoded before transfer.
constexpr size_t kMaxCoverBytes = 20 * 1024 * 1024;

// Unload asks the throttle worker to publish a presence clear and waits this
// long for it. The client call only queues a frame (the client's own worker
// writes it), so the wait is a formality; the bound exists so a wedged client
// can never hold AIMP's unload open.
constexpr int kPresenceClearOnUnloadTimeoutMs = 2000;

int64_t UnixSecondsNow() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

uint64_t MonotonicMillisNow() {
  return static_cast<uint64_t>(GetTickCount64());
}

bool IsStreamUrl(const std::wstring& url) {
  if (url.size() >= 7 && url.compare(0, 7, L"http://") == 0) {
    return true;
  }
  return url.size() >= 8 && url.compare(0, 8, L"https://") == 0;
}

// Drive-absolute ("C:\x") or UNC ("\\server\x") paths are used as configured;
// anything else is resolved below the plugin DLL's directory.
bool IsAbsolutePath(const std::wstring& path) {
  if (path.size() >= 3 && path[1] == L':' &&
      (path[2] == L'\\' || path[2] == L'/')) {
    return true;
  }
  return path.size() >= 2 && (path[0] == L'\\' || path[0] == L'/') &&
         (path[1] == L'\\' || path[1] == L'/');
}

// Directory of the loaded plugin DLL, with a trailing separator, or an empty
// string when it cannot be determined.
std::string PluginDirectory() {
  HMODULE module = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(&PluginDirectory),
                          &module)) {
    return std::string();
  }

  char module_path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameA(module, module_path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return std::string();
  }

  std::string directory(module_path, length);
  const size_t separator = directory.find_last_of("\\/");
  if (separator == std::string::npos) {
    return std::string();
  }

  return directory.substr(0, separator + 1);
}

// Turns the configured DebugLog value into the UTF-8 path the IPC client
// expects. Returns an empty string when the DLL directory cannot be
// determined, in which case logging stays off rather than writing somewhere
// unpredictable.
std::string ResolveDebugLogPath(const std::wstring& configured) {
  if (configured.empty()) {
    return std::string();
  }
  if (IsAbsolutePath(configured)) {
    return Utils::ToString(configured);
  }

  const std::string directory = PluginDirectory();
  if (directory.empty()) {
    return std::string();
  }

  return directory + Utils::ToString(configured);
}

// The cover cache defaults to cover-cache.txt next to the DLL, so an empty
// CoverCache still has a home. A DLL directory that cannot be determined
// leaves the path empty, which turns the publisher's on-disk cache off rather
// than writing somewhere unpredictable.
std::string ResolveCoverCachePath(const std::wstring& configured) {
  if (IsAbsolutePath(configured)) {
    return Utils::ToString(configured);
  }

  const std::string directory = PluginDirectory();
  if (directory.empty()) {
    return std::string();
  }

  return directory + Utils::ToString(configured.empty() ? L"cover-cache.txt"
                                                        : configured);
}

// The Drive refresh token lives in a sidecar in the same directory as the cover
// cache (drive-token.txt), so the whole publish chain's state sits together. An
// empty cache path leaves the sidecar path empty, which turns the Drive host off
// rather than writing a token somewhere unpredictable.
std::string ResolveCoverStatePath(const std::string& cover_cache_path) {
  if (cover_cache_path.empty()) {
    return std::string();
  }
  const size_t separator = cover_cache_path.find_last_of("\\/");
  if (separator == std::string::npos) {
    return std::string();
  }
  return cover_cache_path.substr(0, separator + 1) + "drive-token.txt";
}

}  // namespace

__declspec(dllexport) HRESULT WINAPI AIMPPluginGetHeader(void** Header) {
  *Header = new AimpDiscordPresence();
  return S_OK;
}

LPWSTR AimpDiscordPresence::GetInfo(int index) {
  switch (index) {
    case Info::kName:
      return const_cast<LPWSTR>(L"Discord Presence");
    case Info::kAuthor:
      return const_cast<LPWSTR>(L"Exle (original), nathwn12 (fork), Roadbobek (fork 2)");
    case Info::Description::kShort:
      return const_cast<LPWSTR>(L"Roadbobek's Discord Rich Presence for AIMP \u2014 fork of nathwn12's fork of Exle's plugin");
  }

  return nullptr;
}

DWORD AimpDiscordPresence::GetCategory() {
  return Category::kAddons;
}

bool AimpDiscordPresence::Load() {
  if (!Aimp::Player::Service::Player()) {
    return false;
  }

  LoadConfig();
  InitializeMessageDispatcher();

  client_.reset(new DiscordIpc::Client(std::to_string(settings.application_id)));

  // Opt-in frame log. The client writes nothing while the setting is empty
  // and silently ignores a path it cannot open, so a bad value cannot
  // disturb the connection.
  if (!settings.debug_log.empty()) {
    client_->SetLogPath(ResolveDebugLogPath(settings.debug_log));
  }

  // Publish target for local covers: the cache is keyed by image SHA-256, so a
  // cover is uploaded once and reused. Configuring it never touches the
  // network; an unusable path only costs the on-disk cache.
  const std::string cover_cache_path = ResolveCoverCachePath(settings.cover_cache);
  CoverPublisher::Configure(cover_cache_path);
  // The preferred host chain: "auto" leads with Drive when its credentials are
  // configured, otherwise GitHub. A typo in CoverHost is read as "auto", so the
  // chain is never left empty.
  CoverPublisher::ConfigureHost(Utils::ToString(settings.cover_host));
  // The permanent GitHub host. The token is handed to the publisher and lives
  // only in memory from here on: it is never logged, never written into the
  // cache, and sent only to api.github.com. An empty token simply leaves the
  // GitHub host out of the list, so the chain behaves as it did before.
  CoverPublisher::ConfigureAuth(Utils::ToString(settings.cover_token),
                                Utils::ToString(settings.cover_repo));
  // The Google Drive host. The client secret stays in memory, and the refresh
  // token goes to drive-token.txt next to the cover cache. The first publish
  // that reaches Drive opens the browser for a one-time consent on the cover
  // worker thread; every failure falls through to the next host.
  CoverPublisher::ConfigureDrive(Utils::ToString(settings.drive_client_id),
                                 Utils::ToString(settings.drive_client_secret),
                                 ResolveCoverStatePath(cover_cache_path));
  // Route the publisher's host-choice and all-hosts-failed lines into the same
  // DebugLog as the cover vocabulary.
  g_publisher_log_target = this;
  CoverPublisher::SetLogger(&PublisherLogTrampoline);
  // One line per load, after the logger is live: the policy and which hosts are
  // in the chain, with a fixed reason token for any that are not. This is what
  // makes "no drive lines at all" diagnosable from the log itself.
  CoverPublisher::LogHostConfiguration();

  // Single publisher worker. It owns no AIMP object, only the bytes it is
  // handed, and it is joined in Unload().
  cover_worker_ = std::thread(&AimpDiscordPresence::CoverWorkerMain, this);

  // Rate-limit worker: publishes a deferred update on its trailing edge. It
  // only reads the already-built activity and the IPC client, never AIMP.
  StartThrottleWorker();

  album_art_.SetCallback(
      [this](const std::string& artist, const std::string& album,
             const std::string& file_path, const std::string& url) {
        ApplyResolvedArtwork(artist, album, file_path, url);
      });
  // Online-rung diagnostics go through the same DebugLog path as the cover
  // code; the hook is invoked on the resolver worker thread, never with image
  // bytes.
  album_art_.SetLogger([this](const std::string& line) { LogCover(line); });
  album_art_.SetOnlineEnabled(settings.use_albumart_online);

  client_->EnsureConnected();
  RefreshPresence(true);

  return true;
}

void AimpDiscordPresence::LoadConfig() {
  Aimp::Core::Service::Config config;

  LoadConfigValue(config, L"DiscordPresence\\ApplicationID", &settings.application_id);
  LoadConfigValue(config, L"DiscordPresence\\Timestamp", &settings.timestamp);
  LoadConfigValue(config, L"DiscordPresence\\UseAlbumArt", &settings.use_albumart);
  LoadConfigValue(config, L"DiscordPresence\\UseAlbumArtOnline", &settings.use_albumart_online);
  LoadConfigValue(config, L"DiscordPresence\\LocalCover", &settings.local_cover);
  LoadConfigValue(config, L"DiscordPresence\\CoverCache", &settings.cover_cache);
  LoadConfigValue(config, L"DiscordPresence\\CoverToken", &settings.cover_token);
  LoadConfigValue(config, L"DiscordPresence\\CoverRepo", &settings.cover_repo);
  LoadConfigValue(config, L"DiscordPresence\\CoverHost", &settings.cover_host);
  LoadConfigValue(config, L"DiscordPresence\\LocalCoverArtDirectoryTravelMax", &settings.local_cover_art_directory_travel_max);
  LoadConfigValue(config, L"DiscordPresence\\DriveClientId", &settings.drive_client_id);
  LoadConfigValue(config, L"DiscordPresence\\DriveClientSecret", &settings.drive_client_secret);
  LoadConfigValue(config, L"DiscordPresence\\StatusDisplayType", &settings.status_display_type);
  LoadConfigValue(config, L"DiscordPresence\\Name", &settings.name);
  LoadConfigValue(config, L"DiscordPresence\\Details", &settings.details_template);
  LoadConfigValue(config, L"DiscordPresence\\State", &settings.state_template);
  LoadConfigValue(config, L"DiscordPresence\\LargeText", &settings.large_text_template);
  LoadConfigValue(config, L"DiscordPresence\\Button1.Label", &settings.button1_label);
  LoadConfigValue(config, L"DiscordPresence\\Button1.Url", &settings.button1_url);
  LoadConfigValue(config, L"DiscordPresence\\Button2.Label", &settings.button2_label);
  LoadConfigValue(config, L"DiscordPresence\\Button2.Url", &settings.button2_url);
  LoadConfigValue(config, L"DiscordPresence\\ThrottleMs", &settings.throttle_ms);
  LoadConfigValue(config, L"DiscordPresence\\DebugLog", &settings.debug_log);
  LoadConfigValue(config, L"DiscordPresence\\State.UsePlay", &settings.status.use_play);
  LoadConfigValue(config, L"DiscordPresence\\State.PlayImage", &settings.status.play_image);
  LoadConfigValue(config, L"DiscordPresence\\State.PlayText", &settings.status.play_text);
  LoadConfigValue(config, L"DiscordPresence\\State.UsePause", &settings.status.use_pause);
  LoadConfigValue(config, L"DiscordPresence\\State.PauseImage", &settings.status.pause_image);
  LoadConfigValue(config, L"DiscordPresence\\State.PauseText", &settings.status.pause_text);
  LoadConfigValue(config, L"DiscordPresence\\State.UseRadio", &settings.status.use_radio);
  LoadConfigValue(config, L"DiscordPresence\\State.RadioImage", &settings.status.radio_image);
  LoadConfigValue(config, L"DiscordPresence\\State.RadioText", &settings.status.radio_text);
}

void AimpDiscordPresence::InitializeMessageDispatcher() {
  Aimp::Messages::Service::MessageDispatcher message_dispatcher;

  message_dispatcher.Hook(Aimp::Messages::Events::kPlayerState,
                          [&](DWORD message, int param1, void*, HRESULT*) {
                            OnPlayerState(message, param1);
                          });

  message_dispatcher.Hook(Aimp::Messages::Events::Stream::Start::kSubtrack,
                          [&](DWORD, int, void*, HRESULT*) {
                            OnStreamStartSubtrack();
                          });

  message_dispatcher.Hook(Aimp::Messages::Events::kPropertyValue,
                          [&](DWORD message, int param1, void*, HRESULT*) {
                            OnPropertyValue(message, param1);
                          });

  // The every-second position tick; the asynchronous artwork result needs a
  // notification after it completes, and the seek-only property event is not
  // one.
  message_dispatcher.Hook(Aimp::Messages::Events::kPlayerUpdatePosition,
                          [&](DWORD, int, void*, HRESULT*) {
                            OnPlayerUpdatePosition();
                          });
}

template<typename T>
void AimpDiscordPresence::LoadConfigValue(Aimp::Core::Service::Config config, const std::wstring& key, T value) {
  if (!config.Get(key, value)) {
    config.Set(key, *value);
  }
}

bool AimpDiscordPresence::Unload() {
  Aimp::Messages::Service::MessageDispatcher().UnhookAll();

  // Stop the publisher's log hook before the instance goes away, so a late
  // worker line can never touch a dead plugin.
  CoverPublisher::SetLogger(nullptr);
  g_publisher_log_target = nullptr;

  // Joins the publisher worker: it holds only queued image bytes, so it can be
  // stopped without an AIMP object ever crossing threads. An in-flight browser
  // consent is abandoned first, so the join never waits out the consent window.
  CoverPublisher::CancelDriveConsent();
  StopCoverWorker();

  // Joins the artwork worker, so no callback can touch this plugin afterwards.
  album_art_.Shutdown();

  // Stop the throttle worker before the client goes away. A queued update must
  // not be published while the plugin unloads, and the clear is published by
  // the worker itself, while it is still alive, so the message thread never
  // touches the client. The client's own Shutdown() then clears deliberately on
  // a live connection as its last act, which also covers a timed-out wait.
  ClearPresenceBeforeStop();
  StopThrottleWorker();

  if (client_) {
    client_->Shutdown();
    client_.reset();
  }

  return true;
}

void AimpDiscordPresence::Notification(int, IUnknown*) {}
void AimpDiscordPresence::ShowSettings(HWND) {}

void AimpDiscordPresence::OnPlayerState(DWORD, int param1) {
  if (param1 == kPlayerStateStopped ||
      (param1 == kPlayerStatePaused && !settings.status.use_pause)) {
    track_key_.clear();
    // The worker is the only sender: record that presence must go away and
    // wake it. A queued update must not resurrect the presence after the clear,
    // and the next playback should publish immediately rather than wait out an
    // interval that belonged to the previous track, so the revision is bumped
    // and the interval reset together.
    {
      std::lock_guard<std::mutex> lock(throttle_mutex_);
      sent_at_seconds_.store(0, std::memory_order_relaxed);
      QueueClearLocked();
    }
    throttle_wake_.notify_one();
    return;
  }

  paused_ = (param1 == kPlayerStatePaused);
  RefreshPresence(true);
}

void AimpDiscordPresence::OnStreamStartSubtrack() {
  Aimp::Player::Service::Player player;
  paused_ = (player.State() == kPlayerStatePaused);
  RefreshPresence(true);
}

void AimpDiscordPresence::OnPropertyValue(DWORD, int param1) {
  if (param1 != Aimp::Messages::Properties::Player::kPosition) {
    return;
  }

  Aimp::Player::Service::Player player;
  if (player.State() != kPlayerStatePlaying) {
    return;
  }

  // Track change: AIMP does not raise a dedicated event for every source, so
  // the track identity is compared on the position notification instead.
  const TrackInfo info = ReadTrackInfo();
  if (info.key != track_key_) {
    RefreshPresence(true);
    return;
  }

  // Seek: the timestamps are absolute, so Discord counts down by itself and no
  // periodic update is required. Only a real drift triggers a new update.
  if (sent_at_seconds_.load(std::memory_order_relaxed) != 0) {
    const double elapsed = static_cast<double>(
        UnixSecondsNow() - sent_at_seconds_.load(std::memory_order_relaxed));
    if (std::fabs(player.Position() -
                  (sent_position_.load(std::memory_order_relaxed) + elapsed)) >
        kSeekDriftSeconds) {
      RefreshPresence(false);
      return;
    }
  }

  if (ApplyPendingArtwork()) {
    SendActivity();
  }
}

void AimpDiscordPresence::OnPlayerUpdatePosition() {
  Aimp::Player::Service::Player player;
  if (player.State() != kPlayerStatePlaying) {
    return;
  }

  // The artwork lookup is asynchronous (it can take seconds), so by the time
  // it resolves the seek-only property event may have already passed. This
  // event fires every second while a track plays, so the late result is
  // applied and sent promptly.
  if (ApplyPendingArtwork()) {
    SendActivity();
  }
}

AimpDiscordPresence::TrackInfo AimpDiscordPresence::ReadTrackInfo() {
  TrackInfo info;

  Aimp::Player::Service::Player player;
  Aimp::FileManager::FileInfo fileinfo = player.GetInfo();

  const std::wstring file_name =
      fileinfo.Get<std::wstring>(Aimp::FileManager::FileInfo::Props::kFileName);
  info.title = Utils::ToString(fileinfo.Get<std::wstring>(Aimp::FileManager::FileInfo::Props::kTitle));
  // AIMP joins a multi-value artist tag with bare commas; normalise once here
  // so every consumer (card, templates, identity key) sees ", ".
  info.artist = PresenceLayout::NormalizeArtistSeparators(
      Utils::ToString(fileinfo.Get<std::wstring>(Aimp::FileManager::FileInfo::Props::kArtist)));
  info.album = Utils::ToString(fileinfo.Get<std::wstring>(Aimp::FileManager::FileInfo::Props::kAlbum));
  info.year = Utils::ToString(
      fileinfo.Get<std::wstring>(Aimp::FileManager::FileInfo::Props::kDate));
  info.genre = Utils::ToString(
      fileinfo.Get<std::wstring>(Aimp::FileManager::FileInfo::Props::kGenre));
  info.file = Utils::ToString(file_name);
  info.is_url = IsStreamUrl(file_name);
  info.key = info.artist + "\n" + info.album + "\n" + info.title;

  return info;
}

void AimpDiscordPresence::RefreshPresence(bool request_artwork) {
  const TrackInfo info = ReadTrackInfo();

  // Local (offline) cover: extract the track's own artwork on the message
  // thread once per new track, log its fingerprint, and either apply a URL
  // already known for that image or hand the bytes to the publisher worker.
  // A URL is only ever adopted when an upload proved retrievable, so the
  // online chain's URL (or no image) stays in place until then. Runs
  // before SetInfo() so a known URL lands in this same update.
  // Local cover layers (1 and 2): the track's own embedded art, or a sidecar
  // image the extractor found. `local_art_found` records that a local cover
  // exists; when publishing it fails, the online rung below must still be asked
  // - no image is sent only for a track where every layer failed.
  bool local_art_found = false;
  bool local_published = false;
  if (request_artwork && info.key != local_art_key_) {
    local_art_key_ = info.key;
    // A cover queued for the previous track must not be applied to this one.
    local_cover_sha_.clear();
    if (!info.is_url) {
      LocalArt::Result art =
          ExtractLocalArt(info, settings.use_albumart && settings.local_cover);
      if (art.found) {
        local_art_found = true;
        local_cover_sha_ = art.sha256_hex;
        MaybePublishLocalCover(info, std::move(art));
        // MaybePublishLocalCover applies a URL synchronously only on a known
        // hit; an in-flight upload leaves local_cover_url_ empty, and that is
        // exactly the case that must fall through to online rather than no image.
        std::lock_guard<std::mutex> lock(presence_mutex_);
        local_published = !local_cover_url_.empty();
      }
    }
  }

  {
    std::lock_guard<std::mutex> lock(presence_mutex_);
    SetInfo(info);
    SetSmallImage(info);
    SetTimestamp(info);
  }

  track_key_ = info.key;
  track_artist_ = info.artist;
  track_album_ = info.album;
  track_file_ = info.file;

  if (request_artwork) {
    if (PresenceLayout::ShouldRequestOnlineForTrack(
            settings.use_albumart, settings.use_albumart_online, info.artist,
            local_art_found, local_published)) {
      // Non-blocking: the worker resolves the URL and publishes it, the next
      // notification applies it. The album tag is not required: an album-less
      // track still gets a keyless lookup, keyed by artist + file path. A local
      // cover that was found never suppresses this request: if its publish
      // fails, the online rung below is what may still supply an image.
      album_art_.Request(info.artist, info.album, info.file);
    } else {
      std::lock_guard<std::mutex> lock(presence_mutex_);
      artwork_key_.clear();
      artwork_url_.clear();
    }
  }

  SendActivity();
}

void AimpDiscordPresence::SetInfo(const TrackInfo& info) {
  activity_.type = static_cast<int>(DiscordIpc::ActivityType::kListening);
  activity_.status_display_type = settings.status_display_type;
  activity_.name = Utils::ToString(settings.name);

  // Tag values for the configurable templates. Populated here so the button
  // templates in SendActivity() expand against the same track as the card.
  track_fields_.title = info.title;
  track_fields_.artist = info.artist;
  track_fields_.album = info.album;
  track_fields_.year = info.year;
  track_fields_.genre = info.genre;

  const PresenceLayout::TextFields fields = PresenceLayout::BuildTextFields(
      track_fields_, Utils::ToString(settings.details_template),
      Utils::ToString(settings.state_template),
      Utils::ToString(settings.large_text_template));
  activity_.details = fields.details;
  activity_.state = fields.state;
  activity_.large_text = fields.large_text;

  // The local-cover layer takes priority over the online chain: a cover read
  // from the file itself is always the right one. Both are matched by track
  // identity key, which the shared builder keeps in step with
  // ApplyPendingArtwork(); an empty album does not suppress the image.
  const std::string large_image = ResolveLargeImageLocked(
      AlbumArt::BuildAlbumKey(info.artist, info.album, info.file), nullptr);
  activity_.large_image = large_image;
  last_large_image_ = large_image;
}

void AimpDiscordPresence::SetSmallImage(const TrackInfo& info, int state) {
  Aimp::Player::Service::Player player;
  if (state == -1) {
    state = player.State();
  }

  // One switch decides both the image and its text, so the label always
  // describes the badge actually shown. A configured label shorter than two
  // characters is cleared by the selector, which omits the field instead of
  // sending a value Discord rejects.
  PresenceLayout::SmallBadgeSettings badge_settings;
  badge_settings.use_play = settings.status.use_play;
  badge_settings.play_image = Utils::ToString(settings.status.play_image);
  badge_settings.play_text = Utils::ToString(settings.status.play_text);
  badge_settings.use_pause = settings.status.use_pause;
  badge_settings.pause_image = Utils::ToString(settings.status.pause_image);
  badge_settings.pause_text = Utils::ToString(settings.status.pause_text);
  badge_settings.use_radio = settings.status.use_radio;
  badge_settings.radio_image = Utils::ToString(settings.status.radio_image);
  badge_settings.radio_text = Utils::ToString(settings.status.radio_text);

  const PresenceLayout::SmallBadge badge =
      PresenceLayout::SelectSmallBadge(info.is_url, state, badge_settings);
  activity_.small_image = badge.image;
  activity_.small_text = badge.text;
}

void AimpDiscordPresence::SetTimestamp(const TrackInfo& info) {
  activity_.start_timestamp = 0;
  activity_.end_timestamp = 0;

  const int64_t now = UnixSecondsNow();

  // Paused: count UP from the pause moment (start-only, no end), Potflix gate
  // parity (gate.go:146-160).
  if (paused_) {
    activity_.start_timestamp = now;
    return;
  }

  Aimp::Player::Service::Player player;
  const int position = static_cast<int>(round(player.Position()));
  // Elapsed anchor is always now-pos; the projected end is added only when the
  // bar is enabled, the source is a local file, and the duration is known
  // (total > pos), Potflix gate parity (gate.go:129-137). Streams and
  // bar-off stay start-only (count-up, no bar).
  if (settings.timestamp && !info.is_url) {
    const int duration = static_cast<int>(round(player.Duration()));
    activity_.start_timestamp = now - position;
    if (duration - position > 0) {
      activity_.end_timestamp = now + duration - position;
    }
  } else {
    activity_.start_timestamp = now - position;
  }
}

void AimpDiscordPresence::ApplyResolvedArtwork(const std::string& artist, const std::string& album,
                                               const std::string& file_path,
                                               const std::string& url) {
  // Runs on the album art worker thread; publishing the value is all that is
  // allowed here.
  std::lock_guard<std::mutex> lock(presence_mutex_);
  artwork_key_ = AlbumArt::BuildAlbumKey(artist, album, file_path);
  artwork_url_ = url;
}

bool AimpDiscordPresence::ApplyPendingArtwork() {
  // A finished publish, if any. The URL is adopted only when the publisher
  // proved the upload retrievable (Result.ok); a failure is logged and leaves
  // the online chain's URL (or no image) in place.
  {
    std::string hash;
    bool ok = false;
    std::string url;
    std::string reason;
    std::string detail;
    std::string host;
    {
      std::lock_guard<std::mutex> lock(cover_mutex_);
      if (cover_result_ready_) {
        hash = cover_result_hash_;
        ok = cover_result_ok_;
        url = cover_result_url_;
        reason = cover_result_reason_;
        detail = cover_result_detail_;
        host = cover_result_host_;
        cover_result_hash_.clear();
        cover_result_url_.clear();
        cover_result_reason_.clear();
        cover_result_detail_.clear();
        cover_result_host_.clear();
        cover_result_ready_ = false;
      }
    }

    if (!hash.empty()) {
      if (!ok) {
        // The gate at work: no local URL is adopted, the host that failed is
        // on record, and the online rung (or no image) stays in place. The
        // publisher's own negative cache keeps a dead host from being retried
        // immediately.
        LogCover("local-cover failed hash=" + hash +
                 " host=" + (host.empty() ? "unknown" : host) +
                 " reason=" + (reason.empty() ? "unknown" : reason) +
                 " detail=" + (detail.empty() ? "unknown" : detail));
      } else {
        // Remember the URL even when the track has moved on, so the image is
        // never uploaded twice in one session.
        local_cover_urls_[hash] = url;
        // Once per cover: which host served it, what was prepared, what was
        // transferred, which key the cache used. Sizes and hashes only - never
        // bytes.
        LogCover("local-cover published hash=" + hash +
                 " host=" + (host.empty() ? "unknown" : host) + " url=" + url +
                 " detail=" + (detail.empty() ? "unknown" : detail));
        if (hash == local_cover_sha_) {
          std::lock_guard<std::mutex> lock(presence_mutex_);
          local_cover_url_ = url;
          local_cover_key_ =
              AlbumArt::BuildAlbumKey(track_artist_, track_album_, track_file_);
        } else {
          LogCover("local-cover dropped hash=" + hash + " reason=stale-track");
        }
      }
    }
  }

  std::string applied_image;
  std::string applied_source;
  std::string applied_artist;
  std::string applied_album;
  {
    std::lock_guard<std::mutex> lock(presence_mutex_);

    // The shared builder keeps this comparison in step with SetInfo(). The
    // resolved image is applied for the track identity it was resolved for,
    // album or not: only large_text (the caption) depends on the album tag, so
    // an empty album must not suppress the artwork.
    const std::string album_key =
        AlbumArt::BuildAlbumKey(track_artist_, track_album_, track_file_);
    const std::string large_image = ResolveLargeImageLocked(album_key, &applied_source);
    if (large_image == last_large_image_) {
      return false;
    }

    last_large_image_ = large_image;
    activity_.large_image = large_image;
    applied_image = large_image;
    applied_artist = track_artist_;
    applied_album = track_album_;
  }

  LogCover("large-image applied artist=\"" + applied_artist + "\" album=\"" +
           applied_album + "\" source=" + applied_source + " url=" + applied_image);
  return true;
}

std::string AimpDiscordPresence::ResolveLargeImageLocked(const std::string& album_key,
                                                         std::string* source) const {
  // The setting turns both sources off. It no longer means "no image": the card
  // stays honest either way, because the resolver's empty result is replaced by
  // the shared transparent placeholder, never left empty (Discord would draw
  // its yellow default icon). An empty string cannot leave this function.
  const std::string local_url = settings.use_albumart ? local_cover_url_ : std::string();
  const std::string online_url = settings.use_albumart ? artwork_url_ : std::string();
  return PresenceLayout::ResolveCardLargeImage(album_key, local_cover_key_, local_url,
                                               artwork_key_, online_url, source);
}

void AimpDiscordPresence::MaybePublishLocalCover(const TrackInfo& info,
                                                 LocalArt::Result art) {
  if (!settings.use_albumart || !settings.local_cover) {
    return;
  }
  if (!art.found || art.bytes.empty() || art.sha256_hex.empty()) {
    return;
  }

  const std::string hash = art.sha256_hex;
  const size_t size = art.bytes.size();

  // Only the container shapes the host serves back (and the publisher
  // therefore accepts) are uploaded; anything else would spend an upload on a
  // URL the publisher has to reject.
  const char* mime = nullptr;
  if (art.format == "PNG") {
    mime = "image/png";
  } else if (art.format == "JPEG") {
    mime = "image/jpeg";
  }
  if (mime == nullptr) {
    LogCover("local-cover skipped hash=" + hash + " format=" + art.format +
             " reason=unsupported-format");
    return;
  }

  if (size > kMaxCoverBytes) {
    LogCover("local-cover skipped hash=" + hash + " size=" + std::to_string(size) +
             " limit=" + std::to_string(kMaxCoverBytes) + " reason=too-large");
    return;
  }

  const auto known = local_cover_urls_.find(hash);
  if (known != local_cover_urls_.end()) {
    {
      std::lock_guard<std::mutex> lock(presence_mutex_);
      local_cover_url_ = known->second;
      local_cover_key_ = AlbumArt::BuildAlbumKey(info.artist, info.album, info.file);
    }
    LogCover("local-cover hit hash=" + hash + " size=" + std::to_string(size) +
             " url=" + known->second);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(cover_mutex_);
    if (cover_inflight_hash_ == hash ||
        (cover_result_ready_ && cover_result_hash_ == hash)) {
      // Already queued, running or waiting to be picked up.
      return;
    }
    cover_request_hash_ = hash;
    cover_request_mime_ = mime;
    cover_request_bytes_ = std::move(art.bytes);
    cover_inflight_hash_ = hash;
    cover_has_request_ = true;
  }
  cover_wake_.notify_one();

  LogCover("local-cover queued hash=" + hash + " size=" + std::to_string(size) +
           " mime=" + mime);
}

void AimpDiscordPresence::CoverWorkerMain() {
  for (;;) {
    std::string hash;
    std::string mime;
    std::vector<unsigned char> bytes;
    {
      std::unique_lock<std::mutex> lock(cover_mutex_);
      cover_wake_.wait(lock, [this] { return cover_stopping_ || cover_has_request_; });
      if (cover_stopping_) {
        return;
      }
      hash = std::move(cover_request_hash_);
      mime = std::move(cover_request_mime_);
      bytes = std::move(cover_request_bytes_);
      cover_request_hash_.clear();
      cover_request_mime_.clear();
      cover_request_bytes_.clear();
      cover_has_request_ = false;
    }

    // Blocking, but on this worker only: the message thread is never held up
    // by the upload, which is bounded by the publisher's own budget.
    const CoverPublisher::Result result = CoverPublisher::Publish(bytes, mime.c_str());
    std::vector<unsigned char>().swap(bytes);

    {
      std::lock_guard<std::mutex> lock(cover_mutex_);
      if (cover_inflight_hash_ == hash) {
        cover_inflight_hash_.clear();
      }
      cover_result_hash_ = hash;
      cover_result_ok_ = result.ok;
      cover_result_url_ = result.url;
      cover_result_reason_ = result.reason;
      cover_result_detail_ = result.detail;
      cover_result_host_ = result.host;
      cover_result_ready_ = true;
    }
  }
}

void AimpDiscordPresence::StopCoverWorker() {
  {
    std::lock_guard<std::mutex> lock(cover_mutex_);
    cover_stopping_ = true;
  }
  cover_wake_.notify_all();
  if (cover_worker_.joinable()) {
    cover_worker_.join();
  }
}

void AimpDiscordPresence::LogCover(const std::string& line) {
  if (client_) {
    client_->LogLine(line);
  }
}

LocalArt::Result AimpDiscordPresence::ExtractLocalArt(const TrackInfo& info,
                                                      bool want_bytes) {
  LocalArt::Result result;

  Aimp::Player::Service::Player player;
  const Aimp::FileManager::FileInfo file_info = player.GetInfo();
  IAIMPFileInfo* raw_info = file_info.get();

  const std::wstring file =
      file_info.Get<std::wstring>(Aimp::FileManager::FileInfo::Props::kFileName);
  if (raw_info == nullptr || file.empty()) {
    return result;
  }

  IAIMPServiceAlbumArt* service =
      Aimp::Detail::QueryService<IAIMPServiceAlbumArt>(IID_IAIMPServiceAlbumArt);
  if (service == nullptr) {
    return result;
  }

  const LocalArt::Result art = LocalArt::Extract(service, raw_info, want_bytes);
  service->Release();

  std::string online_url;
  {
    std::lock_guard<std::mutex> lock(presence_mutex_);
    online_url = artwork_url_;
  }

  std::string line = "local-art track=\"" + info.artist + " - " + info.title +
                     "\" album=\"" + info.album + "\" file=\"" + Utils::ToString(file) + "\"";
  LocalArt::Result sidecar_art;
  if (art.found) {
    line += " found=1 source=sdk size=" + std::to_string(art.size) + " sha256=" + art.sha256_hex +
            " format=" + art.format + " aimp_format=" + std::to_string(art.aimp_format) +
            " dims=" + std::to_string(art.width) + "x" + std::to_string(art.height);
    line += " provider=offline-only flags=WAITFOR|OFFLINE|ORIGINAL|NOCACHE";
  } else {
    // AIMP's own offline providers found nothing (the multi-disc case: the art
    // lives in an ancestor folder). Fall back to a conventional sidecar file,
    // then hand its bytes down the exact same path the SDK result would take
    // (same size cap, same format gate, same publish).
    const std::wstring sidecar = LocalArt::FindSidecarArt(file, settings.local_cover_art_directory_travel_max);
    if (!sidecar.empty() && want_bytes) {
      sidecar_art = LocalArt::LoadSidecarBytes(sidecar);
    }

    line += " found=" + std::to_string(sidecar_art.found ? 1 : 0);
    if (sidecar_art.found) {
      line += " source=sidecar path=\"" + Utils::ToString(sidecar) + "\"";
      line += " size=" + std::to_string(sidecar_art.size) + " sha256=" + sidecar_art.sha256_hex +
              " format=" + sidecar_art.format +
              " aimp_format=" + std::to_string(sidecar_art.aimp_format) +
              " dims=" + std::to_string(sidecar_art.width) + "x" +
              std::to_string(sidecar_art.height);
    } else {
      line += " source=sdk";
      if (!sidecar.empty()) {
        line += " sidecar_path=\"" + Utils::ToString(sidecar) + "\" sidecar_found=0";
      }
    }
    line += " provider=offline-only flags=WAITFOR|OFFLINE|ORIGINAL|NOCACHE";
  }
  line += " online_url=\"";
  line += online_url;
  line += "\"";

  LogCover(line);
  return art.found ? art : sidecar_art;
}

void AimpDiscordPresence::SendActivity() {
  DiscordIpc::Activity snapshot;
  {
    std::lock_guard<std::mutex> lock(presence_mutex_);
    snapshot = activity_;
  }

  // Buttons come from config. Their token values are expanded against the
  // current track; Url templates are percent-encoded, labels are not. The
  // builder still enforces the https-only rule and Discord's length limits.
  snapshot.buttons[0].label = PresenceLayout::FormatTemplate(
      Utils::ToString(settings.button1_label), track_fields_, false);
  snapshot.buttons[0].url = PresenceLayout::FormatTemplate(
      Utils::ToString(settings.button1_url), track_fields_, true);
  snapshot.buttons[1].label = PresenceLayout::FormatTemplate(
      Utils::ToString(settings.button2_label), track_fields_, false);
  snapshot.buttons[1].url = PresenceLayout::FormatTemplate(
      Utils::ToString(settings.button2_url), track_fields_, true);

  Aimp::Player::Service::Player player;
  const double position = player.Position();
  const int64_t now_seconds = UnixSecondsNow();

  // Record what should be on screen and wake the only sender. The gate and the
  // client call belong to the worker now, so this path cannot interleave with a
  // send in flight; a newer state simply replaces this one and bumps the
  // revision again, and the worker publishes the newest at the deadline.
  {
    std::lock_guard<std::mutex> lock(throttle_mutex_);
    desired_activity_ = std::move(snapshot);
    desired_position_ = position;
    desired_at_seconds_ = now_seconds;
    desired_valid_ = true;
    ++desired_revision_;
  }
  throttle_wake_.notify_one();
}

void AimpDiscordPresence::StartThrottleWorker() {
  {
    // A reload after Unload must start a fresh gate and a fresh revision
    // sequence: the previous stop flag, desired state and interval belong to
    // the old session. Safe because the previous worker was joined in Unload.
    std::lock_guard<std::mutex> lock(throttle_mutex_);
    throttle_stopping_ = false;
    desired_valid_ = false;
    desired_activity_ = DiscordIpc::Activity();
    desired_revision_ = 0;
    last_sent_revision_ = 0;
    throttle_last_sent_ms_ = 0;
    desired_position_ = 0.0;
    desired_at_seconds_ = 0;
  }
  throttle_worker_ = std::thread(&AimpDiscordPresence::ThrottleWorkerMain, this);
}

void AimpDiscordPresence::StopThrottleWorker() {
  {
    std::lock_guard<std::mutex> lock(throttle_mutex_);
    throttle_stopping_ = true;
    // Bump the revision as well as setting the flag: a worker that is past its
    // stop check must see a revision that is not the one it published, so the
    // next iteration re-reads state instead of assuming it is idle. The stop
    // flag is what actually ends the loop.
    ++desired_revision_;
  }
  throttle_wake_.notify_all();
  if (throttle_worker_.joinable()) {
    throttle_worker_.join();
  }
}

void AimpDiscordPresence::QueueClearLocked() {
  desired_valid_ = false;
  desired_activity_ = DiscordIpc::Activity();
  ++desired_revision_;
  // The gate belonged to the track that just ended: a clear is never held
  // behind it, and the next playback publishes immediately. A worker whose send
  // is already in flight will not overwrite this reset (GateAfterAction), so
  // the clear it superseded is issued on the next iteration.
  throttle_last_sent_ms_ = 0;
}

void AimpDiscordPresence::ClearPresenceBeforeStop() {
  if (!throttle_worker_.joinable() || !client_) {
    return;
  }

  std::unique_lock<std::mutex> lock(throttle_mutex_);
  if (throttle_stopping_) {
    return;
  }

  QueueClearLocked();
  const uint64_t clear_revision = desired_revision_;
  throttle_wake_.notify_one();

  // The worker acts on the clear while it is still alive; after the join no
  // client call can come from this plugin. The predicate is the worker's own
  // completion condition, re-checked under this mutex, so the completion's
  // notify_all cannot be missed.
  throttle_wake_.wait_for(
      lock, std::chrono::milliseconds(kPresenceClearOnUnloadTimeoutMs),
      [this, clear_revision] {
        return throttle_stopping_ || last_sent_revision_ >= clear_revision;
      });

  const bool published = last_sent_revision_ >= clear_revision;
  lock.unlock();
  if (!published) {
    // Not fatal: Client::Shutdown() clears the presence on a live READY
    // connection as its own last act.
    LogCover("presence clear on unload timed out; relying on client shutdown");
  }
}

void AimpDiscordPresence::ThrottleWorkerMain() {
  std::unique_lock<std::mutex> lock(throttle_mutex_);
  for (;;) {
    if (throttle_stopping_) {
      return;
    }

    PresenceThrottlePlan::Request request;
    request.desired_valid = desired_valid_;
    request.desired_revision = desired_revision_;
    request.last_sent_revision = last_sent_revision_;
    request.last_sent_ms = throttle_last_sent_ms_;

    const uint32_t interval =
        PresenceRateLimit::NormalizeThrottleMs(settings.throttle_ms);
    const PresenceThrottlePlan::Decision decision = PresenceThrottlePlan::Decide(
        request, MonotonicMillisNow(), interval);

    if (decision.action == PresenceThrottlePlan::Action::kIdle) {
      // Idle means the published revision is the desired one. Any bump makes
      // them differ again, so that equality is the wake predicate; a spurious
      // wake only re-evaluates it under the lock.
      throttle_wake_.wait(lock, [this] {
        return throttle_stopping_ || desired_revision_ != last_sent_revision_;
      });
      continue;
    }

    if (decision.action == PresenceThrottlePlan::Action::kWait) {
      // Bounded by the deadline. A bump wakes us earlier and the loop decides
      // again against the newer revision, so an old wait never masks newer
      // state; the predicate is the revision we decided on.
      const uint64_t observed_revision = desired_revision_;
      throttle_wake_.wait_for(
          lock, std::chrono::milliseconds(static_cast<int64_t>(decision.wait_ms)),
          [this, observed_revision] {
            return throttle_stopping_ || desired_revision_ != observed_revision;
          });
      continue;
    }

    // Act on exactly the state this decision was made from, then re-check: the
    // revision proves whether a newer request arrived while the call was in
    // flight.
    const uint64_t acted_revision = desired_revision_;
    const bool clear =
        decision.action == PresenceThrottlePlan::Action::kClear;
    const uint64_t gate_before = throttle_last_sent_ms_;
    DiscordIpc::Activity snapshot;
    double position = 0.0;
    int64_t at_seconds = 0;
    if (!clear) {
      snapshot = desired_activity_;
      position = desired_position_;
      at_seconds = desired_at_seconds_;
    }

    // The client takes its own mutex, so it is never called with this one held.
    // One call, no retry: a failure is the client's to recover from, and the
    // revision below is still recorded so the loop cannot spin on it.
    lock.unlock();
    if (client_) {
      if (clear) {
        client_->Clear();
      } else {
        client_->SetActivity(snapshot);
      }
    }
    if (!clear) {
      // A deferred send is a send: it moves the seek baseline to the values the
      // card was built from. Relaxed atomics read by the seek check, so neither
      // store needs the lock and their order after the call does not matter.
      sent_position_.store(position, std::memory_order_relaxed);
      sent_at_seconds_.store(at_seconds, std::memory_order_relaxed);
    } else {
      // No card is on screen now. The request already reset this, but a stale
      // send that completed after the request must not leave a baseline
      // pointing at a card the clear then removed.
      sent_at_seconds_.store(0, std::memory_order_relaxed);
    }
    lock.lock();

    // The published revision is the one the call acted on, not the current one:
    // if a clear arrived during the call this is already stale, the next
    // decision sees the difference, and the clear is issued. The interval gate
    // keeps whatever a clear set while the call was in flight.
    last_sent_revision_ = acted_revision;
    throttle_last_sent_ms_ = PresenceThrottlePlan::GateAfterAction(
        decision.action, gate_before, throttle_last_sent_ms_,
        MonotonicMillisNow());
    // A drain waiting on the unload clear observes this completion.
    throttle_wake_.notify_all();
  }
}
