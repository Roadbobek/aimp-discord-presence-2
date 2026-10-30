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

#ifndef AIMPDISCORDPRESENCE_SRC_ALBUM_ART_H_
#define AIMPDISCORDPRESENCE_SRC_ALBUM_ART_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

// Resolves album art URLs from keyless public APIs on a background worker so
// AIMP's message thread is never blocked on the network. The providers are
// tried in order:
//
//   1. Deezer album search (`cover_xl`).
//   2. iTunes Search API (`artworkUrl100`, upscaled to 600x600).
//   3. MusicBrainz release-group search for an MBID, then the Cover Art Archive
//      front cover for that MBID.
//
// No provider needs an API key. A hit is only accepted when a light identity
// guard confirms the hit really is the requested artist + album, because
// free-text search happily ranks an unrelated album first.
//
// The resolved URL is handed to a callback that runs on the worker thread; the
// callback must not call AIMP services, it should only publish the value to
// the plugin which applies it on the next player notification.
namespace AlbumArt {

// Discord accepts external asset URLs up to 300 characters.
constexpr size_t kMaxUrlLength = 300;

// MusicBrainz policy requires a descriptive User-Agent with a contact URL.
constexpr const char* kUserAgent =
    "AIMP-Discord-Presence/2.0.0 (https://github.com/Roadbobek/aimp-discord-presence-2)";

enum class Provider {
  kNone = 0,
  kDeezer,
  kItunes,
  kMusicBrainz,
};

// --- Pure helpers, unit tested separately ---------------------------------

// Builds the identity key used both as the resolver cache key and to match a
// resolved artwork URL with the track currently playing. An album tag is the
// whole identity (artist + "\n" + album) and the track title is deliberately
// ignored: artwork is per-album, and one shared builder keeps the resolver and
// the plugin from drifting apart. An album-less track has no album identity, so
// the file path captured at extraction time is appended instead
// (artist + "\n\n" + file_path); without that discriminator every untagged file
// by one artist would share a key and inherit another file's cover.
std::string BuildAlbumKey(const std::string& artist, const std::string& album,
                          const std::string& file_path);

// Percent-encodes a UTF-8 string for use as a query parameter value.
std::string UriEncodeUtf8(const std::string& utf8);

// Returns the value of the first `"key":"value"` pair in a JSON document,
// with JSON escapes decoded. Returns an empty string when the key is absent.
std::string ExtractJsonStringField(const std::string& json, const std::string& key);

// Rewrites an iTunes artwork URL (`.../100x100bb.jpg`) to its 600x600 variant,
// which is the largest size iTunes serves for every artwork.
std::string UpscaleItunesArtwork(const std::string& url);

std::string BuildDeezerSearchUrl(const std::string& artist, const std::string& album);
std::string BuildItunesSearchUrl(const std::string& artist, const std::string& album);
// MusicBrainz WS/2 release-group search. Requested at most once per second.
std::string BuildMusicBrainzReleaseGroupUrl(const std::string& artist,
                                            const std::string& album);
// Cover Art Archive cover for an MBID. Returns an empty string when `mbid` is
// not a UUID, so a malformed search response cannot inject a path.
std::string BuildCoverArtArchiveUrl(const std::string& mbid);

// Extracts the artwork URL for a provider from a search response. Returns an
// empty string when no usable (https, <= 300 characters) URL is present, and
// for providers (MusicBrainz) whose answer is not an image URL.
std::string ExtractArtworkUrl(const std::string& body, Provider provider);

// Extracts the MBID of the first release group in a MusicBrainz WS/2 search
// response.
std::string ExtractMusicBrainzReleaseGroupId(const std::string& body);

// True when a provider hit's identity agrees with the requested artist/album,
// so a guard check can be exercised without a provider-specific response
// document. Agreement ignores case, punctuation and whitespace, and accepts a
// word-prefix on either side ("Discovery" matches "Discovery (Deluxe Edition)",
// but "Program Music I" does not match "Program Music III").
//
// An empty `expected` never matches: a genuinely empty candidate is always
// rejected. When `expected` is non-empty it must agree with `candidate`. The
// album half is the same rule, with one deliberate exception: an empty
// *requested* album means the lookup was artist-only, so the album comparison
// is skipped and the artist agreement alone admits the hit. A requested album
// that is present is still compared strictly.
bool MetadataMatches(const std::string& expected, const std::string& candidate);

// Light sanity check on a free-text search response: true only when the first
// hit's own artist and title agree with the requested pair (see
// MetadataMatches). A rejected hit falls through to the next provider.
// When `album` is empty the search is artist-only, so the hit's album is not
// compared and any album it names is accepted; the hit still has to name a
// non-empty album for the provider extractor to consider it a usable hit.
bool SearchResultMatches(const std::string& body, Provider provider,
                         const std::string& artist, const std::string& album);

// --- Background resolver ---------------------------------------------------

class Resolver {
 public:
  // `file_path` is the caller's identity discriminator (see BuildAlbumKey),
  // echoed back so the caller rebuilds the exact key it requested with.
  using Callback = std::function<void(const std::string& artist,
                                      const std::string& album,
                                      const std::string& file_path,
                                      const std::string& url)>;

  Resolver();
  ~Resolver();

  Resolver(const Resolver&) = delete;
  Resolver& operator=(const Resolver&) = delete;

  // When disabled no network request is ever made; requests resolve to "".
  void SetOnlineEnabled(bool enabled);

  void SetCallback(Callback callback);

  // Diagnostics: one concise line per online lookup decision (requested,
  // resolved, not-found); never image bytes. Invoked on the worker thread, so
  // the callback must itself be thread-safe.
  using Logger = std::function<void(const std::string&)>;
  void SetLogger(Logger logger);

  // Queues a lookup for `artist` + `album` (UTF-8). `file_path` is the opaque
  // identity discriminator that keys the cache and is echoed to the callback.
  // Replaces any pending request; stale completions are dropped.
  void Request(const std::string& artist, const std::string& album,
               const std::string& file_path);

  // Blocking lookup (network + cache). Exposed for the worker and tests.
  std::string Lookup(const std::string& artist, const std::string& album,
                     const std::string& file_path);

  void Shutdown();

 private:
  void WorkerMain();
  std::string LookupUncached(const std::string& artist, const std::string& album,
                             bool* definitive);
  // Online-rung diagnostics; called on the worker thread.
  void Log(const std::string& line);
  void LogOutcome(const std::string& artist, const std::string& album,
                  const std::string& url);
  // MusicBrainz allows one request per second; the worker is the only caller,
  // so an elapsed-time gate is enough to guarantee that.
  void WaitForMusicBrainzSlot();
  bool TakeCached(const std::string& key, std::string* url);
  void StoreCached(const std::string& key, const std::string& url);

  std::thread worker_;

  std::mutex mutex_;
  std::condition_variable wake_;
  // Set by Shutdown() and polled by the worker between provider requests so an
  // in-flight resolve aborts promptly instead of running the full timeout
  // budget while AIMP unloads.
  std::atomic<bool> stopping_{false};
  bool online_ = true;

  bool has_request_ = false;
  uint64_t generation_ = 0;
  std::string request_artist_;
  std::string request_album_;
  std::string request_file_path_;

  Callback callback_;
  Logger logger_;

  std::unordered_map<std::string, std::string> cache_;
  std::deque<std::string> cache_order_;

  // Worker thread only.
  std::chrono::steady_clock::time_point last_musicbrainz_request_{};

  static constexpr size_t kMaxCacheEntries = 200;
};

}  // namespace AlbumArt

#endif  // AIMPDISCORDPRESENCE_SRC_ALBUM_ART_H_
