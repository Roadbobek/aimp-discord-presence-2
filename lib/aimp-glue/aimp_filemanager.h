//  In-repo replacement for the retired aimp-sdk-cpp-wrapper facade.
//
//  Aimp::FileManager::FileInfo is a reference counted owner of an IAIMPFileInfo
//  plus the small typed property accessor the plugin uses.

#ifndef AIMP_GLUE_AIMP_FILEMANAGER_H_
#define AIMP_GLUE_AIMP_FILEMANAGER_H_

#include <windows.h>
#include <unknwn.h>

#include <string>

#include "apiFileManager.h"
#include "apiObjects.h"

namespace Aimp {

namespace Detail {

// Reads an IAIMPString valued property as std::wstring. Returns an empty
// string when the property or the file info itself is unavailable.
std::wstring FileInfoGetString(IAIMPFileInfo* info, int property_id);

}  // namespace Detail

namespace FileManager {

class FileInfo {
 public:
  struct Props {
    enum {
      kTitle = AIMP_FILEINFO_PROPID_TITLE,
      kArtist = AIMP_FILEINFO_PROPID_ARTIST,
      kAlbum = AIMP_FILEINFO_PROPID_ALBUM,
      // DATE is the year/date tag AIMP exposes; it is a string ("2004" or a
      // full date), so the plugin substitutes it verbatim.
      kDate = AIMP_FILEINFO_PROPID_DATE,
      kGenre = AIMP_FILEINFO_PROPID_GENRE,
      kFileName = AIMP_FILEINFO_PROPID_FILENAME,
    };
  };

  FileInfo() = default;
  explicit FileInfo(IAIMPFileInfo* info) : info_(info) {}

  FileInfo(const FileInfo& other) : info_(other.info_) {
    if (info_ != nullptr) {
      info_->AddRef();
    }
  }

  FileInfo& operator=(const FileInfo& other) {
    if (this != &other) {
      if (other.info_ != nullptr) {
        other.info_->AddRef();
      }
      if (info_ != nullptr) {
        info_->Release();
      }
      info_ = other.info_;
    }
    return *this;
  }

  FileInfo(FileInfo&& other) noexcept : info_(other.info_) { other.info_ = nullptr; }

  FileInfo& operator=(FileInfo&& other) noexcept {
    if (this != &other) {
      if (info_ != nullptr) {
        info_->Release();
      }
      info_ = other.info_;
      other.info_ = nullptr;
    }
    return *this;
  }

  ~FileInfo() {
    if (info_ != nullptr) {
      info_->Release();
    }
  }

  template <typename T>
  T Get(int property_id) const;

  // The raw SDK object, for callers that hand it to another AIMP service.
  IAIMPFileInfo* get() const { return info_; }

 private:
  IAIMPFileInfo* info_ = nullptr;
};

template <>
inline std::wstring FileInfo::Get<std::wstring>(int property_id) const {
  return Detail::FileInfoGetString(info_, property_id);
}

}  // namespace FileManager

}  // namespace Aimp

#endif  // AIMP_GLUE_AIMP_FILEMANAGER_H_
