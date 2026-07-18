/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_STATE_H_
#define XENIA_KERNEL_XAM_XAM_STATE_H_

#include <array>
#include <memory>
#include <string>

#include "xenia/kernel/xam/achievement_manager.h"
#include "xenia/kernel/xam/app_manager.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/user_tracker.h"
#include "xenia/kernel/xam/xam.h"

namespace xe {
class Emulator;
};

namespace xe {
namespace kernel {
class KernelState;
}
}  // namespace xe

namespace xe {
namespace kernel {
namespace xam {

class XamState {
 public:
  XamState(Emulator* emulator, KernelState* kernel_state);
  ~XamState() = default;

  AppManager* app_manager() const { return app_manager_.get(); }
  ContentManager* content_manager() const { return content_manager_.get(); }
  AchievementManager* achievement_manager() const {
    return achievement_manager_.get();
  }
  ProfileManager* profile_manager() const { return profile_manager_.get(); }

  UserTracker* user_tracker() const { return user_tracker_.get(); }
  SpaInfo* spa_info() const { return spa_info_.get(); }

  UserProfile* GetUserProfile(uint32_t user_index) const;
  UserProfile* GetUserProfile(uint64_t xuid) const;

  bool IsUserSignedIn(uint32_t user_index) const;
  bool IsUserSignedIn(uint64_t xuid) const;

  void LoadSpaInfo(const SpaInfo* info);

  void SetContentRegisterCallback(uint32_t callback);

  bool IsUIActive() const {
    return xam_dialogs_shown_ > 0 || xam_nui_dialogs_shown_ > 0;
  }

  uint32_t GetLanguageFallbackAddress(uint32_t index) const {
    return language_fallback_address_[index];
  }

  uint32_t GetLanguageTypefacePatch(uint32_t language) const;

  uint32_t GetIptvNameAddress() const { return iptv_name_address_; }

  X_DASH_APP_INFO dash_app_info_ = {};
  uint32_t dash_backstack_nodes_count_ = 0;
  X_DASH_BACKSTACK_DATA dash_backstack_data_[2] = {};

  // Retail XAM 17559 URI state. XamPushBackURI stores one URI and a state:
  // 1 when pushed by a system/dashboard title, 2 when pushed by a game title.
  uint32_t dash_launch_request_callback_ = 0;
  std::string dash_back_uri_;
  uint32_t dash_back_uri_state_ = 0;

  // Exact 12-byte XAM 17559 launch-user context stored at 0x81AA3EF8 by
  // xam!8199F6B0 before a dashboard URI request is delivered.
  uint32_t dash_launch_context_active_ = 0;
  uint32_t dash_launch_context_user_index_ = XUserIndexNone;
  uint32_t dash_launch_context_sentinel_ = 0xFFFFFFFEu;

  // Retail XAM 17559 demand object used by XamGetCurrentDemand. The idle
  // state has type 0xFE and a zero demand ID.
  uint32_t current_demand_type_ = 0xFE;
  uint32_t current_demand_id_ = 0;

  // Per-user preference bitfield used by app FB messages B0082/B0083.
  // A newly created local profile has no optional preference bits set.
  std::array<uint32_t, XUserMaxUserCount> user_preference_flags_{};

  uint32_t content_register_callback = 0;

  std::atomic<int32_t> xam_dialogs_shown_ = {0};
  std::atomic<int32_t> xam_nui_dialogs_shown_ = {0};

 private:
  void LoadLanguageLocaleFallback();
  void LoadLanguageTypefacePatch();
  void LoadIptvServiceName();

  KernelState* kernel_state_;

  std::unique_ptr<AppManager> app_manager_;
  std::unique_ptr<ContentManager> content_manager_;
  std::unique_ptr<UserTracker> user_tracker_;
  std::unique_ptr<AchievementManager> achievement_manager_;
  std::unique_ptr<ProfileManager> profile_manager_;

  std::unique_ptr<SpaInfo> spa_info_;

  // Custom XAM stuff
  std::array<uint32_t, 0x12> language_fallback_address_{};
  std::array<uint32_t, 0x7> language_type_face_patch_{};
  uint32_t iptv_name_address_{};
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
