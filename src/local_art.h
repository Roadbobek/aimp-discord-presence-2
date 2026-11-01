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
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
//  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//  DEALINGS IN THE SOFTWARE.

#ifndef AIMPDISCORDPRESENCE_SRC_LOCAL_ART_H_
#define AIMPDISCORDPRESENCE_SRC_LOCAL_ART_H_

#include <windows.h>
#include <unknwn.h>

#include <string>
#include <vector>

#include "apiAlbumArt.h"
#include "apiFileManager.h"

// Extraction leg of the local-album-art feature: asks AIMP's album art
// service for the artwork of one file, restricted to local providers (file
// tags and sidecar files), and fingerprints the raw image container bytes.
// The bytes are never decoded, resized, re-encoded or uploaded.
namespace LocalArt {

struct Result {
  bool found = false;
  DWORD size = 0;
  long width = 0;   // container-reported pixel dimensions
  long height = 0;
  int aimp_format = AIMP_IMAGE_FORMAT_UNKNOWN;  // from Container.GetInfo
  std::string format;      // "PNG", "JPEG" or "other", sniffed from magic bytes
  std::string sha256_hex;  // lowercase hex, empty when not found
  // Raw container bytes exactly as AIMP delivered them, at most one image.
  // Filled only when Extract() is asked for them; never logged, printed or
  // persisted by this module.
  std::vector<unsigned char> bytes;
};

// Blocking, AIMP main thread only. Runs an offline-only album art request for
// `file_info` and returns the fingerprint of the delivered container. The
// AIMP receive callback is invoked on the calling thread before this returns,
// so no state outlives the call. `service` stays owned by the caller. Set
// `want_bytes` to have the result also carry the container bytes.
Result Extract(IAIMPServiceAlbumArt* service, IAIMPFileInfo* file_info,
               bool want_bytes = false);

// Read the image bytes of a sidecar file found by FindSidecarArt, sniffed with
// the same magic-byte/extension logic the SDK result uses, and gate them by the
// same cap. The bytes are never decoded; `local_art.cpp` does not own the cap,
// so the caller applies it after this returns. `path` is a full path.
Result LoadSidecarBytes(const std::wstring& path);

// Deepest number of ancestor levels above the track's own folder that the
// sidecar scan will enter. The track folder itself is level 0.
constexpr int kMaxSidecarLevels = 1;

// One directory offered to the pure sidecar chooser: the directory path plus
// its entries' file names (directories included; the extension gate filters
// them out). `SubdirectoryNames` populates the latter for the filesystem walk,
// while tests build the list by hand so selection stays offline and pure.
struct SidecarDir {
  std::wstring path;
  std::vector<std::wstring> names;
};

// Pure selection: pick the best conventional cover file in one directory, or an
// empty string when the directory has none. Recognised base names, best first,
// are `cover`, `folder`, `front`, `album`, `albumart`, plus any `albumart*`
// prefix (Windows Media Player's `AlbumArtSmall.jpg`, `AlbumArt_{GUID}_Large.jpg`).
// A name's base is everything before the LAST dot, so only names ending in a
// recognised extension can win. Matching is case-insensitive; on equal priority
// the lexicographically smaller name wins so the choice is deterministic.
std::wstring PickBestSidecarName(const std::vector<std::wstring>& names);

// Thin filesystem wrapper: reads `directory`'s entries and delegates to
// PickBestSidecarName. Read-only; never decodes, writes or touches audio.
std::wstring FindSidecarName(const std::wstring& directory);

// Walk from the track's own folder up through its ancestors, newest level
// first, stopping after custom_max_dirs ascent steps. Returns the first
// conventional cover file found, or an empty string when none is in range.
std::wstring FindSidecarArt(const std::wstring& track_path, int custom_max_dirs = kMaxSidecarLevels);

}  // namespace LocalArt

#endif  // AIMPDISCORDPRESENCE_SRC_LOCAL_ART_H_
