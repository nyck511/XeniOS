/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_SYSTEM_H_
#define XENIA_BASE_SYSTEM_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

#include "xenia/base/platform.h"
#include "xenia/base/string.h"

namespace xe {

#if XE_PLATFORM_ANDROID
bool InitializeAndroidSystemForApplicationContext();
void ShutdownAndroidSystem();
#endif

// The URL must include the protocol.
void LaunchWebBrowser(const std::string_view url);
void LaunchFileExplorer(const std::filesystem::path& path);

bool SetProcessPriorityClass(const uint32_t priority_class);

#if XE_PLATFORM_IOS && XE_ARCH_ARM64
// iOS 26 and newer require TXM-backed executable regions to be prepared by an
// attached debugger broker before they can be executed. These helpers keep the
// platform probe and the guarded breakpoint protocol shared by the launcher,
// emulator and code-cache implementations.
int IOSProductMajorVersion();
bool IOSDeviceHasTXM();
bool IOSRequiresTXMJITBroker();
bool IOSIsCodeSignDebugged();
bool IOSCanMapExecutablePage();
bool IOSJITBrokerPrepareExecutableRegion(void* address, size_t length,
                                         bool use_universal_protocol,
                                         uint64_t* result_address);
bool IOSJITBrokerDetach();
bool IOSJITIsAvailable();
#endif  // XE_PLATFORM_IOS && XE_ARCH_ARM64

// Determine if the Xbox Gamebar is enabled via the Windows registry
bool IsUseNexusForGameBarEnabled();

enum class SimpleMessageBoxType {
  Help,
  Warning,
  Error,
};

// This is expected to block the caller until the message box is closed.
void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message);

}  // namespace xe

#endif  // XENIA_BASE_SYSTEM_H_
