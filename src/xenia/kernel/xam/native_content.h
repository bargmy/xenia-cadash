/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_NATIVE_CONTENT_H_
#define XENIA_KERNEL_XAM_NATIVE_CONTENT_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace xe {
namespace kernel {
namespace xam {

// Host applications exposed to the retail dashboard as synthetic installed
// Xbox 360 titles. The metadata lives inside the synthetic content package so
// it travels with Xenia's normal content directory.
inline constexpr std::string_view kNativeContentMetadataFileName =
    "__xenia_native_content.txt";
inline constexpr std::string_view kNativeContentThumbnailFileName =
    "__thumbnail.png";
inline constexpr uint32_t kNativeContentTitleIdPrefix = 0x4E580000;  // "NX"

inline constexpr bool IsNativeContentTitleId(uint32_t title_id) {
  return (title_id & 0xFFFF0000u) == kNativeContentTitleIdPrefix;
}

std::optional<uint32_t> GetNativeContentTitleIdFromPackageName(
    std::string_view package_name);

struct NativeContentEntry {
  uint32_t title_id = 0;
  std::string package_name;
  std::string display_name;
  std::filesystem::path working_directory;
  std::string command;
  std::filesystem::path package_path;
};

// Creates or updates a synthetic Installed Game package beneath content_root.
// The generated title ID is stable for a working directory. Commands are
// intentionally stored verbatim because they are explicitly entered by the
// local user and later executed through the platform command interpreter.
bool CreateNativeContentEntry(const std::filesystem::path& content_root,
                              const std::filesystem::path& working_directory,
                              std::string_view display_name,
                              std::string_view command,
                              NativeContentEntry* out_entry,
                              std::string* out_error);

// Loads a package's host-launch metadata. path may be either the package
// directory itself or a file directly inside it.
std::optional<NativeContentEntry> LoadNativeContentEntry(
    const std::filesystem::path& path);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_NATIVE_CONTENT_H_
