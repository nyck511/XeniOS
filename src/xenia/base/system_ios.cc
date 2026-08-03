/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/system.h"

#include <dirent.h>
#include <mach/mach.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "xenia/base/logging.h"

namespace xe {

#if XE_ARCH_ARM64
namespace {

extern "C" int csops(pid_t pid, unsigned int ops, void* useraddr,
                     size_t usersize);

#ifndef CS_OPS_STATUS
#define CS_OPS_STATUS 0
#endif
#ifndef CS_DEBUGGED
#define CS_DEBUGGED 0x10000000
#endif

constexpr uint64_t kUniversalPrepareCommand = 1;
constexpr uint32_t kLegacyPrepareRejectedResult = 0xE0000069u;
constexpr uint32_t kLegacyPrepareRejectedResultSwapped = 0x690000E0u;
constexpr sig_atomic_t kUniversalBreakpointImmediate = 0xf00d;
constexpr sig_atomic_t kLegacyBreakpointImmediate = 0x69;
constexpr size_t kFallbackIOSPageSize = 16384;

std::mutex ios_jit_broker_command_mutex;
volatile sig_atomic_t ios_jit_broker_trap_was_local = 0;
thread_local volatile sig_atomic_t ios_jit_broker_expected_breakpoint = 0;
struct sigaction ios_jit_broker_previous_trap_action = {};
std::atomic<bool> ios_jit_broker_prepare_succeeded{false};

std::vector<std::string> FindChildrenWithNameLength(
    const std::string& directory, size_t name_length) {
  DIR* dir = opendir(directory.c_str());
  if (!dir) {
    return {};
  }

  std::vector<std::string> found;
  while (dirent* entry = readdir(dir)) {
    const char* name = entry->d_name;
    if (!name || name[0] == '.') {
      continue;
    }
    if (std::strlen(name) == name_length) {
      found.push_back(directory + "/" + name);
    }
  }
  closedir(dir);
  std::sort(found.begin(), found.end());
  return found;
}

void IOSJITBrokerTrapHandler(int signal, siginfo_t* info, void* context) {
  bool is_expected_broker_breakpoint = false;
  if (signal == SIGTRAP && context && ios_jit_broker_expected_breakpoint != 0) {
    auto* signal_context = reinterpret_cast<ucontext_t*>(context);
    const uintptr_t program_counter =
        static_cast<uintptr_t>(signal_context->uc_mcontext->__ss.__pc);
    if (program_counter) {
      const uint32_t instruction =
          *reinterpret_cast<const uint32_t*>(program_counter);
      const uint32_t expected_instruction =
          0xD4200000u |
          (static_cast<uint32_t>(ios_jit_broker_expected_breakpoint) << 5);
      is_expected_broker_breakpoint = instruction == expected_instruction;
    }
  }

  if (!is_expected_broker_breakpoint) {
    if (ios_jit_broker_previous_trap_action.sa_handler == SIG_IGN) {
      return;
    }
    if (ios_jit_broker_previous_trap_action.sa_handler == SIG_DFL) {
      sigaction(SIGTRAP, &ios_jit_broker_previous_trap_action, nullptr);
      raise(SIGTRAP);
      return;
    }
    if (ios_jit_broker_previous_trap_action.sa_flags & SA_SIGINFO) {
      ios_jit_broker_previous_trap_action.sa_sigaction(signal, info, context);
    } else {
      ios_jit_broker_previous_trap_action.sa_handler(signal);
    }
    return;
  }

  ios_jit_broker_trap_was_local = 1;
  auto* signal_context = reinterpret_cast<ucontext_t*>(context);
  signal_context->uc_mcontext->__ss.__pc += sizeof(uint32_t);
}

bool InvokeIOSJITBrokerCommand(uint64_t command, uintptr_t address,
                               size_t length, bool use_universal_protocol,
                               uint64_t* result_address) {
  std::lock_guard<std::mutex> lock(ios_jit_broker_command_mutex);

  struct sigaction action = {};
  struct sigaction previous_action = {};
  action.sa_sigaction = IOSJITBrokerTrapHandler;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGTRAP, &action, &ios_jit_broker_previous_trap_action) != 0) {
    XELOGE("iOS JIT broker: unable to install guarded SIGTRAP handler");
    return false;
  }
  previous_action = ios_jit_broker_previous_trap_action;

  ios_jit_broker_trap_was_local = 0;
  ios_jit_broker_expected_breakpoint = use_universal_protocol
                                           ? kUniversalBreakpointImmediate
                                           : kLegacyBreakpointImmediate;
  register uint64_t x0 __asm("x0") = static_cast<uint64_t>(address);
  register uint64_t x1 __asm("x1") = static_cast<uint64_t>(length);
  if (use_universal_protocol) {
    register uint64_t x16 __asm("x16") = command;
    asm volatile("brk #0xf00d" : "+r"(x0), "+r"(x1), "+r"(x16) : : "memory");
  } else {
    asm volatile("brk #0x69" : "+r"(x0), "+r"(x1) : : "memory");
  }
  ios_jit_broker_expected_breakpoint = 0;

  const bool handled_by_broker = !ios_jit_broker_trap_was_local;
  if (result_address) {
    *result_address = x0;
  }
  if (sigaction(SIGTRAP, &previous_action, nullptr) != 0) {
    XELOGE("iOS JIT broker: unable to restore the SIGTRAP handler");
    return false;
  }
  return handled_by_broker;
}

}  // namespace

int IOSProductMajorVersion() {
  static const int major_version = []() -> int {
    size_t version_size = 0;
    if (sysctlbyname("kern.osproductversion", nullptr, &version_size, nullptr,
                     0) != 0 ||
        version_size == 0) {
      return -1;
    }

    std::string version(version_size, '\0');
    if (sysctlbyname("kern.osproductversion", version.data(), &version_size,
                     nullptr, 0) != 0 ||
        version_size == 0) {
      return -1;
    }
    if (!version.empty() && version.back() == '\0') {
      version.pop_back();
    }

    int parsed_major = 0;
    size_t index = 0;
    while (index < version.size() && version[index] >= '0' &&
           version[index] <= '9') {
      parsed_major = parsed_major * 10 + (version[index] - '0');
      ++index;
    }
    return parsed_major > 0 ? parsed_major : -1;
  }();
  return major_version;
}

bool IOSDeviceHasTXM() {
  static const bool has_txm = []() -> bool {
    if (const char* override_value = std::getenv("HAS_TXM")) {
      if (override_value[0] == '1' && override_value[1] == '\0') {
        return true;
      }
      if (override_value[0] == '0' && override_value[1] == '\0') {
        return false;
      }
    }

    bool txm_candidate_found = false;
    bool txm_candidate_probe_unknown = false;
    const auto check_txm_candidate = [&](const std::string& txm_root) {
      txm_candidate_found = true;
      const std::string txm_path =
          txm_root +
          "/usr/standalone/firmware/FUD/Ap,TrustedExecutionMonitor.img4";
      errno = 0;
      if (access(txm_path.c_str(), F_OK) == 0) {
        return true;
      }
      if (errno != ENOENT) {
        txm_candidate_probe_unknown = true;
      }
      return false;
    };

    for (const std::string& preboot_uuid :
         FindChildrenWithNameLength("/System/Volumes/Preboot", 36)) {
      for (const std::string& txm_root :
           FindChildrenWithNameLength(preboot_uuid + "/boot", 96)) {
        if (check_txm_candidate(txm_root)) {
          return true;
        }
      }
    }

    for (const std::string& private_preboot_root :
         FindChildrenWithNameLength("/private/preboot", 96)) {
      if (check_txm_candidate(private_preboot_root)) {
        return true;
      }
    }

    const int ios_major_version = IOSProductMajorVersion();
    if (ios_major_version >= 26) {
      if (txm_candidate_found && !txm_candidate_probe_unknown) {
        XELOGW(
            "iOS JIT: TXM firmware was not found at the legacy paths on iOS "
            "{}; conservatively assuming TXM",
            ios_major_version);
      } else {
        XELOGW(
            "iOS JIT: unable to inspect TXM firmware on iOS {}; "
            "conservatively assuming TXM",
            ios_major_version);
      }
      return true;
    }
    return false;
  }();
  return has_txm;
}

bool IOSRequiresTXMJITBroker() {
  return IOSProductMajorVersion() >= 26 && IOSDeviceHasTXM();
}

bool IOSIsCodeSignDebugged() {
  int flags = 0;
  return !csops(getpid(), CS_OPS_STATUS, &flags, sizeof(flags)) &&
         (flags & CS_DEBUGGED);
}

bool IOSCanMapExecutablePage() {
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) {
    page_size = static_cast<long>(kFallbackIOSPageSize);
  }
  void* test = mmap(nullptr, static_cast<size_t>(page_size),
                    PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (test == MAP_FAILED) {
    return false;
  }
  munmap(test, static_cast<size_t>(page_size));
  return true;
}

bool IOSJITBrokerPrepareExecutableRegion(void* address, size_t length,
                                         bool use_universal_protocol,
                                         uint64_t* result_address) {
  if (!address || !length) {
    return false;
  }
  uint64_t broker_result = reinterpret_cast<uintptr_t>(address);
  const bool handled = InvokeIOSJITBrokerCommand(
      kUniversalPrepareCommand, reinterpret_cast<uintptr_t>(address), length,
      use_universal_protocol, &broker_result);
  if (result_address) {
    *result_address = broker_result;
  }
  const uint64_t requested_address = reinterpret_cast<uintptr_t>(address);
  const uint32_t low_result = static_cast<uint32_t>(broker_result);
  const bool legacy_rejected =
      !use_universal_protocol &&
      (low_result == kLegacyPrepareRejectedResult ||
       low_result == kLegacyPrepareRejectedResultSwapped);
  const bool prepared =
      handled && (broker_result == requested_address || legacy_rejected);
  if (handled && !prepared) {
    ios_jit_broker_prepare_succeeded.store(false, std::memory_order_release);
    XELOGE(
        "iOS JIT broker: prepare command returned an invalid result "
        "address 0x{:X} for requested address 0x{:X}",
        broker_result, reinterpret_cast<uintptr_t>(address));
  }
  if (prepared && use_universal_protocol) {
    ios_jit_broker_prepare_succeeded.store(true, std::memory_order_release);
  }
  return prepared;
}

bool IOSJITBrokerDetach() {
  return InvokeIOSJITBrokerCommand(0, 0, 0, true, nullptr);
}

bool IOSJITIsAvailable() {
  if (!IOSCanMapExecutablePage()) {
    return false;
  }
  if (!IOSRequiresTXMJITBroker()) {
    return true;
  }
  if (!IOSIsCodeSignDebugged()) {
    return false;
  }
  if (ios_jit_broker_prepare_succeeded.load(std::memory_order_acquire)) {
    return true;
  }

  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) {
    page_size = static_cast<long>(kFallbackIOSPageSize);
  }
  void* probe = mmap(nullptr, static_cast<size_t>(page_size),
                     PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (probe == MAP_FAILED) {
    return false;
  }
  const bool prepared = IOSJITBrokerPrepareExecutableRegion(
      probe, static_cast<size_t>(page_size), true, nullptr);
  munmap(probe, static_cast<size_t>(page_size));
  return prepared;
}
#endif  // XE_ARCH_ARM64

void LaunchWebBrowser(const std::string_view url) {
  XELOGW("LaunchWebBrowser is not supported on iOS: {}", url);
}

void LaunchFileExplorer(const std::filesystem::path& path) {
  XELOGW("LaunchFileExplorer is not supported on iOS: {}", path.string());
}

bool SetProcessPriorityClass(const uint32_t priority_class) {
  static_cast<void>(priority_class);
  return true;
}

bool IsUseNexusForGameBarEnabled() { return false; }

void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  static_cast<void>(type);
  XELOGW("ShowSimpleMessageBox is not supported on iOS: {}", message);
}

}  // namespace xe
