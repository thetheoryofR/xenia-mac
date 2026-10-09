/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Gears of War 3 UnrealScript call tracer (diagnostics, off by default).    *
 ******************************************************************************
 */

// Port of gears3-recomp's src/gears3_native_trace.cpp to Xenia's JIT, so the
// recompiled build and Xenia can be traced identically and diffed (BOOT-3).
//
// Natives: UE3 registers script natives in static FNativeFunctionLookup tables,
// (pointer to "UClassexecFunction", pointer to exec function) pairs in the
// loaded image. They are found by scanning guest memory on first use; nothing
// game-derived lives in this file. Script calls: the EX_VirtualFunction and
// EX_FinalFunction opcode handlers (addresses from --gears3_script_handlers,
// as logged by the recompile's tracer) read the called function from the
// bytecode. Instead of wrapping functions, the JIT emits a builtin call at the
// entry of each traced function (see PPCHIRBuilder::Emit).
//
// Output (to --gears3_trace_log, same line formats as the recompile):
//   [<wall>] [ntrace] first <ms> <native name> self=<obj>
//   [<wall>] [strace] first <ms> <op> fn=<name idx> self=<name idx> class=<idx>
//   [<wall>] [strace] state <ms> <native> self=<idx> class=<idx> frame=<w>:<12w>
// GotoState/PushState/PopState are logged after the call in the recompile; an
// entry hook can't see the return, so the state frame is logged at the next
// hook on the same thread (the state change has happened by then).
// --gears3_trace_file: per-second histogram ("S", "n", "f" lines, "N" header).
// --gears3_memdump: guest memory dump every --gears3_memdump_every seconds.

#include "xenia/cpu/ppc/ppc_gears3_trace.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/memory.h"

DEFINE_bool(gears3_trace, false,
            "Gears 3 diagnostics: trace UE3 natives and UnrealScript calls "
            "(BOOT-3).",
            "CPU");
DEFINE_bool(gears3_trace_natives, true,
            "With gears3_trace: also hook every UE3 native exec function.",
            "CPU");
DEFINE_string(gears3_script_handlers, "822cd110,822cd180",
              "With gears3_trace: guest addresses of the EX_VirtualFunction "
              "and EX_FinalFunction opcode handlers.",
              "CPU");
DEFINE_path(gears3_trace_log, "gears3_xenia_trace.log",
            "With gears3_trace: [ntrace]/[strace] line log.", "CPU");
DEFINE_path(gears3_trace_file, "",
            "With gears3_trace: per-second call histogram.", "CPU");
DEFINE_path(gears3_memdump, "",
            "With gears3_trace: guest memory dump path (rewritten "
            "periodically).",
            "CPU");
DEFINE_int32(gears3_memdump_every, 60,
             "With gears3_trace: seconds between guest memory dumps.", "CPU");

namespace xe {
namespace cpu {
namespace ppc {
namespace gears3 {
namespace {

constexpr uint32_t kImageBase = 0x82000000, kImageEnd = 0x835B0000;
constexpr size_t kNameSlots = 1 << 18;
// UE3 UObject / FFrame layout on this build (see the recompile's tracer).
constexpr uint32_t kObjStateFrame = 0x18, kObjName = 0x2C, kObjClass = 0x34;
constexpr uint32_t kFrameCode = 0x18;
constexpr const char* kOpNames[] = {"virtual", "final"};

struct Hook {
  enum Kind { kNative, kScriptOp } kind;
  int index;  // native index, or op index
  uint32_t addr;
  std::string name;
  bool state_change = false;
  std::atomic<uint64_t> calls{0};
  uint64_t last_reported = 0;
  Function* builtin = nullptr;
};

std::mutex g_init_mutex;
std::atomic<bool> g_initialized{false};
std::unordered_map<uint32_t, Hook*> g_hooks;
std::vector<std::unique_ptr<Hook>> g_natives;  // index order for the histogram
std::vector<std::unique_ptr<Hook>> g_ops;
Memory* g_memory = nullptr;
uint8_t* g_base = nullptr;
std::chrono::steady_clock::time_point g_start;
std::mutex g_log_mutex;
FILE* g_log = nullptr;

std::atomic<uint32_t> g_fn_calls[kNameSlots];
std::atomic<uint8_t> g_fn_seen[kNameSlots];
uint32_t g_fn_reported[kNameSlots];

thread_local std::vector<std::pair<const Hook*, uint32_t>> t_pending_states;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - g_start)
      .count();
}

void Log(const std::string& msg) {
  if (!g_log) return;
  auto now = std::chrono::system_clock::now();
  auto t = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch())
                .count() %
            1000;
  std::tm tm;
  localtime_r(&t, &tm);
  char wall[32];
  std::strftime(wall, sizeof(wall), "%Y-%m-%d %H:%M:%S", &tm);
  std::lock_guard<std::mutex> lock(g_log_mutex);
  std::fprintf(g_log, "[%s.%03lld] [warning] [xenia] %s\n", wall,
               (long long)ms, msg.c_str());
  std::fflush(g_log);
}

uint32_t Be32(uint32_t guest) {
  if (guest < 0x40000000 || guest >= 0xA0000000) return 0;
  uint32_t v;
  std::memcpy(&v, g_base + guest, 4);
  return xe::byte_swap(v);
}
uint32_t NameOf(uint32_t obj) { return obj ? Be32(obj + kObjName) : 0; }
uint32_t ClassNameOf(uint32_t obj) {
  return obj ? NameOf(Be32(obj + kObjClass)) : 0;
}

void FlushPendingStates() {
  for (auto& [hook, self] : t_pending_states) {
    const uint32_t sf = Be32(self + kObjStateFrame);
    std::string words;
    for (uint32_t k = 0; sf && k < 12; ++k) {
      words += fmt::format(" {:08x}", Be32(sf + 4 * k));
    }
    Log(fmt::format("[strace] state {} {} self={} class={} frame={:08x}:{}",
                    NowMs(), hook->name, NameOf(self), ClassNameOf(self), sf,
                    words));
  }
  t_pending_states.clear();
}

void OnEntry(PPCContext* ctx, void* arg0, void*) {
  auto* hook = static_cast<Hook*>(arg0);
  if (!t_pending_states.empty()) FlushPendingStates();
  const uint32_t self = uint32_t(ctx->r[3]);
  if (hook->kind == Hook::kNative) {
    if (hook->calls.fetch_add(1, std::memory_order_relaxed) == 0) {
      Log(fmt::format("[ntrace] first {} {} self={:08x}", NowMs(), hook->name,
                      self));
    }
    if (hook->state_change) t_pending_states.emplace_back(hook, self);
    return;
  }
  hook->calls.fetch_add(1, std::memory_order_relaxed);
  const uint32_t stack = uint32_t(ctx->r[4]);
  const uint32_t code = Be32(stack + kFrameCode);
  uint32_t fn = 0;
  if (code) {
    // Code points just past the opcode. EX_FinalFunction's UFunction* is a
    // 64-bit ScriptPointerType (high word 0); EX_VirtualFunction an FName.
    uint32_t v;
    std::memcpy(&v, g_base + code + (hook->index == 1 ? 4 : 0), 4);
    v = xe::byte_swap(v);
    fn = hook->index == 1 ? NameOf(v) : v;
  }
  const size_t slot = fn & (kNameSlots - 1);
  g_fn_calls[slot].fetch_add(1, std::memory_order_relaxed);
  if (!g_fn_seen[slot].exchange(1, std::memory_order_relaxed)) {
    Log(fmt::format("[strace] first {} {} fn={} self={} class={}", NowMs(),
                    kOpNames[hook->index], fn, NameOf(self),
                    ClassNameOf(self)));
  }
}

bool IsNameChar(uint8_t c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '_';
}

void DumpGuestMemory(const std::string& path) {
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return;
  size_t total = 0;
  for (uint64_t a = 0x40000000; a < 0xA0000000;) {
    auto* heap = g_memory->LookupHeap(uint32_t(a));
    HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(uint32_t(a), &info) ||
        info.region_size == 0) {
      a += 0x10000;
      continue;
    }
    const uint64_t next = uint64_t(info.base_address) + info.region_size;
    if ((info.state & kMemoryAllocationCommit) &&
        (info.protect & kMemoryProtectRead)) {
      const uint32_t hdr[2] = {info.base_address, info.region_size};
      std::fwrite(hdr, 4, 2, f);
      std::fwrite(g_base + info.base_address, 1, info.region_size, f);
      total += info.region_size;
    }
    a = next > a ? next : a + 0x1000;
  }
  std::fclose(f);
  std::rename(tmp.c_str(), path.c_str());
  Log(fmt::format("[ntrace] dumped {} MB of guest memory to {} at {} ms",
                  total >> 20, path, NowMs()));
}

void Reporter(std::string trace, std::string dump, int every) {
  FILE* tf = trace.empty() ? nullptr : std::fopen(trace.c_str(), "w");
  if (tf) {
    for (size_t i = 0; i < g_natives.size(); ++i) {
      std::fprintf(tf, "N %zu %s\n", i, g_natives[i]->name.c_str());
    }
  }
  for (int sec = 1;; ++sec) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (!dump.empty() && sec % every == 0) DumpGuestMemory(dump);
    if (!tf) continue;
    std::fprintf(tf, "S %d %lld\n", sec, (long long)NowMs());
    for (size_t i = 0; i < g_natives.size(); ++i) {
      Hook& n = *g_natives[i];
      const uint64_t now = n.calls.load(std::memory_order_relaxed);
      if (now != n.last_reported) {
        std::fprintf(tf, "n %zu %llu\n", i,
                     (unsigned long long)(now - n.last_reported));
      }
      n.last_reported = now;
    }
    for (size_t i = 0; i < kNameSlots; ++i) {
      const uint32_t now = g_fn_calls[i].load(std::memory_order_relaxed);
      if (now != g_fn_reported[i]) {
        std::fprintf(tf, "f %zu %u\n", i, now - g_fn_reported[i]);
      }
      g_fn_reported[i] = now;
    }
    std::fflush(tf);
  }
}

void Initialize(Processor* processor, Memory* memory) {
  g_memory = memory;
  g_base = memory->TranslateVirtual(0);
  g_start = std::chrono::steady_clock::now();
  g_log = std::fopen(cvars::gears3_trace_log.string().c_str(), "w");
  if (!g_log) {
    XELOGE("gears3_trace: cannot open {}", cvars::gears3_trace_log.string());
  }

  auto add = [&](std::unique_ptr<Hook> hook) {
    hook->builtin = processor->DefineBuiltin(
        fmt::format("gears3_trace_{:08X}", hook->addr), OnEntry, hook.get(),
        nullptr);
    g_hooks[hook->addr] = hook.get();
    return hook;
  };

  // Script-call opcode handlers.
  const char* p = cvars::gears3_script_handlers.c_str();
  for (int op = 0; op < 2 && *p; ++op) {
    char* end = nullptr;
    const uint32_t addr = uint32_t(std::strtoul(p, &end, 16));
    if (end == p) break;
    p = *end == ',' ? end + 1 : end;
    auto hook = std::make_unique<Hook>();
    hook->kind = Hook::kScriptOp;
    hook->index = op;
    hook->addr = addr;
    hook->name = kOpNames[hook->index];
    g_ops.push_back(add(std::move(hook)));
  }

  size_t string_count = 0;
  if (cvars::gears3_trace_natives) {
    const uint8_t* img = g_base + kImageBase;
    const size_t size = kImageEnd - kImageBase;
    // 1. Strings "[AU]<Class>exec<Function>\0" that start right after a NUL.
    std::unordered_map<uint32_t, std::string> strings;
    for (size_t i = 1; i < size; ++i) {
      if (img[i - 1] != 0 || (img[i] != 'A' && img[i] != 'U')) continue;
      size_t j = i;
      while (j < size && IsNameChar(img[j])) ++j;
      if (j >= size || img[j] != 0 || j - i < 6) continue;
      std::string s(reinterpret_cast<const char*>(img + i), j - i);
      if (s.find("exec") != std::string::npos) {
        strings.emplace(kImageBase + uint32_t(i), std::move(s));
      }
      i = j;
    }
    string_count = strings.size();
    // 2. (name pointer, exec pointer) pairs; group names by exec address.
    std::unordered_map<uint32_t, std::string> by_addr;
    for (size_t off = 0; off + 8 <= size; off += 4) {
      uint32_t p, q;
      std::memcpy(&p, img + off, 4);
      std::memcpy(&q, img + off + 4, 4);
      p = xe::byte_swap(p);
      q = xe::byte_swap(q);
      auto it = strings.find(p);
      if (it == strings.end() || q < kImageBase || q >= kImageEnd || (q & 3)) {
        continue;
      }
      std::string& names = by_addr[q];
      if (names.find(it->second) == std::string::npos) {
        if (!names.empty()) names += '|';
        names += it->second;
      }
    }
    for (auto& [addr, names] : by_addr) {
      if (g_hooks.count(addr)) continue;
      auto hook = std::make_unique<Hook>();
      hook->kind = Hook::kNative;
      hook->index = int(g_natives.size());
      hook->addr = addr;
      hook->name = names;
      hook->state_change = names.find("execGotoState") != std::string::npos ||
                           names.find("execPushState") != std::string::npos ||
                           names.find("execPopState") != std::string::npos;
      g_natives.push_back(add(std::move(hook)));
    }
  }
  Log(fmt::format("[ntrace] {} native strings, {} exec addresses wrapped",
                  string_count, g_natives.size()));
  for (auto& h : g_ops) {
    Log(fmt::format("[strace] {} call handler {:08x} hooked", h->name,
                    h->addr));
  }
  XELOGI("gears3_trace: {} natives + {} script handlers hooked, log {}",
         g_natives.size(), g_ops.size(), cvars::gears3_trace_log.string());

  std::thread(Reporter, cvars::gears3_trace_file.string(),
              cvars::gears3_memdump.string(),
              std::max(5, int(cvars::gears3_memdump_every)))
      .detach();
}

}  // namespace

Function* EntryHookFor(Processor* processor, Memory* memory,
                       uint32_t address) {
  if (!cvars::gears3_trace) return nullptr;
  if (address < kImageBase || address >= kImageEnd) return nullptr;
  if (!g_initialized.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (!g_initialized.load(std::memory_order_relaxed)) {
      Initialize(processor, memory);
      g_initialized.store(true, std::memory_order_release);
    }
  }
  auto it = g_hooks.find(address);
  return it == g_hooks.end() ? nullptr : it->second->builtin;
}

}  // namespace gears3
}  // namespace ppc
}  // namespace cpu
}  // namespace xe
