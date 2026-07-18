/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_module.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <system_error>
#include <utility>

#include "xenia/base/platform.h"

#if XE_PLATFORM_GNU_LINUX
#include <dirent.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#elif XE_PLATFORM_MAC
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#elif XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif

#include "third_party/imgui/imgui.h"
#include "xenia/emulator.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/string.h"
#include "xenia/config.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/native_content.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"

namespace xe {
namespace kernel {
namespace xam {

namespace {

constexpr char kRelaunchFadeEnvironment[] = "XENIA_XAM_FADE_IN";
std::atomic_bool g_title_relaunch_pending = false;
std::atomic_bool g_native_content_launch_pending = false;

std::string NormalizeGuestLaunchPath(std::string launch_path) {
  std::replace(launch_path.begin(), launch_path.end(), '\\', '/');

  auto lower_ascii = [](std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char c) {
      if (c >= 'A' && c <= 'Z') {
        return char(c - 'A' + 'a');
      }
      return c;
    });
    return value;
  };

  const auto remove_prefix = [&](std::string_view prefix) {
    const std::string lower_path = lower_ascii(launch_path);
    const std::string lower_prefix = lower_ascii(std::string(prefix));
    if (lower_path.starts_with(lower_prefix)) {
      launch_path.erase(0, prefix.size());
      return true;
    }
    return false;
  };

  remove_prefix("game:/");
  remove_prefix("d:/");
  remove_prefix("xsyslaunch:/");

  while (!launch_path.empty() && launch_path.front() == '/') {
    launch_path.erase(launch_path.begin());
  }
  return launch_path;
}

float SmoothStep(float value) {
  value = std::clamp(value, 0.0f, 1.0f);
  return value * value * (3.0f - 2.0f * value);
}

struct RelaunchFadeJob {
  std::mutex mutex;
  std::condition_variable condition;
  std::atomic_bool opaque = false;
  std::atomic_bool cancelled = false;
  std::atomic_bool restart_failed = false;
};

bool StartRelaunchFadeOut(ui::ImGuiDrawer* imgui_drawer,
                          std::shared_ptr<RelaunchFadeJob> job) {
  if (!imgui_drawer || !job) {
    return false;
  }

  using Clock = std::chrono::steady_clock;
  enum class Phase { kFadeOut, kOpaque, kFailureFadeIn };

  constexpr auto kFadeOutDuration = std::chrono::milliseconds(350);
  constexpr auto kFailureFadeInDuration = std::chrono::milliseconds(250);

  Phase phase = Phase::kFadeOut;
  Clock::time_point phase_start = Clock::now();
  uint32_t opaque_frames = 0;

  return imgui_drawer->SetFullScreenOverlay(
      [job, phase, phase_start, opaque_frames](ImGuiIO& io) mutable -> bool {
        const auto now = Clock::now();
        float alpha = 1.0f;

        if (job->cancelled.load(std::memory_order_acquire)) {
          g_title_relaunch_pending.store(false, std::memory_order_release);
          return false;
        }

        switch (phase) {
          case Phase::kFadeOut: {
            const float progress =
                std::chrono::duration<float>(now - phase_start).count() /
                std::chrono::duration<float>(kFadeOutDuration).count();
            alpha = SmoothStep(progress);
            if (progress >= 1.0f) {
              phase = Phase::kOpaque;
              phase_start = now;
            }
            break;
          }
          case Phase::kOpaque:
            alpha = 1.0f;
            // Present at least two fully black frames before replacing the
            // process image. This hides the dashboard's failed-launch UI and
            // any host shutdown frames.
            if (!job->opaque.load(std::memory_order_relaxed) &&
                ++opaque_frames >= 2) {
              job->opaque.store(true, std::memory_order_release);
              job->condition.notify_all();
            }
            if (job->restart_failed.load(std::memory_order_acquire)) {
              phase = Phase::kFailureFadeIn;
              phase_start = now;
            }
            break;
          case Phase::kFailureFadeIn: {
            const float progress =
                std::chrono::duration<float>(now - phase_start).count() /
                std::chrono::duration<float>(kFailureFadeInDuration).count();
            alpha = 1.0f - SmoothStep(progress);
            if (progress >= 1.0f) {
              g_title_relaunch_pending.store(false,
                                             std::memory_order_release);
              return false;
            }
            break;
          }
        }

        const uint8_t alpha_byte = static_cast<uint8_t>(
            std::clamp(std::lround(alpha * 255.0f), 0l, 255l));
        ImGui::GetForegroundDrawList()->AddRectFilled(
            ImVec2(0.0f, 0.0f), io.DisplaySize,
            IM_COL32(0, 0, 0, alpha_byte));
        return true;
      });
}

void MarkFileDescriptorsCloseOnExec() {
#if XE_PLATFORM_GNU_LINUX
  DIR* descriptor_directory = opendir("/proc/self/fd");
  if (!descriptor_directory) {
    return;
  }
  const int directory_descriptor = dirfd(descriptor_directory);
  while (dirent* entry = readdir(descriptor_directory)) {
    char* end = nullptr;
    const long descriptor = std::strtol(entry->d_name, &end, 10);
    if (!end || *end != '\0' || descriptor < 3 ||
        descriptor == directory_descriptor) {
      continue;
    }
    const int flags = fcntl(int(descriptor), F_GETFD);
    if (flags >= 0) {
      fcntl(int(descriptor), F_SETFD, flags | FD_CLOEXEC);
    }
  }
  closedir(descriptor_directory);
#elif XE_PLATFORM_MAC
  rlimit descriptor_limit = {};
  if (getrlimit(RLIMIT_NOFILE, &descriptor_limit) != 0) {
    return;
  }
  for (int descriptor = 3;
       descriptor < static_cast<int>(descriptor_limit.rlim_cur);
       ++descriptor) {
    const int flags = fcntl(descriptor, F_GETFD);
    if (flags >= 0) {
      fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC);
    }
  }
#endif
}

void SetRelaunchFadeEnvironment(bool enabled) {
#if XE_PLATFORM_WIN32
  _putenv_s(kRelaunchFadeEnvironment, enabled ? "1" : "");
#else
  if (enabled) {
    setenv(kRelaunchFadeEnvironment, "1", 1);
  } else {
    unsetenv(kRelaunchFadeEnvironment);
  }
#endif
}

bool RestartXeniaProcess() {
  const std::filesystem::path executable_path =
      xe::filesystem::GetExecutablePath();
  if (executable_path.empty()) {
    XELOGE("Unable to restart Xenia: executable path is empty.");
    return false;
  }

  SetRelaunchFadeEnvironment(true);

#if XE_PLATFORM_GNU_LINUX || XE_PLATFORM_MAC
  MarkFileDescriptorsCloseOnExec();
  std::string executable_utf8 = xe::path_to_utf8(executable_path);
  char* argv[] = {executable_utf8.data(), nullptr};
  execv(executable_utf8.c_str(), argv);
  XELOGE("Unable to restart Xenia via execv({}): {}", executable_utf8,
         std::strerror(errno));
  SetRelaunchFadeEnvironment(false);
  return false;
#elif XE_PLATFORM_WIN32
  std::wstring command_line = L"\"" + executable_path.wstring() + L"\"";
  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);
  PROCESS_INFORMATION process_info = {};
  if (!CreateProcessW(executable_path.c_str(), command_line.data(), nullptr,
                      nullptr, FALSE, 0, nullptr, nullptr, &startup_info,
                      &process_info)) {
    XELOGE("Unable to restart Xenia: CreateProcessW failed with error {}.",
           GetLastError());
    SetRelaunchFadeEnvironment(false);
    return false;
  }
  CloseHandle(process_info.hThread);
  CloseHandle(process_info.hProcess);
  std::quick_exit(EXIT_SUCCESS);
#else
  SetRelaunchFadeEnvironment(false);
  XELOGE("Automatic Xenia restart is not supported on this platform.");
  return false;
#endif
}

struct NativeContentLaunchJob {
  std::mutex mutex;
  std::condition_variable condition;
  std::atomic_bool opaque = false;
  std::atomic_bool finished = false;
  std::atomic_bool cancelled = false;
};

bool StartNativeContentOverlay(ui::ImGuiDrawer* imgui_drawer,
                               std::shared_ptr<NativeContentLaunchJob> job) {
  if (!imgui_drawer || !job) {
    return false;
  }

  using Clock = std::chrono::steady_clock;
  enum class Phase { kFadeOut, kOpaque, kFadeIn };

  constexpr auto kFadeOutDuration = std::chrono::milliseconds(250);
  constexpr auto kFadeInDuration = std::chrono::milliseconds(250);

  Phase phase = Phase::kFadeOut;
  Clock::time_point phase_start = Clock::now();
  uint32_t opaque_frames = 0;

  return imgui_drawer->SetFullScreenOverlay(
      [job, phase, phase_start, opaque_frames](ImGuiIO& io) mutable -> bool {
        if (job->cancelled.load(std::memory_order_acquire)) {
          g_native_content_launch_pending.store(false,
                                                std::memory_order_release);
          return false;
        }

        const auto now = Clock::now();
        float alpha = 1.0f;
        switch (phase) {
          case Phase::kFadeOut: {
            const float progress =
                std::chrono::duration<float>(now - phase_start).count() /
                std::chrono::duration<float>(kFadeOutDuration).count();
            alpha = SmoothStep(progress);
            if (progress >= 1.0f) {
              phase = Phase::kOpaque;
              phase_start = now;
            }
            break;
          }
          case Phase::kOpaque:
            alpha = 1.0f;
            if (!job->opaque.load(std::memory_order_relaxed) &&
                ++opaque_frames >= 2) {
              job->opaque.store(true, std::memory_order_release);
              job->condition.notify_all();
            }
            if (job->finished.load(std::memory_order_acquire)) {
              phase = Phase::kFadeIn;
              phase_start = now;
            }
            break;
          case Phase::kFadeIn: {
            const float progress =
                std::chrono::duration<float>(now - phase_start).count() /
                std::chrono::duration<float>(kFadeInDuration).count();
            alpha = 1.0f - SmoothStep(progress);
            if (progress >= 1.0f) {
              g_native_content_launch_pending.store(false,
                                                    std::memory_order_release);
              return false;
            }
            break;
          }
        }

        const uint8_t alpha_byte = static_cast<uint8_t>(
            std::clamp(std::lround(alpha * 255.0f), 0l, 255l));
        ImGui::GetForegroundDrawList()->AddRectFilled(
            ImVec2(0.0f, 0.0f), io.DisplaySize,
            IM_COL32(0, 0, 0, alpha_byte));
        return true;
      });
}

bool RunNativeContentCommand(const NativeContentEntry& entry,
                             int* out_exit_code) {
  if (out_exit_code) {
    *out_exit_code = -1;
  }

#if XE_PLATFORM_GNU_LINUX || XE_PLATFORM_MAC
  // Mark descriptors in the parent before fork. This only affects future exec
  // calls and avoids using non-async-signal-safe directory APIs in the child.
  MarkFileDescriptorsCloseOnExec();
  const pid_t child_pid = fork();
  if (child_pid < 0) {
    XELOGE("Unable to launch native content '{}': fork failed: {}",
           entry.display_name, std::strerror(errno));
    return false;
  }

  if (child_pid == 0) {
    if (chdir(entry.working_directory.c_str()) != 0) {
      _exit(126);
    }

    // Standard input/output/error are intentionally retained so wrappers may
    // still log or use an already-configured sudo askpass flow. Other host
    // descriptors have FD_CLOEXEC set and are closed by execl.
#if XE_PLATFORM_GNU_LINUX
    execl("/bin/bash", "bash", "-lc", entry.command.c_str(),
          static_cast<char*>(nullptr));
#else
    execl("/bin/sh", "sh", "-lc", entry.command.c_str(),
          static_cast<char*>(nullptr));
#endif
    _exit(127);
  }

  int status = 0;
  pid_t wait_result;
  do {
    wait_result = waitpid(child_pid, &status, 0);
  } while (wait_result < 0 && errno == EINTR);

  if (wait_result < 0) {
    XELOGE("Unable to wait for native content '{}': {}", entry.display_name,
           std::strerror(errno));
    return false;
  }

  int exit_code = -1;
  if (WIFEXITED(status)) {
    exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    exit_code = 128 + WTERMSIG(status);
  }
  if (out_exit_code) {
    *out_exit_code = exit_code;
  }
  return true;
#elif XE_PLATFORM_WIN32
  const std::u16string command_u16 = xe::to_utf16(entry.command);
  std::wstring command(command_u16.begin(), command_u16.end());
  std::wstring command_line = L"cmd.exe /D /S /C \"" + command + L"\"";
  const std::wstring working_directory = entry.working_directory.wstring();

  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);
  PROCESS_INFORMATION process_info = {};
  if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE, 0,
                      nullptr, working_directory.c_str(), &startup_info,
                      &process_info)) {
    XELOGE("Unable to launch native content '{}': CreateProcessW error {}.",
           entry.display_name, GetLastError());
    return false;
  }

  CloseHandle(process_info.hThread);
  WaitForSingleObject(process_info.hProcess, INFINITE);
  DWORD exit_code = 0;
  GetExitCodeProcess(process_info.hProcess, &exit_code);
  CloseHandle(process_info.hProcess);
  if (out_exit_code) {
    *out_exit_code = static_cast<int>(exit_code);
  }
  return true;
#else
  XELOGE("Native content launching is not supported on this platform.");
  return false;
#endif
}

}  // namespace

XamModule::XamModule(Emulator* emulator, KernelState* kernel_state)
    : KernelModule(kernel_state, "xe:\\xam.xex"), loader_data_() {
  RegisterExportTable(export_resolver_);

  // Register all exported functions.
#define XE_MODULE_EXPORT_GROUP(m, n) \
  Register##n##Exports(export_resolver_, kernel_state_);
#include "xam_module_export_groups.inc"
#undef XE_MODULE_EXPORT_GROUP
}

static auto& get_xam_exports() {
  static std::vector<xe::cpu::Export*> xam_exports(4096);
  return xam_exports;
}

xe::cpu::Export* RegisterExport_xam(xe::cpu::Export* export_entry) {
  auto& xam_exports = get_xam_exports();
  assert_true(export_entry->ordinal < xam_exports.size());
  xam_exports[export_entry->ordinal] = export_entry;
  return export_entry;
}
// Build the export table used for resolution.
#include "xenia/kernel/util/export_table_pre.inc"
static constexpr xe::cpu::Export xam_export_table[] = {
#include "xenia/kernel/xam/xam_table.inc"
};
#include "xenia/kernel/util/export_table_post.inc"
void XamModule::RegisterExportTable(xe::cpu::ExportResolver* export_resolver) {
  assert_not_null(export_resolver);
  auto& xam_exports = get_xam_exports();

  for (size_t i = 0; i < xe::countof(xam_export_table); ++i) {
    auto& export_entry = xam_export_table[i];
    assert_true(export_entry.ordinal < xam_exports.size());
    if (!xam_exports[export_entry.ordinal]) {
      xam_exports[export_entry.ordinal] =
          const_cast<xe::cpu::Export*>(&export_entry);
    }
  }
  export_resolver->RegisterTable("xam.xex", &get_xam_exports());
}

XamModule::~XamModule() {}

void XamModule::LoadLoaderData() {
  FILE* file = xe::filesystem::OpenFile(kXamModuleLoaderDataFileName, "rb");

  if (!file) {
    loader_data_.launch_data.clear();
    return;
  }

  auto string_read = [file]() {
    uint16_t string_size = 0;
    fread(&string_size, sizeof(string_size), 1, file);

    std::string result_string;
    result_string.resize(string_size);
    fread(result_string.data(), string_size, 1, file);
    return result_string;
  };

  loader_data_.host_path = string_read();
  loader_data_.launch_path = string_read();

  fread(&loader_data_.launch_flags, sizeof(loader_data_.launch_flags), 1, file);

  uint16_t launch_data_size = 0;
  fread(&launch_data_size, sizeof(launch_data_size), 1, file);

  if (launch_data_size > 0) {
    loader_data_.launch_data.resize(launch_data_size);
    fread(loader_data_.launch_data.data(), launch_data_size, 1, file);
  }

  fclose(file);
  // We read launch data. Let's remove it till next request.
  std::filesystem::remove(kXamModuleLoaderDataFileName);
}

void XamModule::SaveLoaderData() {
  FILE* file = xe::filesystem::OpenFile(kXamModuleLoaderDataFileName, "wb");

  if (!file) {
    XELOGE("Failed to open {} for writing.", kXamModuleLoaderDataFileName);
    return;
  }

  const std::filesystem::path host_path = loader_data_.host_path;
  const std::string launch_path =
      NormalizeGuestLaunchPath(loader_data_.launch_path);

  const std::string host_path_as_string = xe::path_to_utf8(host_path);
  const uint16_t host_path_length =
      static_cast<uint16_t>(host_path_as_string.size());

  fwrite(&host_path_length, sizeof(host_path_length), 1, file);
  fwrite(host_path_as_string.c_str(), host_path_length, 1, file);

  const uint16_t launch_path_length = static_cast<uint16_t>(launch_path.size());
  fwrite(&launch_path_length, sizeof(launch_path_length), 1, file);
  fwrite(launch_path.c_str(), launch_path_length, 1, file);

  fwrite(&loader_data_.launch_flags, sizeof(loader_data_.launch_flags), 1,
         file);

  const uint16_t launch_data_size =
      static_cast<uint16_t>(loader_data_.launch_data.size());
  fwrite(&launch_data_size, sizeof(launch_data_size), 1, file);

  fwrite(loader_data_.launch_data.data(), launch_data_size, 1, file);

  fclose(file);
}

bool XamModule::RequestNativeContentLaunch(
    const NativeContentEntry& entry) {
  if (g_native_content_launch_pending.exchange(true,
                                                std::memory_order_acq_rel)) {
    XELOGW("Ignoring duplicate native-content launch while one is active.");
    return true;
  }

  std::error_code ec;
  if (!std::filesystem::is_directory(entry.working_directory, ec) || ec) {
    XELOGE("Native-content launch rejected: working directory '{}' is invalid.",
           entry.working_directory);
    g_native_content_launch_pending.store(false, std::memory_order_release);
    return false;
  }

  Emulator* emulator = kernel_state_->emulator();
  auto* display_window = emulator ? emulator->display_window() : nullptr;
  auto* imgui_drawer = emulator ? emulator->imgui_drawer() : nullptr;
  if (!emulator || !display_window || !imgui_drawer) {
    XELOGE("Native-content launch requires an emulator window and ImGui drawer.");
    g_native_content_launch_pending.store(false, std::memory_order_release);
    return false;
  }

  XELOGI(
      "Native-content handoff: title={:08X}, name='{}', cwd='{}', "
      "command='{}'.",
      entry.title_id, entry.display_name, entry.working_directory,
      entry.command);

  auto launch_job = std::make_shared<NativeContentLaunchJob>();
  const bool queued = display_window->app_context().CallInUIThreadDeferred(
      [imgui_drawer, launch_job]() {
        if (!StartNativeContentOverlay(imgui_drawer, launch_job)) {
          XELOGE("Native-content launch failed: another full-screen overlay is "
                 "active.");
          launch_job->cancelled.store(true, std::memory_order_release);
          launch_job->opaque.store(true, std::memory_order_release);
          launch_job->condition.notify_all();
        }
      });
  if (!queued) {
    XELOGE("Native-content launch failed: unable to queue the black overlay.");
    g_native_content_launch_pending.store(false, std::memory_order_release);
    return false;
  }

  std::unique_lock launch_lock(launch_job->mutex);
  const bool opaque = launch_job->condition.wait_for(
      launch_lock, std::chrono::seconds(3), [launch_job]() {
        return launch_job->opaque.load(std::memory_order_acquire);
      });
  launch_lock.unlock();

  if (!opaque || launch_job->cancelled.load(std::memory_order_acquire)) {
    XELOGE("Native-content launch stopped before the screen became opaque.");
    launch_job->cancelled.store(true, std::memory_order_release);
    g_native_content_launch_pending.store(false, std::memory_order_release);
    return false;
  }

  // Keep this XAM launch call blocked until the host game exits. On success,
  // Xenia restarts itself directly into dash.xex while the screen is opaque;
  // the Linux Xbox-mode service never exits and does not replay bootanim.
  const bool was_paused = emulator->is_paused();
  if (!was_paused) {
    emulator->Pause();
  }

  int exit_code = -1;
  const bool process_started = RunNativeContentCommand(entry, &exit_code);
  if (process_started) {
    XELOGI("Native content '{}' exited with code {}.", entry.display_name,
           exit_code);

    // A successful Xbox title launch never returns to the dashboard call site.
    // Keep that behavior here: while the screen is still opaque, stage the
    // dashboard as the next title and replace this process image. execv keeps
    // the systemd service and Linux Xbox session alive, so the boot animation
    // script is not run again.
    const std::filesystem::path dashboard_path =
        xe::filesystem::GetExecutableFolder() / "dash.xex";
    std::error_code dashboard_ec;
    if (std::filesystem::is_regular_file(dashboard_path, dashboard_ec) &&
        !dashboard_ec) {
      const LoaderData previous_loader_data = loader_data_;
      loader_data_.host_path = xe::path_to_utf8(dashboard_path);
      loader_data_.launch_path.clear();
      loader_data_.launch_flags = 0;
      loader_data_.launch_data.clear();
      SaveLoaderData();

      dashboard_ec.clear();
      if (std::filesystem::exists(kXamModuleLoaderDataFileName, dashboard_ec) &&
          !dashboard_ec) {
        XELOGI(
            "Native-content handoff complete; restarting directly into "
            "dashboard without the Linux boot animation.");
        config::SaveConfig();
        xe::FlushLog();
        if (RestartXeniaProcess()) {
          return true;
        }
      }

      XELOGE("Unable to restart Xenia directly into the dashboard.");
      std::filesystem::remove(kXamModuleLoaderDataFileName, dashboard_ec);
      loader_data_ = previous_loader_data;
    } else {
      XELOGE("Unable to return to dashboard: {} was not found.",
             dashboard_path);
    }
  } else {
    XELOGE("Native content '{}' could not be started.", entry.display_name);
  }

  // Starting the process or the direct dashboard restart failed. Restore the
  // still-running dashboard instead of leaving the user on a black screen.
  if (!was_paused) {
    emulator->Resume();
  }

  launch_job->finished.store(true, std::memory_order_release);
  launch_job->condition.notify_all();
  display_window->app_context().CallInUIThreadDeferred(
      [display_window]() { display_window->Focus(); });

  return false;
}

bool XamModule::RequestTitleLaunch(const std::filesystem::path& host_path,
                                   std::string_view launch_path,
                                   uint32_t launch_flags) {
  if (const auto native_entry = LoadNativeContentEntry(host_path)) {
    return RequestNativeContentLaunch(*native_entry);
  }

  if (host_path.empty()) {
    XELOGE("XAM title launch rejected: host path is empty.");
    return false;
  }

  if (g_title_relaunch_pending.exchange(true, std::memory_order_acq_rel)) {
    XELOGW("Ignoring duplicate XAM title launch while a relaunch is pending.");
    return true;
  }

  std::error_code ec;
  if (!std::filesystem::exists(host_path, ec) || ec) {
    XELOGE("XAM title launch rejected: {} does not exist.", host_path);
    g_title_relaunch_pending.store(false, std::memory_order_release);
    return false;
  }

  std::filesystem::path resolved_host_path = host_path;
  std::string resolved_launch_path =
      NormalizeGuestLaunchPath(std::string(launch_path));

  if (std::filesystem::is_directory(resolved_host_path, ec) && !ec) {
    if (resolved_launch_path.empty()) {
      resolved_launch_path = "default.xex";
    }
    resolved_host_path /= xe::to_path(resolved_launch_path);
    resolved_launch_path.clear();
  } else if (resolved_host_path.extension() == ".xex" &&
             !resolved_launch_path.empty()) {
    resolved_host_path =
        resolved_host_path.parent_path() / xe::to_path(resolved_launch_path);
    resolved_launch_path.clear();
  }

  ec.clear();
  if (!std::filesystem::is_regular_file(resolved_host_path, ec) || ec) {
    XELOGE("XAM title launch rejected: resolved target {} is not a file.",
           resolved_host_path);
    g_title_relaunch_pending.store(false, std::memory_order_release);
    return false;
  }

  LoaderData previous_loader_data = loader_data_;
  loader_data_.host_path = xe::path_to_utf8(resolved_host_path);
  loader_data_.launch_path = resolved_launch_path;
  loader_data_.launch_flags = launch_flags;
  SaveLoaderData();

  ec.clear();
  if (!std::filesystem::exists(kXamModuleLoaderDataFileName, ec) || ec) {
    XELOGE("XAM title launch failed: {} was not created.",
           kXamModuleLoaderDataFileName);
    loader_data_ = std::move(previous_loader_data);
    g_title_relaunch_pending.store(false, std::memory_order_release);
    return false;
  }

  Emulator* emulator = kernel_state_->emulator();
  auto* display_window = emulator ? emulator->display_window() : nullptr;
  auto* imgui_drawer = emulator ? emulator->imgui_drawer() : nullptr;
  if (!display_window || !imgui_drawer) {
    XELOGE("XAM title relaunch requires a host window and ImGui drawer.");
    std::filesystem::remove(kXamModuleLoaderDataFileName, ec);
    loader_data_ = std::move(previous_loader_data);
    g_title_relaunch_pending.store(false, std::memory_order_release);
    return false;
  }

  XELOGI("XAM fade-and-relaunch handoff: host='{}', flags={:08X}.",
         resolved_host_path, launch_flags);

  auto fade_job = std::make_shared<RelaunchFadeJob>();
  const bool queued = display_window->app_context().CallInUIThreadDeferred(
      [imgui_drawer, fade_job]() {
        if (!StartRelaunchFadeOut(imgui_drawer, fade_job)) {
          XELOGE("XAM title relaunch failed: fade overlay is already active.");
          fade_job->cancelled.store(true, std::memory_order_release);
          fade_job->opaque.store(true, std::memory_order_release);
          fade_job->condition.notify_all();
        }
      });
  if (!queued) {
    XELOGE("XAM title relaunch failed: unable to queue the fade overlay.");
    std::filesystem::remove(kXamModuleLoaderDataFileName, ec);
    loader_data_ = std::move(previous_loader_data);
    g_title_relaunch_pending.store(false, std::memory_order_release);
    return false;
  }

  std::unique_lock fade_lock(fade_job->mutex);
  const bool opaque = fade_job->condition.wait_for(
      fade_lock, std::chrono::seconds(3), [fade_job]() {
        return fade_job->opaque.load(std::memory_order_acquire);
      });
  fade_lock.unlock();

  if (!opaque || fade_job->cancelled.load(std::memory_order_acquire)) {
    XELOGE("XAM title relaunch failed before the screen became opaque.");
    fade_job->cancelled.store(true, std::memory_order_release);
    std::filesystem::remove(kXamModuleLoaderDataFileName, ec);
    loader_data_ = std::move(previous_loader_data);
    g_title_relaunch_pending.store(false, std::memory_order_release);
    return false;
  }

  XELOGI("XAM screen is opaque; replacing the Xenia process image now.");
  config::SaveConfig();
  xe::FlushLog();
  if (RestartXeniaProcess()) {
    return true;
  }

  std::filesystem::remove(kXamModuleLoaderDataFileName, ec);
  loader_data_ = std::move(previous_loader_data);
  fade_job->restart_failed.store(true, std::memory_order_release);
  return false;
}

bool XamModule::RequestDashboardLaunch(uint32_t launch_flags) {
  const std::filesystem::path dashboard_path =
      xe::filesystem::GetExecutableFolder() / "dash.xex";
  std::error_code ec;
  if (!std::filesystem::is_regular_file(dashboard_path, ec) || ec) {
    XELOGE("Unable to return to dashboard: {} was not found.", dashboard_path);
    return false;
  }

  loader_data_.launch_data.clear();
  return RequestTitleLaunch(dashboard_path, {}, launch_flags);
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
