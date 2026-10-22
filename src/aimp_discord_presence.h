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

#ifndef AIMPDISCORDPRESENCE_SRC_AIMP_DISCORD_PRESENCE_H_
#define AIMPDISCORDPRESENCE_SRC_AIMP_DISCORD_PRESENCE_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "aimp_implements.h"
#include "aimp_plugin.h"
#include "aimp_core.h"

#include "album_art.h"
#include "discord_ipc.h"
#include "local_art.h"
#include "presence_layout.h"

class AimpDiscordPresence :
  public Aimp::Implements<Aimp::Plugin, Aimp::ExternalSettingsDialog> {
 public:
  PWCHAR GetInfo(int index) override;
  DWORD GetCategory() override;
  bool Load() override;
  bool Unload() override;
  void Notification(int id, IUnknown* data) override;
  void ShowSettings(HWND parent_wnd) override;

  // Routes a diagnostic line to the DebugLog client. Public so the publisher's
  // plain-function-pointer log hook can forward here without an instance.
  void LogCover(const std::string& line);

 private:
  struct TrackInfo {
    std::string key;
    std::string title;
    std::string artist;
    std::string album;
    std::string year;
    std::string genre;
    // Full file path; the identity discriminator for album-less tracks.
    std::string file;
    bool is_url = false;
  };

  void OnStreamStartSubtrack();
  void OnPlayerState(DWORD message, int param1 = NULL);
  void OnPropertyValue(DWORD message, int param1 = NULL);
  void OnPlayerUpdatePosition();

 private:
  void InitializeMessageDispatcher();

 private:
  // Rebuilds the Discord presence. `request_artwork` starts a new album art
  // lookup for the current track.
  void RefreshPresence(bool request_artwork);
  // Builds the card and publishes it as the desired state. It never touches the
  // Discord client: the throttle worker is the only sender.
  void SendActivity();

  // Rate-limit plumbing. The worker is the only thread that calls SetActivity
  // or Clear; every other path records the desired state and wakes it.
  void StartThrottleWorker();
  void StopThrottleWorker();
  void ThrottleWorkerMain();
  // Records "presence should be cleared" and resets the interval, so the clear
  // is never held behind the track that just ended and the next playback
  // publishes immediately. Caller holds throttle_mutex_.
  void QueueClearLocked();
  // Unload only: asks the worker to publish a clear and waits, bounded, for the
  // clear to be acted on while the worker is still alive.
  void ClearPresenceBeforeStop();

  TrackInfo ReadTrackInfo();

  // Fill the activity from AIMP's currently playing file. Must run on AIMP's
  // message thread.
  void SetInfo(const TrackInfo& info);
  void SetSmallImage(const TrackInfo& info, int state = -1);
  void SetTimestamp(const TrackInfo& info);

  // Called on the album art worker thread: only publishes the value.
  void ApplyResolvedArtwork(const std::string& artist, const std::string& album,
                            const std::string& file_path, const std::string& url);
  // Called on AIMP's message thread: applies a newly resolved URL, local or
  // online. Returns true when the card changed and an update should be sent.
  bool ApplyPendingArtwork();

  // Called on AIMP's message thread for a new track: extracts the local
  // (tags/sidecar) cover, appends its fingerprint to the DebugLog, and returns
  // it. Never uploads, decodes or alters the image.
  LocalArt::Result ExtractLocalArt(const TrackInfo& info, bool want_bytes);
  // Called on AIMP's message thread with a freshly extracted cover: applies a
  // URL already known for that image, or queues the bytes for the publisher
  // worker. A published URL is only ever adopted when the upload proved
  // retrievable, so a dead host cannot blank the card.
  void MaybePublishLocalCover(const TrackInfo& info, LocalArt::Result art);
  // Publisher worker: blocks on its own mailbox and owns no AIMP object.
  void CoverWorkerMain();
  void StopCoverWorker();

  // Large-image value for `album_key`; must be called with presence_mutex_
  // held. A published local cover wins over the online chain's URL, which wins
  // over the black PNG fallback. `source` names the winner for the DebugLog.
  std::string ResolveLargeImageLocked(const std::string& album_key,
                                      std::string* source) const;

 private:
  void LoadConfig();

  template <typename T>
  void LoadConfigValue(Aimp::Core::Service::Config config, const std::wstring& key, T value);

  struct Properties {
    int64_t application_id = 429559336982020107LL;
    bool timestamp = true;
    bool use_albumart = true;
    bool use_albumart_online = true;
    // Publishes the track's own (offline) cover for Discord to fetch.
    // Publishing is best effort and gated on the upload proving retrievable:
    // when it fails, the online chain's URL (or the black fallback) stays.
    bool local_cover = true;
    // Optional file for the published-cover cache. Empty (the default) means
    // cover-cache.txt next to the plugin DLL; a bare or relative name resolves
    // there as well, an absolute path is used as given.
    std::wstring cover_cache;
    // Permanent, user-owned GitHub upload host. `cover_token` is a fine-grained
    // personal access token; empty (the default) skips the GitHub host entirely.
    // `cover_repo` is "owner/name" and defaults to the owner's art repo. The
    // token is never logged, cached, or written anywhere by the plugin.
    std::wstring cover_token;
    std::wstring cover_repo = L"nathwn12/aimp-discord-presence-art";
    // Preferred cover host: "auto" (the default) uses Google Drive when its
    // credentials exist, otherwise GitHub when a token exists, and falls back to
    // the anonymous keyless hosts either way. "drive", "github" and "keyless"
    // pin the preference; the keyless hosts always remain the last resort.
    std::wstring cover_host = L"auto";
    // Google Drive upload host: the owner's own OAuth desktop client. With
    // either value empty (the default) Drive is simply absent from the chain.
    // The secret is never logged or written; the refresh token lives in
    // drive-token.txt next to the cover cache, and the one-time browser consent
    // runs on the cover worker thread.
    std::wstring drive_client_id;
    std::wstring drive_client_secret;
    // What the member-list status line shows: 0 = name, 1 = state (artist),
    // 2 = details (track title). Defaults to 2, the track title.
    int status_display_type = 2;
    // Activity card text templates. `name` is the application name line above
    // the activity. Each button is emitted only when its url is an https://
    // URL; token values in a Url are percent-encoded, labels are not.
    std::wstring name = L"Music";
    std::wstring details_template = L"{title}";
    std::wstring state_template = L"{artist}";
    std::wstring large_text_template = L"{album}";
    std::wstring button1_label = L"Listen on Spotify";
    std::wstring button1_url = L"https://open.spotify.com/search/";
    std::wstring button2_label = L"Find on YouTube";
    std::wstring button2_url = L"https://music.youtube.com/search?q=";
    // Minimum milliseconds between activity updates. 0 disables the limit;
    // every other value below 1000, negative included, is raised to 1000.
    // Default 4000 stays under Discord's five-updates-per-20-seconds cap.
    int throttle_ms = 4000;
    // Optional file for the Discord IPC frame log. Empty (the default) leaves
    // logging off. A bare filename resolves next to the plugin DLL; an
    // absolute path is used as given.
    std::wstring debug_log;
    struct State {
      bool use_play = false;
      std::wstring play_image = L"aimp_play";
      std::wstring play_text = L"Playing";
      bool use_pause = false;
      std::wstring pause_image = L"aimp_pause";
      std::wstring pause_text = L"Paused";
      bool use_radio = true;
      std::wstring radio_image = L"https://raw.githubusercontent.com/Exle/aimp-discord-presence/main/"
                           L".github/aimp_icons/animated/aimp_radio.gif";
      // Empty (the default) omits small_text in radio state.
      std::wstring radio_text;
    };
    State status;
  };

  Properties settings;

  std::unique_ptr<DiscordIpc::Client> client_;
  AlbumArt::Resolver album_art_;

  // Guards activity_ and the artwork state, which the album art worker thread
  // publishes into. The publisher worker never touches this state; it only
  // fills its own guarded slot below.
  std::mutex presence_mutex_;
  DiscordIpc::Activity activity_;
  std::string artwork_key_;
  std::string artwork_url_;
  // Published local cover, valid for one album key. Set from a publisher
  // Result only when it reported ok, so a failure leaves the online chain's
  // URL (or the black fallback) in place.
  std::string local_cover_key_;
  std::string local_cover_url_;

  // AIMP message thread only.
  std::string track_key_;
  std::string track_artist_;
  std::string track_album_;
  // Identity discriminator for album-less tracks; AIMP message thread only.
  std::string track_file_;
  // Tag values of the track the card currently describes, for button template
  // expansion. AIMP message thread only.
  PresenceLayout::TrackFields track_fields_;
  std::string last_large_image_;
  // Last track whose local cover was fingerprinted; AIMP message thread only.
  std::string local_art_key_;
  // SHA-256 of the local cover of the track that is playing now; the result of
  // an upload is applied only while it still matches. AIMP message thread only.
  std::string local_cover_sha_;
  // Image hash -> published URL, this session. A hit is applied synchronously;
  // the publisher's on-disk cache is consulted on the worker, where blocking
  // for a moment is allowed. AIMP message thread only.
  std::unordered_map<std::string, std::string> local_cover_urls_;
  // Baseline of the activity Discord is showing: the player position and the
  // wall-clock second the card was built for. Written by the throttle worker
  // after a real SetActivity (never after a Clear), and read by the seek check
  // on the message thread, so both members are relaxed atomics: the seek check
  // takes no lock, and the pair only has to be individually intact, not
  // consistent as a unit.
  std::atomic<double> sent_position_{0.0};
  std::atomic<int64_t> sent_at_seconds_{0};
  bool paused_ = false;

  // Publisher worker plumbing. The worker is handed bytes, a mime and a hash;
  // it owns no AIMP object, and it writes its result into the guarded slot the
  // position handler picks up.
  std::thread cover_worker_;
  std::mutex cover_mutex_;
  std::condition_variable cover_wake_;
  bool cover_stopping_ = false;
  bool cover_has_request_ = false;
  std::string cover_request_hash_;
  std::string cover_request_mime_;
  std::vector<unsigned char> cover_request_bytes_;
  // Hash of the image a request is queued or running for, so a track that
  // visits the same cover twice does not upload it twice.
  std::string cover_inflight_hash_;
  bool cover_result_ready_ = false;
  bool cover_result_ok_ = false;
  std::string cover_result_hash_;
  std::string cover_result_url_;
  std::string cover_result_reason_;
  std::string cover_result_detail_;
  // Which upload host produced this outcome (winner on ok, last tried on fail).
  std::string cover_result_host_;

  // Throttle worker: the only thread that calls into the Discord client. The
  // message thread writes the desired state and bumps desired_revision_; the
  // worker publishes at most one update per interval and compares revisions so
  // a request that arrived during a client call is never lost. No client call
  // is made while this mutex is held.
  std::thread throttle_worker_;
  std::mutex throttle_mutex_;
  std::condition_variable throttle_wake_;
  bool throttle_stopping_ = false;
  // false means "presence should be cleared"; `desired_activity_` and the
  // baseline pair below belong to the same request.
  bool desired_valid_ = false;
  uint64_t desired_revision_ = 0;
  uint64_t last_sent_revision_ = 0;
  uint64_t throttle_last_sent_ms_ = 0;
  DiscordIpc::Activity desired_activity_;
  // Seek baseline that belongs to the desired activity, captured with it on the
  // message thread so the worker can publish a fresh baseline without ever
  // touching an AIMP object.
  double desired_position_ = 0.0;
  int64_t desired_at_seconds_ = 0;
};

#endif  // AIMPDISCORDPRESENCE_SRC_AIMP_DISCORD_PRESENCE_H_
