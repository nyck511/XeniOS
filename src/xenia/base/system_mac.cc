/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <dlfcn.h>
#include <stdlib.h>
#include <sys/resource.h>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"
#include "xenia/base/system.h"

#if !XE_PLATFORM_IOS
#include <alloca.h>

#include <cstdint>
#include <cstring>
#endif  // !XE_PLATFORM_IOS

#if !XE_PLATFORM_IOS
namespace {
using SDL_ShowSimpleMessageBox_fn = bool (*)(uint32_t flags, const char* title,
                                             const char* message, void* window);
constexpr uint32_t kSDLMessageBoxError = 0x00000010u;
constexpr uint32_t kSDLMessageBoxWarning = 0x00000020u;
constexpr uint32_t kSDLMessageBoxInformation = 0x00000040u;
}  // namespace
#endif  // !XE_PLATFORM_IOS

namespace xe {

void LaunchWebBrowser(const std::string_view url) {
#if XE_PLATFORM_IOS
  // TODO(wmarti): Implement via UIApplication openURL.
  XELOGW("LaunchWebBrowser not yet implemented on iOS: {}", url);
#else
  auto cmd = std::string("open ");
  cmd.append(url);
  system(cmd.c_str());
#endif
}

void LaunchFileExplorer(const std::filesystem::path& path) {
#if XE_PLATFORM_IOS
  XELOGW("LaunchFileExplorer not supported on iOS: {}", path.string());
#else
  auto cmd = std::string("open \"");
  cmd.append(path.string());
  cmd.append("\"");
  system(cmd.c_str());
#endif
}

void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
#if XE_PLATFORM_IOS
  // TODO(wmarti): Implement via UIAlertController.
  XELOGW("ShowSimpleMessageBox (iOS): {}", message);
#else
  void* libsdl = dlopen("libSDL3.0.dylib", RTLD_LAZY | RTLD_LOCAL);
  if (!libsdl) {
    libsdl = dlopen("libSDL3.dylib", RTLD_LAZY | RTLD_LOCAL);
  }
  if (!libsdl) {
    libsdl = dlopen("/opt/homebrew/lib/libSDL3.dylib", RTLD_LAZY | RTLD_LOCAL);
  }
  if (!libsdl) {
    libsdl = dlopen("/usr/local/lib/libSDL3.dylib", RTLD_LAZY | RTLD_LOCAL);
  }
  if (!libsdl) {
    XELOGE("ShowSimpleMessageBox (SDL3 not available): {}", message);
    return;
  }
  if (libsdl) {
    auto* pSDL_ShowSimpleMessageBox =
        reinterpret_cast<SDL_ShowSimpleMessageBox_fn>(
            dlsym(libsdl, "SDL_ShowSimpleMessageBox"));
    assert_not_null(pSDL_ShowSimpleMessageBox);
    if (pSDL_ShowSimpleMessageBox) {
      uint32_t flags;
      const char* title;
      char* message_copy = reinterpret_cast<char*>(alloca(message.size() + 1));
      std::memcpy(message_copy, message.data(), message.size());
      message_copy[message.size()] = '\0';

      switch (type) {
        default:
        case SimpleMessageBoxType::Help:
          title = "Xenia Help";
          flags = kSDLMessageBoxInformation;
          break;
        case SimpleMessageBoxType::Warning:
          title = "Xenia Warning";
          flags = kSDLMessageBoxWarning;
          break;
        case SimpleMessageBoxType::Error:
          title = "Xenia Error";
          flags = kSDLMessageBoxError;
          break;
      }
      pSDL_ShowSimpleMessageBox(flags, title, message_copy, NULL);
    }
    dlclose(libsdl);
  }
#endif  // XE_PLATFORM_IOS
}

bool SetProcessPriorityClass(const uint32_t priority_class) {
  int nice_value = 0;
  switch (priority_class) {
    case 0:
      nice_value = 0;
      break;
    case 1:
      nice_value = -5;
      break;
    case 2:
      nice_value = -10;
      break;
    case 3:
      nice_value = -20;
      break;
    default:
      return false;
  }

  return setpriority(PRIO_PROCESS, 0, nice_value) == 0;
}

bool IsUseNexusForGameBarEnabled() { return false; }

}  // namespace xe
