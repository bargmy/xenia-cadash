/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/apps/xam_app.h"

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/xam/xam_content_device.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/kernel/xthread.h"

/* Notes:
   - Messages ids that start with 0x00021xxx are UI calls
   - Messages ids that start with 0x00023xxx are used for the user profile
   - Messages ids that start with 0x0002Bxxx are used by the Kinect device
   usually for camera related functions
   - Messages ids that start with 0x0002Cxxx are used by the XamNuiIdentity
   functions
*/

namespace xe {
namespace kernel {
namespace xam {
namespace apps {

namespace {

struct X_LAUNCH_URI_MESSAGE {
  xe::be<uint32_t> flags;
  xe::be<uint32_t> user_index;
  char uri[0x3FC];
};
static_assert_size(X_LAUNCH_URI_MESSAGE, 0x404);

struct UriAlias {
  std::string_view scheme;
  bool initially_available;
  const char* offline_format;
  const char* online_format;
};

// Exact URI alias records at xam.xex 17559 .rdata 0x81637748.
constexpr UriAlias kUriAliases[] = {
    {"pam", false, "dash:pam:%s", nullptr},
    {"hub", false, "dash:pam:BuiltIn.Hub.xzp:%s", nullptr},
    {"epix", false, "hub:root:Epix:%s", nullptr},
    {"gamedetails", true,
     "hub:root:BuiltIn.ContentApp.xzp:gamedetails:MediaId=%s",
     "hub:root:Dash.MP.ContentExplorer.lex:gamedetails:MediaId=%s"},
    {"appdetails", true, "hub:root:BuiltIn.ContentApp.xzp:appdetails:%s",
     "hub:root:Dash.MP.ContentExplorer.lex:appdetails:%s"},
    {"contentdetails", true,
     "hub:root:BuiltIn.ContentApp.xzp:contentdetails:%s",
     "hub:root:Dash.MP.ContentExplorer.lex:contentdetails:%s"},
    {"contentlist", true, nullptr,
     "hub:root:Dash.MP.ContentExplorer.lex:contentlist:%s"},
    {"contentlibrary", true,
     "hub:root:BuiltIn.ContentApp.xzp:contentlibrary:%s",
     "hub:root:Dash.MP.ContentExplorer.lex:contentlibrary:%s"},
    {"gameslibrary", true, "contentlibrary:PackageType=1;%s", nullptr},
    {"applibrary", true, "contentlibrary:PackageType=2;%s", nullptr},
    {"gamesmp", true, nullptr,
     "hub:root:Dash.MP.ContentExplorer.lex:gamesmp:%s"},
    {"appsmp", true, nullptr,
     "hub:root:Dash.MP.ContentExplorer.lex:appsmp:%s"},
    {"userpins", true, nullptr,
     "hub:root:Dash.MP.ContentExplorer.lex:userpins:%s"},
    {"videosmp", true, nullptr,
     "hub:root:Dash.MP.ContentExplorer.lex:videosmp:%s"},
    {"search", true, nullptr, "pam:Dash.Search.xex:scene=results|%s"},
    {"moviedetails", true, nullptr,
     "pam:Dash.Search.xex:scene=details|itemtype=Movie|id=%s"},
    {"tvEpisodedetails", true, nullptr,
     "pam:Dash.Search.xex:scene=details|itemtype=TVEpisode|id=%s"},
    {"tvSeasondetails", true, nullptr,
     "pam:Dash.Search.xex:scene=details|itemtype=TVSeason|id=%s"},
    {"tvSeriesdetails", true, nullptr,
     "pam:Dash.Search.xex:scene=details|itemtype=TVSeries|id=%s"},
    {"tvShowdetails", true, nullptr,
     "pam:Dash.Search.xex:scene=details|itemtype=TVShow|id=%s"},
    {"webvideocollection", true, nullptr,
     "pam:Dash.Search.xex:scene=details|itemtype=WebVideoCollection|id=%s"},
    {"picturelibrary", false, "dash:library:picture:%s", nullptr},
    {"videolibrary", false, "dash:library:video:%s", nullptr},
    {"musiclibrary", false, "dash:library:music:%s", nullptr},
    {"networkhelp", true, "dash:networktroubleshooter", nullptr},
    {"mediaoverlay", false, nullptr,
     "dash:pamoverlay:BuiltIn.Hub.xzp:overlay:Dash.MP.ContentExplorer.lex:overlay:%s"},
    {"http", true, nullptr, "app:58480880:http:%s"},
    {"https", true, nullptr, "app:58480880:https:%s"},
    {"microsoftstore", true, nullptr, "pam:Dash.MP.MicrosoftStore.lex:%s"},
    {"newlivesignup", false, "app:FFFE07DE:%s", "app:FFFE07DE:%s"},
    {"activityfeed", false, nullptr,
     "hub:root:Dash.MP.ContentExplorer.lex:ActivityFeed:%s"},
    {"activitydetails", false, nullptr,
     "hub:root:Dash.MP.ContentExplorer.lex:ActivityDetails:%s"},
    {"activityalerts", false, nullptr,
     "hub:root:Dash.MP.ContentExplorer.lex:ActivityAlerts:%s"},
};

const UriAlias* FindUriAlias(std::string_view scheme) {
  for (const auto& alias : kUriAliases) {
    if (alias.scheme == scheme) {
      return &alias;
    }
  }
  return nullptr;
}

std::string ApplyAliasFormat(const char* format, std::string_view remainder) {
  std::string result(format);
  const size_t replacement = result.find("%s");
  if (replacement != std::string::npos) {
    result.replace(replacement, 2, remainder);
  }
  return result;
}

X_HRESULT ResolveUri17559(KernelState* kernel_state, std::string input,
                          uint32_t flags, std::string* resolved) {
  const X_HRESULT logon_result =
      kernel_state->app_manager()->DispatchMessageSync(0xFC, 0x00058003, 0, 0);
  const bool online =
      !XFAILED(logon_result) && logon_result != 0x001510F1u;

  // xam!819A06D0 initializes this from flag bit 31. xam!8199FEE8 sets it
  // after the first successful alias, allowing protected aliases later in the
  // same recursive chain (gameslibrary -> contentlibrary -> hub -> pam).
  bool alias_access = (flags & 0x80000000u) != 0;

  for (uint32_t depth = 0; depth < 10; ++depth) {
    const size_t colon = input.find(':');
    if (colon == std::string::npos || colon == 0) {
      return X_E_INVALIDARG;
    }

    const std::string_view scheme(input.data(), colon);
    const UriAlias* alias = FindUriAlias(scheme);
    if (!alias) {
      *resolved = std::move(input);
      return X_E_SUCCESS;
    }

    if (!alias_access && !alias->initially_available) {
      return X_E_ACCESS_DENIED;
    }
    alias_access = true;

    const char* format = alias->offline_format;
    if (online && alias->online_format) {
      format = alias->online_format;
    }
    if (!format) {
      return X_E_NOT_SUPPORTED;
    }

    const std::string remainder = input.substr(colon + 1);
    const std::string next = ApplyAliasFormat(format, remainder);
    if (next.size() + 1 > 0x3FC) {
      return X_HRESULT_FROM_WIN32(X_ERROR_INSUFFICIENT_BUFFER);
    }
    XELOGI("XAM URI resolve[{}]: '{}' -> '{}'", depth, input, next);
    input = next;
  }

  return X_E_FAIL;
}

X_HRESULT StoreDashboardLaunchData(KernelState* kernel_state,
                                   uint32_t user_index,
                                   const std::string& resolved_uri) {
  if (resolved_uri.size() + 1 > 0x3F0) {
    return X_HRESULT_FROM_WIN32(X_ERROR_INSUFFICIENT_BUFFER);
  }

  // Exact request consumed by xam!819A0440 and copied by xam!8169F4A8:
  // +0 reserved, +4 launch state 12, +8 user index, +C resolved URI.
  std::vector<uint8_t> launch_data(0x3FC, 0);
  xe::store_and_swap<uint32_t>(launch_data.data() + 4, 12);
  xe::store_and_swap<uint32_t>(launch_data.data() + 8, user_index);
  std::memcpy(launch_data.data() + 0x0C, resolved_uri.c_str(),
              resolved_uri.size() + 1);

  auto xam = kernel_state->GetKernelModule<XamModule>("xam.xex");
  if (!xam) {
    return X_E_FAIL;
  }
  xam->loader_data().launch_data = std::move(launch_data);
  XELOGI("XAM launch data: state=12 user={} uri='{}'", user_index,
         resolved_uri);
  return X_E_SUCCESS;
}

X_HRESULT HandleLaunchUri17559(KernelState* kernel_state,
                               const X_LAUNCH_URI_MESSAGE& message) {
  const uint32_t flags = message.flags.get();
  const uint32_t user_index = message.user_index.get();
  const void* uri_terminator =
      std::memchr(message.uri, '\0', sizeof(message.uri));
  if (!uri_terminator || uri_terminator == message.uri) {
    return X_E_INVALIDARG;
  }
  const size_t uri_length =
      static_cast<const char*>(uri_terminator) - message.uri;

  std::string resolved_uri;
  X_HRESULT result = ResolveUri17559(
      kernel_state, std::string(message.uri, uri_length), flags,
      &resolved_uri);
  if (XFAILED(result)) {
    return result;
  }

  const size_t colon = resolved_uri.find(':');
  if (colon == std::string::npos ||
      std::string_view(resolved_uri.data(), colon) != "dash") {
    XELOGE("XAM URI target is not dashboard: '{}'", resolved_uri);
    return X_E_NOT_SUPPORTED;
  }

  const bool current_title_is_dashboard =
      kernel_state->title_id() == kDashboardID;

  // Exact xam!819A0440 -> xam!8199F6B0 side effect. Retail first removes
  // any previous 12-byte context and then installs {active=1, user, -2}.
  if (current_title_is_dashboard) {
    auto state = kernel_state->xam_state();
    state->dash_launch_context_active_ = 1;
    state->dash_launch_context_user_index_ = user_index;
    state->dash_launch_context_sentinel_ = 0xFFFFFFFEu;
    XELOGI(
        "XAM launch context: active=1 user={} sentinel=FFFFFFFE",
        user_index);
  }

  // The direct dashboard destination handler xam!819A0440 receives r4 as
  // the part after the top-level "dash:" scheme. It copies that remainder
  // verbatim to launch-data +0x0C. Dashboard notification 0x80040021 then
  // dispatches this field against its pam/pamoverlay/library handler table.
  // Storing the full resolved URI would leave the field beginning with
  // "dash:", for which that table has no entry.
  const std::string dashboard_uri = resolved_uri.substr(colon + 1);
  if (dashboard_uri.empty()) {
    return X_E_INVALIDARG;
  }

  result = StoreDashboardLaunchData(kernel_state, user_index, dashboard_uri);
  if (XFAILED(result)) {
    return result;
  }

  // Exact xam!819A0440 branch used by dash 17559 for My Games. The
  // observed call has flags 00000005, so bit 2 is set while the current title
  // is FFFE07D1. Retail stores launch data and broadcasts 80040021; it does
  // not invoke the registered callback on this branch.
  if (!(current_title_is_dashboard && (flags & 0x00000004u))) {
    // Other retail launch branches use XAM's request-provider queue. That
    // queue is outside this exact My Games implementation, so do not emulate
    // it with fabricated callback behavior.
    return X_E_NOT_SUPPORTED;
  }

  const uint32_t notification_data = (user_index << 16) | 12u;
  kernel_state->BroadcastNotification(
      kXNotificationInternal | 0x00040021u, notification_data);
  XELOGI("XAM URI notification 80040021 data={:08X}", notification_data);
  return X_E_SUCCESS;
}

}  // namespace

XamApp::XamApp(KernelState* kernel_state) : App(kernel_state, 0xFE) {}

X_HRESULT XamApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x0002000E: {
      X_ENUMERATE_PARAM* data_ptr =
          reinterpret_cast<X_ENUMERATE_PARAM*>(buffer);

      XELOGD(
          "XEnumerateCrossTitle({:04X}, {:04X}, {:04X}, {:04X}, {}, {}, "
          "{:04X})",
          data_ptr->user_index.get(), data_ptr->flags.get(),
          data_ptr->private_enum_structure_ptr.get(),
          data_ptr->buffer_ptr.get(), data_ptr->buffer_size.get(),
          data_ptr->items_requested.get(), data_ptr->items_returned_ptr.get());

      if (!data_ptr->buffer_ptr || !data_ptr->private_enum_structure_ptr) {
        return X_E_INVALIDARG;
      }

      auto enum_struct =
          memory_->TranslateVirtual<X_KENUMERATOR_CONTENT_AGGREGATE*>(
              data_ptr->private_enum_structure_ptr);

      auto e = kernel_state_->object_table()->LookupObject<XEnumerator>(
          enum_struct->handle);

      if (!e) {
        return X_E_INVALIDARG;
      }

      assert_true(enum_struct->magic == kXObjSignature);

      XCONTENT_CROSS_TITLE_DATA cross_title_data = {};
      uint8_t* cross_title_data_ptr =
          reinterpret_cast<uint8_t*>(&cross_title_data);

      uint32_t item_count = 0;
      X_RESULT result = e->WriteItems(cross_title_data_ptr,
                                      data_ptr->buffer_size, &item_count);

      XCONTENT_DATA_INTERNAL* content_data_ptr =
          memory_->TranslateVirtual<XCONTENT_DATA_INTERNAL*>(
              data_ptr->buffer_ptr);

      assert_true(data_ptr->buffer_size == sizeof(XCONTENT_DATA_INTERNAL));

      std::memset(content_data_ptr, 0, data_ptr->buffer_size);

      if (!result) {
        content_data_ptr->device_id = cross_title_data.content_data.device_id;
        content_data_ptr->content_type =
            cross_title_data.content_data.content_type;
        content_data_ptr->set_display_name(
            cross_title_data.content_data.display_name());
        content_data_ptr->set_file_name(
            cross_title_data.content_data.file_name());
        content_data_ptr->padding[0] = content_data_ptr->padding[1] = 0;
        content_data_ptr->title_id = cross_title_data.title_id;
      }

      result = X_HRESULT_FROM_WIN32(result);

      xe::be<uint32_t>* items_returned_ptr =
          memory_->TranslateVirtual<xe::be<uint32_t>*>(
              data_ptr->items_returned_ptr);

      *items_returned_ptr = item_count;

      return result;
    }
    case 0x00020021: {
      struct XContentQueryVolumeDeviceType {
        char root_name[64];
        xe::be<uint32_t> is_title_process;
        xe::be<DeviceType> device_type_ptr;
        xe::be<uint32_t> overlapped_ptr;
      }* data = reinterpret_cast<XContentQueryVolumeDeviceType*>(buffer);
      assert_true(buffer_length == sizeof(XContentQueryVolumeDeviceType));

      std::string target;
      if (!kernel_state_->file_system()->FindSymbolicLink(
              std::string(data->root_name) + ':', target)) {
        return X_E_INVALIDARG;
      }

      // Only apply this check to XContent packages
      if (!target.starts_with("\\Device\\Package_")) {
        return X_E_INVALIDARG;
      }

      xe::be<DeviceType>* device_type_ptr =
          memory_->TranslateVirtual<xe::be<DeviceType>*>(
              static_cast<uint32_t>(data->device_type_ptr.get()));

      switch (kernel_state_->deployment_type_) {
        case XDeploymentType::kDownload:
        case XDeploymentType::kInstalledToHDD: {
          *device_type_ptr = DeviceType::HDD;
        } break;
        case XDeploymentType::kOpticalDisc: {
          *device_type_ptr = DeviceType::ODD;
        } break;
        default: {
          *device_type_ptr = DeviceType::Invalid;
        } break;
      }

      XELOGD("XContentQueryVolumeDeviceType('{}', {:08X}, {:08X}, {:08X})",
             data->root_name,
             static_cast<uint32_t>(data->is_title_process.get()),
             static_cast<uint32_t>(data->device_type_ptr.get()),
             static_cast<uint32_t>(data->overlapped_ptr.get()));

      return X_E_SUCCESS;
    }
    case 0x00021012: {
      uint32_t enabled = xe::load_and_swap<uint32_t>(buffer);
      XELOGD("XEnableGuestSignin: {}", enabled ? "true" : "false");
      return X_E_SUCCESS;
    }
    case 0x00022003: {
      if (buffer_length != sizeof(X_LAUNCH_URI_MESSAGE)) {
        return X_E_INVALIDARG;
      }
      auto message = reinterpret_cast<X_LAUNCH_URI_MESSAGE*>(buffer);
      return HandleLaunchUri17559(kernel_state_, *message);
    }
    case 0x00022005: {
      struct XTITLE_GET_DEPLOYMENT_TYPE {
        xe::be<uint32_t> deployment_type_ptr;
        xe::be<uint32_t> overlapped_ptr;
      }* data = reinterpret_cast<XTITLE_GET_DEPLOYMENT_TYPE*>(buffer);
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XTITLE_GET_DEPLOYMENT_TYPE));
      auto deployment_type =
          memory_->TranslateVirtual<uint32_t*>(data->deployment_type_ptr);
      *deployment_type = static_cast<uint32_t>(kernel_state_->deployment_type_);
      XELOGD("XTitleGetDeploymentType({:08X}, {:08X}",
             data->deployment_type_ptr.get(), data->overlapped_ptr.get());
      return X_E_SUCCESS;
    }
    case 0x0002B003: {
      // Games used in:
      // 4D5309C9
      // It only receives buffer
      struct {
        xe::be<uint64_t> unk1;
        xe::be<uint64_t> unk2;
        xe::be<uint64_t> unk3;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);

      XELOGD("XamUnk2B003({:016X}, {:016X}, {:016X}), unimplemented",
             args->unk1.get(), args->unk2.get(), args->unk3.get());
      return X_E_SUCCESS;
    }
    // Causes dashboard to correctly process language/region change. It does not
    // contain any buffer.
    case 0x8000000D: {
      const bool is_pc_enabled =
          (kernel_state_->xconfig()->ReadSetting<uint8_t>(
               XCONFIG_USER_CATEGORY, XCONFIG_USER_PC_FLAGS) &
           X_PC_FLAGS::PCEnabled) != 0;

      return is_pc_enabled ? X_E_ACCESS_DENIED : X_E_SUCCESS;
    }
  }
  XELOGE(
      "Unimplemented XAM message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe
