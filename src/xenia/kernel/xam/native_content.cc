/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/native_content.h"

#include <array>
#include <fstream>
#include <iomanip>
#include <system_error>
#include <utility>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xam/xam_content_device.h"

namespace xe {
namespace kernel {
namespace xam {

namespace {

constexpr uint64_t kNativeContentXuid = 0;
constexpr XContentType kNativeContentType = XContentType::kInstalledGame;
constexpr std::string_view kNativeContentMagic = "XENIA_NATIVE_CONTENT";
constexpr uint32_t kNativeContentVersion = 1;

uint32_t Fnv1a32(std::string_view value) {
  uint32_t hash = 2166136261u;
  for (const unsigned char c : value) {
    hash ^= c;
    hash *= 16777619u;
  }
  return hash;
}

std::filesystem::path NormalizeDirectory(
    const std::filesystem::path& directory) {
  std::error_code ec;
  std::filesystem::path result =
      std::filesystem::weakly_canonical(directory, ec);
  if (!ec) {
    return result;
  }
  ec.clear();
  result = std::filesystem::absolute(directory, ec);
  return ec ? directory.lexically_normal() : result.lexically_normal();
}

std::filesystem::path MetadataPath(const std::filesystem::path& package_path) {
  return package_path / xe::to_path(kNativeContentMetadataFileName);
}

bool ReadMetadata(const std::filesystem::path& package_path,
                  NativeContentEntry* out_entry) {
  if (!out_entry) {
    return false;
  }

  std::ifstream stream(MetadataPath(package_path), std::ios::binary);
  if (!stream) {
    return false;
  }

  std::string magic;
  uint32_t version = 0;
  stream >> magic >> version;
  if (!stream || magic != kNativeContentMagic.data() ||
      version != kNativeContentVersion) {
    return false;
  }

  std::string key;
  std::string display_name;
  std::string working_directory;
  std::string command;
  uint32_t title_id = 0;
  std::string package_name;

  while (stream >> key) {
    if (key == "title_id") {
      stream >> std::hex >> title_id >> std::dec;
    } else if (key == "package_name") {
      stream >> std::quoted(package_name);
    } else if (key == "display_name") {
      stream >> std::quoted(display_name);
    } else if (key == "working_directory") {
      stream >> std::quoted(working_directory);
    } else if (key == "command") {
      stream >> std::quoted(command);
    } else {
      std::string ignored;
      std::getline(stream, ignored);
    }
    if (!stream) {
      return false;
    }
  }

  if (!IsNativeContentTitleId(title_id) || package_name.empty() ||
      display_name.empty() || working_directory.empty() || command.empty()) {
    return false;
  }

  out_entry->title_id = title_id;
  out_entry->package_name = std::move(package_name);
  out_entry->display_name = std::move(display_name);
  out_entry->working_directory = xe::to_path(working_directory);
  out_entry->command = std::move(command);
  out_entry->package_path = package_path;
  return true;
}

bool WriteMetadata(const NativeContentEntry& entry, std::string* out_error) {
  std::ofstream stream(MetadataPath(entry.package_path),
                       std::ios::binary | std::ios::trunc);
  if (!stream) {
    if (out_error) {
      *out_error = "Could not create the native-content metadata file.";
    }
    return false;
  }

  stream << kNativeContentMagic << ' ' << kNativeContentVersion << '\n';
  stream << "title_id " << std::hex << std::uppercase << entry.title_id
         << std::dec << '\n';
  stream << "package_name " << std::quoted(entry.package_name) << '\n';
  stream << "display_name " << std::quoted(entry.display_name) << '\n';
  stream << "working_directory "
         << std::quoted(xe::path_to_utf8(entry.working_directory)) << '\n';
  stream << "command " << std::quoted(entry.command) << '\n';
  stream.flush();
  if (!stream) {
    if (out_error) {
      *out_error = "Could not finish writing native-content metadata.";
    }
    return false;
  }
  return true;
}

bool WriteContentHeader(const std::filesystem::path& content_root,
                        const NativeContentEntry& entry,
                        std::string* out_error) {
  XCONTENT_AGGREGATE_DATA content_data = {};
  content_data.device_id = static_cast<uint32_t>(DummyDeviceId::HDD);
  content_data.content_type = kNativeContentType;
  content_data.set_display_name(xe::to_utf16(entry.display_name));
  content_data.set_file_name(entry.package_name);
  content_data.xuid = kNativeContentXuid;
  content_data.title_id = entry.title_id;

  const std::filesystem::path header_path =
      content_root / fmt::format("{:016X}", kNativeContentXuid) /
      fmt::format("{:08X}", entry.title_id) / "Headers" /
      fmt::format("{:08X}", static_cast<uint32_t>(kNativeContentType)) /
      xe::to_path(entry.package_name + ".header");

  std::error_code ec;
  std::filesystem::create_directories(header_path.parent_path(), ec);
  if (ec) {
    if (out_error) {
      *out_error = fmt::format("Could not create content header directory: {}",
                               ec.message());
    }
    return false;
  }

  std::ofstream stream(header_path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    if (out_error) {
      *out_error = "Could not create the dashboard content header.";
    }
    return false;
  }
  stream.write(reinterpret_cast<const char*>(&content_data),
               sizeof(content_data));
  stream.flush();
  if (!stream) {
    if (out_error) {
      *out_error = "Could not finish writing the dashboard content header.";
    }
    return false;
  }
  return true;
}

void CopyDiscoveredThumbnail(const std::filesystem::path& working_directory,
                             const std::filesystem::path& package_path) {
  static constexpr std::array<std::string_view, 5> kCandidateNames = {
      "__thumbnail.png", "icon.png", "Icon.png", "cover.png", "Cover.png"};

  for (const std::string_view candidate_name : kCandidateNames) {
    const std::filesystem::path source =
        working_directory / xe::to_path(candidate_name);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec) || ec) {
      continue;
    }

    const std::filesystem::path destination =
        package_path / xe::to_path(kNativeContentThumbnailFileName);
    ec.clear();
    std::filesystem::copy_file(
        source, destination,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      XELOGW("Unable to copy native-content thumbnail {}: {}", source,
             ec.message());
    }
    return;
  }
}

}  // namespace

std::optional<uint32_t> GetNativeContentTitleIdFromPackageName(
    std::string_view package_name) {
  if (package_name.size() < 8) {
    return std::nullopt;
  }

  uint32_t title_id = 0;
  for (size_t i = 0; i < 8; ++i) {
    const char c = package_name[i];
    uint32_t nibble = 0;
    if (c >= '0' && c <= '9') {
      nibble = static_cast<uint32_t>(c - '0');
    } else if (c >= 'A' && c <= 'F') {
      nibble = static_cast<uint32_t>(c - 'A' + 10);
    } else if (c >= 'a' && c <= 'f') {
      nibble = static_cast<uint32_t>(c - 'a' + 10);
    } else {
      return std::nullopt;
    }
    title_id = (title_id << 4) | nibble;
  }

  if (!IsNativeContentTitleId(title_id)) {
    return std::nullopt;
  }
  return title_id;
}

std::optional<NativeContentEntry> LoadNativeContentEntry(
    const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::path package_path = path;
  if (!std::filesystem::is_directory(package_path, ec) || ec) {
    package_path = path.parent_path();
  }

  NativeContentEntry entry;
  if (!ReadMetadata(package_path, &entry)) {
    return std::nullopt;
  }
  return entry;
}

bool CreateNativeContentEntry(const std::filesystem::path& content_root,
                              const std::filesystem::path& working_directory,
                              std::string_view display_name,
                              std::string_view command,
                              NativeContentEntry* out_entry,
                              std::string* out_error) {
  if (out_error) {
    out_error->clear();
  }

  const std::filesystem::path normalized_directory =
      NormalizeDirectory(working_directory);
  std::error_code ec;
  if (!std::filesystem::is_directory(normalized_directory, ec) || ec) {
    if (out_error) {
      *out_error = "The selected game location is not a directory.";
    }
    return false;
  }

  std::string normalized_name(display_name);
  normalized_name = xe::string_util::trim(normalized_name);
  std::string normalized_command(command);
  normalized_command = xe::string_util::trim(normalized_command);
  if (normalized_name.empty()) {
    if (out_error) {
      *out_error = "Enter a name for the dashboard tile.";
    }
    return false;
  }
  if (normalized_command.empty()) {
    if (out_error) {
      *out_error = "Enter the command used to start the game.";
    }
    return false;
  }

  const std::string directory_key = xe::path_to_utf8(normalized_directory);
  // Keep the low 15-bit game number above 0x7D0 so generic title helpers do
  // not classify the synthetic entry as an original-Xbox title.
  constexpr uint32_t kFirstNativeGameNumber = 0x1000;
  constexpr uint32_t kNativeGameNumberCount = 0x7000;
  const uint32_t candidate =
      kFirstNativeGameNumber +
      (Fnv1a32(directory_key) % kNativeGameNumberCount);

  NativeContentEntry entry;
  entry.display_name = std::move(normalized_name);
  entry.working_directory = normalized_directory;
  entry.command = std::move(normalized_command);

  bool found_slot = false;
  for (uint32_t attempt = 0; attempt < kNativeGameNumberCount; ++attempt) {
    const uint32_t suffix =
        kFirstNativeGameNumber +
        ((candidate - kFirstNativeGameNumber + attempt) %
         kNativeGameNumberCount);

    entry.title_id = kNativeContentTitleIdPrefix | suffix;
    entry.package_name = fmt::format("{:08X}XENIANATIVE", entry.title_id);
    entry.package_path =
        content_root / fmt::format("{:016X}", kNativeContentXuid) /
        fmt::format("{:08X}", entry.title_id) /
        fmt::format("{:08X}", static_cast<uint32_t>(kNativeContentType)) /
        xe::to_path(entry.package_name);

    NativeContentEntry existing;
    const bool package_exists =
        std::filesystem::exists(entry.package_path, ec);
    if (ec) {
      ec.clear();
      continue;
    }
    if (!package_exists) {
      found_slot = true;
      break;
    }
    if (ReadMetadata(entry.package_path, &existing) &&
        NormalizeDirectory(existing.working_directory) ==
            normalized_directory) {
      found_slot = true;
      break;
    }
  }

  if (!found_slot) {
    if (out_error) {
      *out_error = "The native-content title ID range is full.";
    }
    return false;
  }

  ec.clear();
  std::filesystem::create_directories(entry.package_path, ec);
  if (ec) {
    if (out_error) {
      *out_error = fmt::format("Could not create the native-content package: {}",
                               ec.message());
    }
    return false;
  }

  if (!WriteMetadata(entry, out_error) ||
      !WriteContentHeader(content_root, entry, out_error)) {
    return false;
  }

  CopyDiscoveredThumbnail(normalized_directory, entry.package_path);
  XELOGI(
      "Added native content: title={:08X}, name='{}', directory='{}', "
      "command='{}'.",
      entry.title_id, entry.display_name, entry.working_directory,
      entry.command);

  if (out_entry) {
    *out_entry = entry;
  }
  return true;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
