/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>

#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/string_util.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/xam/native_content.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_content_device.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/vfs/file.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

void AddODDContentTest(object_ref<XStaticEnumerator<XCONTENT_AGGREGATE_DATA>> e,
                       XContentType content_type) {
  auto root_entry = kernel_state()->file_system()->ResolvePath(
      "GAME:\\Content\\0000000000000000");
  if (!root_entry) {
    return;
  }

  auto content_type_path = fmt::format("{:08X}", uint32_t(content_type));

  xe::filesystem::WildcardEngine title_find_engine;
  title_find_engine.SetRule("????????");

  xe::filesystem::WildcardEngine content_find_engine;
  content_find_engine.SetRule("????????????????");

  size_t title_find_index = 0;
  vfs::Entry* title_entry;
  for (;;) {
    title_entry =
        root_entry->IterateChildren(title_find_engine, &title_find_index);
    if (!title_entry) {
      break;
    }

    auto title_id =
        string_util::from_string<uint32_t>(title_entry->name(), true);

    auto content_root_entry = title_entry->ResolvePath(content_type_path);
    if (content_root_entry) {
      size_t content_find_index = 0;
      vfs::Entry* content_entry;
      for (;;) {
        content_entry = content_root_entry->IterateChildren(
            content_find_engine, &content_find_index);
        if (!content_entry) {
          break;
        }

        auto item = e->AppendItem();
        assert_not_null(item);
        if (item) {
          item->device_id = static_cast<uint32_t>(DummyDeviceId::ODD);
          item->content_type = content_type;
          item->set_display_name(to_utf16(content_entry->name()));
          item->set_file_name(content_entry->name());
          item->title_id = title_id;
        }
      }
    }
  }
}

static void AppendDashboardContent(
    const object_ref<XStaticEnumerator<XCONTENT_DATA_INTERNAL>>& enumerator,
    const XCONTENT_AGGREGATE_DATA& source) {
  XCONTENT_DATA_INTERNAL* item = enumerator->AppendItem();
  assert_not_null(item);
  if (!item) {
    return;
  }

  // XStaticUntypedEnumerator grows raw storage without constructing T.
  std::memset(item, 0, sizeof(*item));

  item->device_id = source.device_id;
  item->content_type = source.content_type;
  item->display_name_raw = source.display_name_raw;
  std::memcpy(item->file_name_raw, source.file_name_raw,
              sizeof(source.file_name_raw));
  item->XCONTENT_DATA::padding[0] = 0;
  item->XCONTENT_DATA::padding[1] = 0;

  item->category = 0;
  item->xuid = source.xuid;
  item->title_id = source.title_id;

  // Bit 0 is the conventional full-content license. The content-root header
  // format currently has no license field, so local non-demo titles use the
  // minimal full-content mask while demos remain unlicensed/trial content.
  item->license_mask =
      source.content_type == XContentType::kGameDemo ? 0u : 1u;
  item->content_size = 0;
  item->creation_time = 0;

  std::u16string title_name = source.display_name();
  if (title_name.empty()) {
    title_name = xe::to_utf16(
        fmt::format("{:08X}", source.title_id.get()));
  }
  string_util::copy_and_swap_truncating(
      item->title_name, title_name, countof(item->title_name));
  item->xcontent_flag = static_cast<XCONTENT_INTERNAL_FLAGS>(0);
}

// Alias XContentCreateCrossTitleEnumerator
dword_result_t XamContentAggregateCreateEnumerator_entry(qword_t xuid,
                                                         dword_t device_id,
                                                         dword_t content_type,
                                                         dword_t title_id,
                                                         lpdword_t handle_out) {
  assert_not_null(handle_out);
  if (!handle_out) {
    return X_E_INVALIDARG;
  }

  // The retail dashboard's GameLibrary backend uses this export as an
  // aggregate database enumerator, not as the smaller SDK cross-title
  // enumerator. Its call sites pass content_type == -1 as a wildcard and give
  // XamEnumerate 0x200-byte records. Keep this compatibility path scoped to
  // the dashboard so normal title behavior does not regress.
  if (kernel_state()->title_id() == kDashboardID) {
    // Retail XAM creates this enumerator before validating / resolving the
    // dashboard's device selector. Dashboard device IDs are not limited to
    // Xenia's synthetic DummyDeviceId values, so rejecting an unknown ID here
    // prevents the Content App from ever issuing XamEnumerate. The local HLE
    // provider below maps the dashboard aggregate query to Xenia's HDD content
    // root, while retaining the caller's device ID in the diagnostic log.
    auto device_info =
        device_id == 0 ? nullptr : GetDummyDeviceInfo(device_id);

    XELOGI(
        "XamContentAggregateCreateEnumerator(dashboard): "
        "xuid={:016X}, device={:08X}, type={:08X}, title={:08X}",
        static_cast<uint64_t>(xuid), device_id.value(), content_type.value(),
        title_id.value());

    // Retail XAM 17559 creates the aggregate enumerator with one item per
    // enumerate operation. XamEnumerate may still request a larger output
    // buffer; XStaticEnumerator enforces the retail cadence.
    auto e = make_object<XStaticEnumerator<XCONTENT_DATA_INTERNAL>>(
        kernel_state(), 1);
    X_KENUMERATOR_CONTENT_AGGREGATE* extra;
    auto result =
        e->Initialize(XUserIndexAny, 0xFE, 0x2000E, 0x20010, 0, &extra);
    if (XFAILED(result)) {
      return result;
    }

    extra->magic = kXObjSignature;
    extra->handle = e->handle();

    const uint64_t resolved_xuid =
        static_cast<uint64_t>(xuid) == UINT64_MAX
            ? 0
            : static_cast<uint64_t>(xuid);
    const XContentType requested_type =
        static_cast<XContentType>(content_type.value());

    std::vector<XCONTENT_AGGREGATE_DATA> content;
    if (!device_info || device_info->device_type == DeviceType::HDD) {
      if (device_id && !device_info) {
        XELOGI(
            "XamContentAggregateCreateEnumerator(dashboard): "
            "mapping retail device {:08X} to local HDD content root",
            device_id.value());
      }
      if (title_id) {
        content = kernel_state()->content_manager()->ListContent(
            static_cast<uint32_t>(DummyDeviceId::HDD), resolved_xuid,
            title_id.value(), requested_type);
      } else {
        content = kernel_state()->content_manager()->ListContentAcrossTitles(
            static_cast<uint32_t>(DummyDeviceId::HDD), resolved_xuid,
            requested_type);
      }
    }

    // Native host applications are global machine content, not profile-owned
    // Xbox packages. Always merge them from XUID 0 so they remain visible in
    // My Games regardless of which dashboard profile is signed in.
    if (requested_type == XContentType::kFolder ||
        requested_type == XContentType::kInstalledGame) {
      auto native_content =
          kernel_state()->content_manager()->ListContentAcrossTitles(
              static_cast<uint32_t>(DummyDeviceId::HDD), 0, requested_type);
      for (auto& native_item : native_content) {
        if (!IsNativeContentTitleId(native_item.title_id.get())) {
          continue;
        }
        const bool duplicate =
            std::find(content.begin(), content.end(), native_item) !=
            content.end();
        if (!duplicate) {
          content.emplace_back(std::move(native_item));
        }
      }
    }

    for (const auto& item : content) {
      AppendDashboardContent(e, item);
      XELOGI(
          "{}: dashboard item title={:08X}, type={:08X}, name={} file={}",
          __func__, item.title_id.get(),
          static_cast<uint32_t>(item.content_type.get()),
          xe::to_utf8(item.display_name()), item.file_name());
    }

    XELOGI("XamContentAggregateCreateEnumerator(dashboard): added {} item(s)",
           e->item_count());
    *handle_out = e->handle();
    return X_ERROR_SUCCESS;
  }

  auto device_info = device_id == 0 ? nullptr : GetDummyDeviceInfo(device_id);
  if (device_id && device_info == nullptr) {
    return X_E_INVALIDARG;
  }

  auto e = make_object<XStaticEnumerator<XCONTENT_CROSS_TITLE_DATA>>(
      kernel_state(), 1);
  X_KENUMERATOR_CONTENT_AGGREGATE* extra;
  auto result = e->Initialize(XUserIndexAny, 0xFE, 0x2000E, 0x20010, 0, &extra);
  if (XFAILED(result)) {
    return result;
  }

  extra->magic = kXObjSignature;
  extra->handle = e->handle();

  const XContentType content_type_enum =
      static_cast<XContentType>(content_type.value());

  if (!device_info || device_info->device_type == DeviceType::HDD) {
    // Fetch any alternate title IDs defined in the XEX header
    // (used by games to load saves from other titles, etc)
    std::vector<uint32_t> title_ids{title_id ? title_id.value()
                                             : kCurrentlyRunningTitleId};
    auto exe_module = kernel_state()->GetExecutableModule();
    if (exe_module && exe_module->xex_module()) {
      const auto& alt_ids = exe_module->xex_module()->opt_alternate_title_ids();
      std::copy(alt_ids.cbegin(), alt_ids.cend(),
                std::back_inserter(title_ids));
    }

    for (const auto& title_id : title_ids) {
      // Get all content data.
      auto content_datas = kernel_state()->content_manager()->ListContent(
          static_cast<uint32_t>(DummyDeviceId::HDD),
          xuid == -1 ? 0 : static_cast<uint64_t>(xuid), title_id,
          content_type_enum);
      for (const auto& content_data : content_datas) {
        auto item = e->AppendItem();
        assert_not_null(item);
        if (item) {
          item->content_data.device_id = content_data.device_id;
          item->content_data.content_type = content_data.content_type;
          item->content_data.display_name_raw = content_data.display_name_raw;
          std::memcpy(item->content_data.file_name_raw,
                      content_data.file_name_raw,
                      sizeof(content_data.file_name_raw));
          item->content_data.padding[0] = 0;
          item->content_data.padding[1] = 0;

          item->title_id = content_data.title_id;
        }
      }
    }
  }

  // if (!device_info || device_info->device_type == DeviceType::ODD) {
  //   AddODDContentTest(e, content_type_enum);
  // }

  XELOGD("XamContentAggregateCreateEnumerator: added {} items to enumerator",
         e->item_count());

  *handle_out = e->handle();
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamContentAggregateCreateEnumerator, kContent, kImplemented);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(ContentAggregate);
