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

#ifndef AIMPDISCORDPRESENCE_SRC_COVER_PUBLISHER_H_
#define AIMPDISCORDPRESENCE_SRC_COVER_PUBLISHER_H_

#include <string>
#include <vector>

// Anonymous cover hosting for the presence's cover art.
//
// Publish() prepares the image (downscale to at most 600 px on the longest side
// and JPEG re-encode whenever that shrinks the transfer), uploads it to
// litterbox, then proves the upload is actually retrievable (a bare GET of the
// returned URL must answer 200 with an image/* content type) before reporting
// success. Distinct images are uploaded once and the returned URL is cached on
// disk, keyed by the SHA-256 of the bytes that were uploaded - except a cached
// URL on an ephemeral keyless host, which is re-published to the permanent
// GitHub host when a token is configured, and revalidated (re-published when
// dead) otherwise, so a dead cached URL is never sent. A transport
// failure is retried once after a short pause; host answers (a 412, an empty
// body) are never retried. Failures are cached briefly too, so a host outage is
// not hammered from the player's callback.
//
// Every call blocks; each HTTP operation gets a ~20 second budget, so an upload
// attempt plus its retry plus the health check can take longer than that. The
// module never logs, prints, or persists image bytes - only sizes and hashes.
// The on-disk cache is best effort: an unwritable path degrades to no cache.
//
// The chain can also lead with Google Drive, the owner's own storage: one
// interactive OAuth consent (loopback redirect, PKCE) writes a refresh token to
// a sidecar next to the cache, a single bootstrap makes an aimp-covers folder
// publicly readable, and each cover is uploaded once and named by its SHA-256.
// A Drive failure - no consent, a rejected grant, quota - is logged, cached
// briefly, and falls through to the next host; it never blocks or crashes AIMP.

namespace CoverPublisher {

struct Result {
  bool ok = false;
  std::string url;
  std::string reason;
  // The host that produced this outcome: the winner on success, the last host
  // tried on failure (UTF-8, e.g. "litter.catbox.moe" or "uguu.se").
  std::string host;
  // Total upload attempts spent across every host tried for this request.
  int attempts = 0;
  // One-line size/dimension record for the caller's log (never image bytes),
  // populated on every outcome including cache hits.
  std::string detail;
};

// Routes the publisher's own diagnostic lines (host success / all-hosts-failed)
// to a sink; when unset they are dropped. Distinct from Result.detail, which is
// per-outcome, and invoked once per publish attempt on the calling thread.
void SetLogger(void (*logger)(const std::string& line));

// Names the cache file (UTF-8). An empty path disables the cache entirely.
// Reloads whatever the file already holds.
void Configure(const std::string& cache_path);

// Configures the permanent, user-owned GitHub host. `token` is a fine-grained
// personal access token; when it is empty the GitHub host is skipped entirely
// and the chain is exactly the keyless hosts. `repo` is "owner/name" and
// defaults to Roadbobek/aimp-discord-presence-art when empty. The token is
// never logged, cached, or persisted - only its presence is ever named.
void ConfigureAuth(const std::string& token, const std::string& repo);

// Chooses the preferred host. `value` is "auto" (the default), "drive",
// "github" or "keyless"; anything else is read as "auto". "auto" prefers Drive
// when its credentials are configured, otherwise GitHub when a token is
// configured. The anonymous keyless hosts always stay last, so a preferred host
// that fails falls through exactly as the chain did before.
void ConfigureHost(const std::string& value);

// Emits one diagnostic line naming the policy and every host, saying which are
// in the chain for this load and why any that are not. Host names, the policy
// and fixed tokens only: no token or secret value, nor a prefix or length of
// one, can reach the line. Call once per load, after the logger is installed.
void LogHostConfiguration();

// Configures the Google Drive host. `client_id` and `client_secret` are the
// owner's own OAuth desktop-client values; with either empty the Drive host is
// simply absent from the chain. `state_path` names the sidecar that holds the
// refresh token and the bootstrap folder id, normally drive-token.txt next to
// the cover cache; an empty path also leaves the host absent, because a token
// that cannot be persisted would re-ask for consent on every start.
//
// Only the refresh token and the folder id are ever written there: the access
// token lives in memory, the client secret lives in memory, and no token value
// is ever logged. A missing or corrupt sidecar means "not authorized", and the
// next publish starts the browser consent flow - once, on the cover worker
// thread, never on AIMP's message thread.
void ConfigureDrive(const std::string& client_id, const std::string& client_secret,
                    const std::string& state_path);

// Abandons an in-flight browser consent wait, so an unload is never held open
// by the consent window. Safe to call from any thread; the flag stays set until
// the next ConfigureDrive (plugin load), so a consent that starts after the
// unload began aborts immediately too.
void CancelDriveConsent();

// Uploads image_bytes (mime names their type, e.g. "image/png") and returns the
// public URL on success. On failure reason names the cause: "empty-body" when
// the host answered HTTP 200 with an empty body, "http-<code>" for statuses
// without a useful body, the response body text for 412, and phase-tagged
// transport failures ("receive-failed phase=receive winhttp=12002",
// "health-check-...") otherwise.
Result Publish(const std::vector<unsigned char>& image_bytes, const char* mime);

// Forgets every cached result and deletes the cache file (best effort).
void ClearCache();

}  // namespace CoverPublisher

#endif  // AIMPDISCORDPRESENCE_SRC_COVER_PUBLISHER_H_
