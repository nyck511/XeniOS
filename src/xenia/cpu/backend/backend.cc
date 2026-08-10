/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/backend.h"

#include <cstring>

#include "xenia/base/cvar.h"

DEFINE_bool(record_mmio_access_exceptions, true,
            "For guest addresses records whether we caught any MMIO accesses "
            "for them. This info can then be used on a subsequent run to "
            "instruct the recompiler to emit checks",
            "CPU");

DEFINE_bool(emit_mmio_aware_stores_for_recorded_exception_addresses, true,
            "Uses info gathered via record_mmio_access_exceptions to emit "
            "special loads/stores that are faster than trapping the exception",
            "CPU");

DEFINE_bool(log_mmio_recording, false,
            "Log a small number of MMIO recording events for diagnostics.",
            "CPU");

namespace xe {
namespace cpu {
namespace backend {

Backend::Backend() { std::memset(&machine_info_, 0, sizeof(machine_info_)); }
Backend::~Backend() = default;

bool Backend::Initialize(Processor* processor) {
  processor_ = processor;
  return true;
}

void* Backend::AllocThreadData() { return nullptr; }

void Backend::FreeThreadData(void* thread_data) {}

void (*preempt_yield_handler)(void* raw_context) = nullptr;

uint32_t Backend::ReservedLoad32(ppc::PPCContext* context, uint32_t address) {
  return xe::byte_swap(*context->TranslateVirtual<uint32_t*>(address));
}

uint64_t Backend::ReservedLoad64(ppc::PPCContext* context, uint32_t address) {
  return xe::byte_swap(*context->TranslateVirtual<uint64_t*>(address));
}

bool Backend::ReservedStore32(ppc::PPCContext* context, uint32_t address,
                              uint32_t value) {
  *context->TranslateVirtual<uint32_t*>(address) = xe::byte_swap(value);
  return true;
}

bool Backend::ReservedStore64(ppc::PPCContext* context, uint32_t address,
                              uint64_t value) {
  *context->TranslateVirtual<uint64_t*>(address) = xe::byte_swap(value);
  return true;
}

}  // namespace backend
}  // namespace cpu
}  // namespace xe
