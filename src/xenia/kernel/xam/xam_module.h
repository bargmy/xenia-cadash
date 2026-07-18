/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_MODULE_H_
#define XENIA_KERNEL_XAM_XAM_MODULE_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/cpu/export_resolver.h"
#include "xenia/kernel/kernel_module.h"
#include "xenia/kernel/kernel_state.h"

namespace xe {
namespace kernel {
namespace xam {

struct NativeContentEntry;

static constexpr std::string_view kXamModuleLoaderDataFileName =
    "launch_data.bin";

class XamModule : public KernelModule {
 public:
  XamModule(Emulator* emulator, KernelState* kernel_state);
  virtual ~XamModule();

  static void RegisterExportTable(xe::cpu::ExportResolver* export_resolver);

  struct LoaderData {
    std::string host_path;  // Full host path to next title to load
    std::string
        launch_path;  // Full guest path to next xex. might be default.xex

    uint32_t launch_flags = 0;
    std::vector<uint8_t> launch_data;
  };

  void LoadLoaderData();
  void SaveLoaderData();

  // Persists the requested title handoff, fades the existing window fully to
  // black, and then replaces the current Xenia process image. The relaunched
  // emulator boots the next title with a clean kernel, GPU and audio state and
  // fades the new title back in after it reports a successful launch.
  bool RequestTitleLaunch(const std::filesystem::path& host_path,
                          std::string_view launch_path, uint32_t launch_flags);

  // Boots dash.xex from the folder containing the Xenia executable.
  bool RequestDashboardLaunch(uint32_t launch_flags = 0);

  const LoaderData& loader_data() const { return loader_data_; }
  LoaderData& loader_data() { return loader_data_; }

 private:
  bool RequestNativeContentLaunch(const NativeContentEntry& entry);

  LoaderData loader_data_;
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XAM_MODULE_H_
