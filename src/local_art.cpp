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

#include "local_art.h"

#include <bcrypt.h>

#include <algorithm>
#include <cstddef>
#include <cwctype>

// SHA-256 comes from CNG; linking it here keeps the extraction leg
// self-contained.
#pragma comment(lib, "bcrypt.lib")

namespace {

// The receive callback writes into this stack-owned context. WAITFOR makes
// AIMP deliver the callback before IAIMPServiceAlbumArt::Get2 returns.
struct ReceiveContext {
  LocalArt::Result result;
  bool want_bytes = false;
};

std::string ToHex(const unsigned char* bytes, size_t size) {
  static const char kDigits[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    hex.push_back(kDigits[bytes[i] >> 4]);
    hex.push_back(kDigits[bytes[i] & 0x0F]);
  }
  return hex;
}

std::string DetectFormat(const byte* data, DWORD size, int aimp_format) {
  if (size >= 8 && data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E &&
      data[3] == 0x47 && data[4] == 0x0D && data[5] == 0x0A &&
      data[6] == 0x1A && data[7] == 0x0A) {
    return "PNG";
  }
  if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
    return "JPEG";
  }
  if (size >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F') {
    return "GIF";
  }
  if (size >= 2 && data[0] == 'B' && data[1] == 'M') {
    return "BMP";
  }

  switch (aimp_format) {
    case AIMP_IMAGE_FORMAT_PNG:
      return "PNG";
    case AIMP_IMAGE_FORMAT_JPG:
      return "JPEG";
    case AIMP_IMAGE_FORMAT_GIF:
      return "GIF";
    case AIMP_IMAGE_FORMAT_BMP:
      return "BMP";
    default:
      return "other";
  }
}

bool Sha256(const byte* data, DWORD size, std::string* hex) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
    return false;
  }

  BCRYPT_HASH_HANDLE hash = nullptr;
  bool ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0 &&
            BCryptHashData(hash, const_cast<PUCHAR>(data), size, 0) >= 0;

  unsigned char digest[32] = {};
  if (ok) {
    ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
  }

  if (hash != nullptr) {
    BCryptDestroyHash(hash);
  }
  BCryptCloseAlgorithmProvider(algorithm, 0);

  if (ok) {
    *hex = ToHex(digest, sizeof(digest));
  }
  return ok;
}

void CALLBACK OnReceive(IAIMPImage* /*image*/, IAIMPImageContainer* container,
                        void* user_data) {
  auto* context = static_cast<ReceiveContext*>(user_data);
  if (container == nullptr) {
    return;
  }

  byte* data = container->GetData();
  const DWORD size = container->GetDataSize();
  if (data == nullptr || size == 0) {
    return;
  }

  SIZE dimensions = {};
  int aimp_format = AIMP_IMAGE_FORMAT_UNKNOWN;
  container->GetInfo(&dimensions, &aimp_format);

  std::string sha256_hex;
  if (!Sha256(data, size, &sha256_hex)) {
    return;
  }

  context->result.found = true;
  context->result.size = size;
  context->result.width = dimensions.cx;
  context->result.height = dimensions.cy;
  context->result.aimp_format = aimp_format;
  context->result.format = DetectFormat(data, size, aimp_format);
  context->result.sha256_hex = sha256_hex;
  if (context->want_bytes) {
    context->result.bytes.assign(data, data + size);
  }
}

std::wstring LowerAscii(const std::wstring& text) {
  std::wstring lowered = text;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return lowered;
}

// Priority of a candidate cover base name; -1 means "not a conventional cover".
// The prefix rule accepts Windows Media Player's AlbumArtSmall / AlbumArt_{GUID}
// names without admitting an unrelated name that merely starts with "albumart".
int SidecarPriority(const std::wstring& lower_base) {
  static const wchar_t* const kCoverBases[] = {L"cover", L"folder", L"front",
                                               L"album", L"albumart"};
  constexpr int kCoverBaseCount = 5;

  // Exact convention bases, in priority order.
  for (int i = 0; i < kCoverBaseCount; ++i) {
    if (lower_base == kCoverBases[i]) {
      return i;
    }
  }

  const std::wstring kPrefix = L"albumart";
  if (lower_base.size() > kPrefix.size() &&
      lower_base.compare(0, kPrefix.size(), kPrefix) == 0) {
    return kCoverBaseCount;
  }
  return -1;
}

bool IsImageExtension(const std::wstring& lower_extension) {
  return lower_extension == L"jpg" || lower_extension == L"jpeg" ||
         lower_extension == L"png" || lower_extension == L"bmp";
}

}  // namespace

namespace LocalArt {

std::wstring PickBestSidecarName(const std::vector<std::wstring>& names) {
  std::wstring best;
  int best_priority = -1;

  for (const std::wstring& name : names) {
    const std::wstring lower = LowerAscii(name);
    // The base is everything before the last dot; a name without a dot has no
    // extension and cannot match.
    const size_t dot = lower.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= lower.size()) {
      continue;
    }
    const std::wstring base = lower.substr(0, dot);
    const std::wstring extension = lower.substr(dot + 1);
    if (!IsImageExtension(extension)) {
      continue;
    }

    const int priority = SidecarPriority(base);
    if (priority < 0) {
      continue;
    }
    if (best_priority < 0 || priority < best_priority ||
        (priority == best_priority && lower < LowerAscii(best))) {
      best = name;
      best_priority = priority;
    }
  }
  return best;
}

std::wstring FindSidecarName(const std::wstring& directory) {
  if (directory.empty()) {
    return std::wstring();
  }

  std::wstring pattern = directory;
  if (pattern.back() != L'\\' && pattern.back() != L'/') {
    pattern += L'\\';
  }
  pattern += L'*';

  WIN32_FIND_DATAW data = {};
  HANDLE handle = FindFirstFileW(pattern.c_str(), &data);
  if (handle == INVALID_HANDLE_VALUE) {
    return std::wstring();
  }

  std::vector<std::wstring> names;
  do {
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      names.push_back(data.cFileName);
    }
  } while (FindNextFileW(handle, &data) != FALSE);

  FindClose(handle);
  return PickBestSidecarName(names);
}

std::wstring FindSidecarArt(const std::wstring& track_path, int custom_max_dirs) {
  const int safe_levels = (custom_max_dirs > 0) ? custom_max_dirs : 0;

  if (track_path.empty()) {
    return std::wstring();
  }

  // Start at the track's own folder. The track path is a file path, so drop the
  // last component; a path that names a directory instead still starts there.
  std::wstring directory = track_path;
  const size_t last_separator = directory.find_last_of(L"\\/");
  const bool names_a_file = last_separator != std::wstring::npos &&
                            last_separator + 1 < directory.size() &&
                            directory.find_last_of(L'.') > last_separator;
  if (names_a_file) {
    directory.erase(last_separator);
  }

  for (int level = 0; level <= safe_levels; ++level) {
    if (directory.empty()) {
      break;
    }
    const std::wstring name = FindSidecarName(directory);
    if (!name.empty()) {
      return directory + L"\\" + name;
    }

    // Ascend one level: drop the trailing component. Stop when no separator
    // remains, which is a bare drive like "E:" - never the drive root listing.
    const size_t separator = directory.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
      break;
    }
    directory.erase(separator);
  }
  return std::wstring();
}

Result Extract(IAIMPServiceAlbumArt* service,
               IAIMPFileInfo* file_info,
               bool want_bytes) {  Result result;
  if (service == nullptr || file_info == nullptr) {
    return result;
  }

  ReceiveContext context;
  context.want_bytes = want_bytes;
  void* task_id = nullptr;

  // OFFLINE disables Internet providers; ORIGINAL suppresses AIMP's display
  // downscaling so the container holds the bytes exactly as stored; NOCACHE
  // keeps a previously downloaded cover out of this answer; WAITFOR delivers
  // the callback before return, keeping the context stack-owned.
  const DWORD flags = AIMP_SERVICE_ALBUMART_FLAGS_WAITFOR |
                      AIMP_SERVICE_ALBUMART_FLAGS_OFFLINE |
                      AIMP_SERVICE_ALBUMART_FLAGS_ORIGINAL |
                      AIMP_SERVICE_ALBUMART_FLAGS_NOCACHE;

  const HRESULT hr = service->Get2(file_info, flags, &OnReceive, &context, &task_id);
  if (FAILED(hr)) {
    return result;
  }
  return context.result;
}

Result LoadSidecarBytes(const std::wstring& path) {
  Result result;
  if (path.empty()) {
    return result;
  }

  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return result;
  }

  LARGE_INTEGER length = {};
  if (GetFileSizeEx(file, &length) && length.QuadPart > 0 &&
      length.QuadPart <= 0x7FFFFFFF) {
    const DWORD size = static_cast<DWORD>(length.QuadPart);
    std::vector<unsigned char> bytes(size);
    DWORD read = 0;
    if (ReadFile(file, bytes.data(), size, &read, nullptr) != FALSE && read == size) {
      std::string sha256_hex;
      if (Sha256(bytes.data(), size, &sha256_hex)) {
        result.found = true;
        result.size = size;
        result.aimp_format = AIMP_IMAGE_FORMAT_UNKNOWN;
        result.format = DetectFormat(bytes.data(), size, result.aimp_format);
        result.sha256_hex = sha256_hex;
        result.bytes = std::move(bytes);
      }
    }
  }

  CloseHandle(file);
  return result;
}

}  // namespace LocalArt
