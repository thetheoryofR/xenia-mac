/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Gears of War 3 UnrealScript call tracer (diagnostics, off by default).    *
 * Mirrors gears3-recomp's gears3_native_trace.cpp so the two runs can be    *
 * diffed with the same resolver (gears3-recomp tools/strace_resolve.py).    *
 ******************************************************************************
 */

#ifndef XENIA_CPU_PPC_PPC_GEARS3_TRACE_H_
#define XENIA_CPU_PPC_PPC_GEARS3_TRACE_H_

#include <cstdint>

namespace xe {
class Memory;
namespace cpu {
class Function;
class Processor;
namespace ppc {
namespace gears3 {

// Returns a builtin to call at the entry of the guest function at `address`
// when --gears3_trace is on and the address is a traced UE3 native or a
// script-call opcode handler; nullptr otherwise. The first call scans the
// loaded image, so it must happen after the XEX is loaded (any translation).
Function* EntryHookFor(Processor* processor, Memory* memory, uint32_t address);

}  // namespace gears3
}  // namespace ppc
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_PPC_PPC_GEARS3_TRACE_H_
