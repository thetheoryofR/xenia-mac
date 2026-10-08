# Xenia Canary — macOS / Apple Silicon (Metal) Port: Dev Log & Build Guide

This document is the working record for bringing the Xenia Canary Xbox 360 emulator
up on **macOS (Apple Silicon, Metal)**. It is written for three audiences: the
maintainer, future debugging sessions, and any GitHub contributor who wants to pick
up where this left off.

- **Upstream port branch:** `github.com/wmarti/xenia-mac`, branch `xenia-canary-mac-rebase`
- **FFmpeg submodule:** `github.com/wmarti/FFmpeg`
- **Verified host:** Apple M3 Max, macOS 15.7.4, Xcode toolchain + Homebrew (arm64)
- **CPU backend:** `a64` (PowerPC → ARM64 JIT via oaknut)
- **GPU backend:** `metal` (brand-new, still maturing)

---

## 1. TL;DR status

| Stage | State |
|-------|-------|
| Build (arm64) | ✅ Works |
| App launch + Metal device init | ✅ Works |
| Game boot (XEX load, threads, kernel) | ✅ All 5 test titles reach guest `main` |
| First frames presented | ✅ (e.g. GoW2 renders the Epic logo) |
| Past the intro | ✅ **Fixed** — macOS 16 KB/4 KB page-protection desync (see §4); GoW2 reaches the dashboard/menu, GoW1/GoW3/Bully boot past their old freezes (0 host faults) |
| GoW1 menu / campaign-entry hang | ✅ **Fixed & confirmed** — XMA decoder `Work()→Clear()` non-recursive-mutex self-deadlock (see §4b). User played end-to-end with a real controller: menus → launch campaign → cutscene → in-game combat all work |
| GoW1 gameplay lag | ✅ **Fixed & confirmed** — debug-only `LogMetalRenderTargetTopLeftPixels` was stalling the GPU thread on `waitUntilCompleted` every resolve (~75% of GPU-thread time); gated behind `mtl_debug_log_rt_pixels` (default off, see §4c). User confirmed lag gone |
| GoW1 color corruption | 🔄 **Fix under test** — removing the per-resolve debug drain (lag fix) exposed a GPU sync hazard: full-screen green/magenta garbage. Replaced with one CPU↔GPU sync per frame at swap (`metal_sync_on_swap`, default on, see §4d). Awaiting in-game visual confirmation |
| Texture fidelity | ⚠️ Heavy corruption (tiling/swizzle/format in the Metal texture cache); Bully renders only static |
| Remaining blockers | GoW2 crashes in-menu (controller nav); GoW3 black screen; Bully intro YUV-as-RGB static; Splinter Cell ISO won't parse (`C00000BB`, bad dump); GPU corruption |

**Test library** (`/Volumes/Crucial X6/Games/XboX`, user-owned dumps): Gears of War 1/2/3,
Bully: Scholarship Edition, Splinter Cell: Conviction. All UE3 / RAGE AAA titles.

---

## 2. Build instructions (Apple Silicon)

Two host-environment gotchas dominate; get these right and the build is clean.

```sh
# 1. Put arm64 Homebrew FIRST on PATH. Intel /usr/local brew shadows sdl2/lz4
#    and silently produces an x86 / mismatched link.
export PATH="/opt/homebrew/bin:$PATH"

# 2. Pin the deployment target. This flows into the Metal shader compile
#    (see fix #2 below) and keeps the metallib VERS byte compatible with the host.
export MACOSX_DEPLOYMENT_TARGET=15.0

# Recursive clone is mandatory — a plain ZIP download leaves ~41 submodules empty.
git clone --recursive -b xenia-canary-mac-rebase https://github.com/wmarti/xenia-mac.git
cd xenia-mac

./xb setup
./xb build --arch=arm64
```

Output: `build/bin/Mac-ARM64/Debug/Xenia-Canary.app` (ad-hoc signed with the JIT
entitlements it needs to `mmap` executable pages).

### Running headless from the CLI (preferred for debugging)

The GUI takes the game as a positional argument, so you can launch a title directly
and capture logs without touching the picker:

```sh
BIN="build/bin/Mac-ARM64/Debug/Xenia-Canary.app/Contents/MacOS/xenia"
"$BIN" "/path/to/Game.iso" > /tmp/run.log 2>&1 &
```

`tools/mac/repro_hang.sh` (see §6) wraps this: it launches a title, waits for the log
to go quiet, captures a `sample(1)` of the still-running process, and kills it — a
deterministic freeze snapshot.

---

## 3. Fixes applied in this port effort

All of the following are candidates for upstream PRs. The first two are already
folded into the `wmarti` repos; the CPU-backend and kernel fixes below were made
during this debugging effort and are (as of this writing) local.

### Fix 1 — FFmpeg: missing generated list files (compile blocker)
`wmarti/FFmpeg` committed `libavcodec/{bsf,codec,parser}_list.c` but **not** the
`libavformat` equivalents (they are normally generated and `.gitignore`d). The build
fails to link without them. Created, force-added past `.gitignore`:

- `libavformat/protocol_list.c` → `{ NULL }`
- `libavformat/muxer_list.c` → `{ NULL }`
- `libavformat/demuxer_list.c` → `{ &ff_wav_demuxer (guarded by CONFIG_WAV_DEMUXER), NULL }`

Committed in the FFmpeg submodule as `2b2b6bd320`.

### Fix 2 — Metal: shader deployment target (runtime blocker)
Prebuilt `texture_load` metallib headers were stamped for a too-new OS
(VERS `0x00020008`), so on macOS 15 device creation failed with
*"Unable to setup command processor internal state."* Added
`-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET` to the Metal compile in
`xenia-build.py` (both the `xcrun` and direct `metal` command arrays under
`xb buildshaders`) and regenerated all 94 Metal headers (now VERS `0x00020007`).

### Fix 3 — a64 CPU backend: constant-operand handling
Several `a64` instruction sequences asserted `!is_constant` or dereferenced a
constant `ValueOp` as a register, causing `assert` failures / wild-pointer
`EXC_BAD_ACCESS`. Fixed by materializing constants into scratch V-registers
(`V0`–`V7`, `V16`–`V31` are scratch per `a64_emitter.h`; `Q0`/`Q1` are safe temps),
mirroring the existing `TOSINGLE_F64_F64` pattern:

- `SQRT_F32` / `SQRT_F64` — `src/xenia/cpu/backend/a64/a64_sequences.cc`
- `ROUND_V128` — same file
- `EmitCommutativeBinaryVOp` / `EmitAssociativeBinaryVOp` — `a64_op.h`
  (the gap was the **both-operands-constant** case)

**Debugging trick that cracked these:** a parallel `EXC_BAD_ACCESS` kept winning the
race against lldb. Temporarily adding `backtrace()` / `backtrace_symbols_fd()` inside
`ValueOp::reg()` (guarded by `is_constant`) and doing a plain run prints the
symbolized culprit Sequence deterministically.

### Fix 4 — kernel: unimplemented exports surfaced during boot (in progress)
`IoDismountVolumeByFileHandle` and `XeKeysConsolePrivateKeySign` log
*"undefined extern call"* (listed in `xboxkrnl_table.inc` but never registered).
The default handler returns 0, which appears benign for these two, but they are
stubbed properly as part of cleaning up the boot path.

---

## 4. The "intro hang" — ROOT CAUSE FOUND **and FIXED** (was the critical-path blocker)

> **No title is playable until it clears the intro, regardless of texture quality —
> so this was the #1 blocker. Fixed 2026-06-19.**

### The fix (TL;DR)
Two changes in `src/xenia/memory.cc` resolve it; afterwards GoW2 boots past the
intro to the dashboard/menu and GoW1/GoW3/Bully clear their old early freezes
(verified on a Release build, 0 `Pause()`/`ExceptionCallback`):

1. **`BaseHeap::Protect` (macOS branch)** — previously *skipped* the host
   `mprotect` whenever the guest's 4 KB-granular protect range wasn't 16 KB
   (host-page) aligned. That let the real host page protection drift out of sync
   with the guest page table. It now sets each 16 KB host page to the
   **union (most-permissive) access of the four 4 KB guest pages it covers**, so
   a write to any guest page the table believes is writable never spuriously
   faults. (Trade-off: a read-only guest page sharing a host page with a writable
   one isn't hardware-enforced — acceptable, and far better than a hard freeze.)
2. **`PhysicalHeap::TriggerCallbacks` (write-watch unprotect loop)** — when
   deciding whether to make a watched system page writable again, it now checks
   **all** guest sub-pages the 16 KB system page covers, not just the single
   offset-shifted one (which, under `host_address_offset = 0x1000`, pointed at the
   wrong 4 KB page and left the host page read-only while clearing the watch bit).

### Why it looked like a hang (the original analysis)
It is **not** a deadlock and **not** a GPU/vblank stall. It was a **guest CPU
exception (access violation) in JIT-compiled code** that tripped Xenia's host
exception handler, which calls `Emulator::Pause()`. `Pause()` suspends the GPU
command-processor thread and parks the emulator — so the symptom *looks* like a
hang (Main XThread polling an unsignaled event, GPU command ring idle), but the
real event is a guest crash on one worker thread.

### How it was traced (so the method is repeatable)
1. `sample(1)` of the frozen process: Main XThread parked in
   `NtWaitForSingleObjectEx`; **GPU Commands** thread parked in
   `CommandProcessor::Pause()::$_0 → Thread::Suspend()` — i.e. the GPU CP was
   *paused*, not merely idle.
2. `Emulator::Pause()`'s only caller path during a run is `ExceptionCallback`
   (`emulator.cc`) — the host SIGSEGV handler for faults inside guest JIT code.
   `--no_reserved_ops=true` did **not** change the hang, ruling out lwarx/stwcx.
3. A diagnostic added at the top of `ExceptionCallback` (still in tree, fires only
   on a crash) prints the fault. Result, reproducible every run:

```
ExceptionCallback code=1 host_pc=…480 guest_pc=82A92D18
   fault_addr=00000003F6871000 op=2(write) insn=[std r6, 0x8(r3)]
   r3=00000000F686FFF8 r6=000000000000000A virtual_membase=0x300000000
```

### The actual bug (macOS 16 KB host pages)
The faulting guest instruction is an ordinary `std r6, 0x8(r3)` with
`r3 = 0xF686FFF8` → guest EA `0xF6870000`, which lies in the **`0xE0000000`+
"physical 4 KB pages" window**. Arithmetic:
- naive host = `virtual_membase + EA` = `0x3F6870000`
- observed fault = `0x3F687`**`1`**`000` = exactly **+0x1000** higher

That `+0x1000` is the CPU-side emulation of the `0xE0000000` window's 4 KB physical
offset (`a64_seq_memory.cc`/`ComputeMemoryAddress`, applied when
`allocation_granularity > 0x1000`; macOS = 16 KB). The offset itself is by design
(`memory.cc` `map_info` target `0x100001000`; `host_address_offset = 0x1000`). The
fault means that **host page is not writable**, and the write-watch / crash handler
fails to claim it. The prime suspect is the macOS page-size workaround in
`BaseHeap::Protect` (`memory.cc:1434`):

```cpp
#if XE_PLATFORM_MAC
    XELOGW("BaseHeap::Protect: unaligned to host page size; skipping mprotect");
#endif
```

Guest 4 KB-page protect/commit requests that aren't 16 KB-aligned are **skipped**,
so the host page protection silently diverges from the guest page table. This same
warning appears in the log on the very threads that crash. Net effect: a physical
page the guest believes is committed/writable is left inaccessible (or left
read-only by an earlier write-watch on the shared 16 KB host page), and the next
guest write faults straight through to the crash handler.

### Confirmation (diagnostic that proved it)
A temporary diagnostic dumped the page state at the fatal fault:
`va=F6870000` had page-table protection `0xB` (Read|Write|WriteCombine = writable)
but its 16 KB host page was read-only **and** its write-watch bit was clear — the
exact host↔table desync described above. After applying the two fixes (union host
protection in `BaseHeap::Protect` + all-sub-pages check in `TriggerCallbacks`), the
fault disappears and GoW2 advances to the dashboard. The fixes are the resolution
of the options that were evaluated here (host-page-granular `mprotect`, plus making
the watch unprotect path agree with the actual written page under
`host_address_offset = 0x1000`).

---

## 4b. GoW1 menu / campaign-entry hang — XMA audio decoder self-deadlock **FIXED**

Distinct from the §4 intro hang (a CPU access-violation). This one strikes
**later** — navigating Gears of War's menus or entering Single Player freezes the
whole emulator. It is a genuine **recursive self-deadlock on a non-recursive
mutex** in the XMA audio decoder.

### Root cause
`src/xenia/apu/xma_context_new.cc`. Each `XmaContextNew` has a non-recursive
`lock_` (`xe_mutex`; both the `xe_fast_mutex` spinlock and the `std::mutex`
variant in `base/mutex.h` are non-recursive). The decoder worker thread runs:

```
XmaDecoder::WorkerThreadMain()            // xma_decoder.cc
  → XmaContextNew::Work()                 // takes lock_  (line 119)
      → Clear()                           // RE-takes lock_  → DEADLOCK
```

`Work()` acquires `lock_`, and for a *consume-only* context whose output has
fully drained (menu audio finishing) it called `Clear()` — which acquires
`lock_` again. The worker thread then blocks forever **while holding `lock_`**.
That in turn wedges the guest thread, which reaches the same context through an
MMIO register write:

```
guest JIT store → MMIOHandler::ExceptionCallback
  → XmaDecoder::WriteRegister → XmaContextNew::Disable()  // wants lock_ → blocked
```

Both threads are now stuck → full freeze. (Confirmed by `sample`: the audio
worker blocked in `std::mutex::lock()` under `xma_context_new.cc:134 → Clear()`,
the guest thread blocked in `std::mutex::lock()` under `Disable()`.)

### The fix
Split a lock-free body out of `Clear()`:

- `ClearInternal()` — the original body, **no locking**; caller must already
  hold `lock_`.
- `Clear()` — public; takes `lock_`, then calls `ClearInternal()`.
- `Work()` (already holding `lock_`) now calls `ClearInternal()` directly
  instead of `Clear()`.

External callers that hold no lock (e.g. `XmaDecoder::WriteRegister` →
`context.Clear()` in `xma_decoder.cc`) keep using the locking `Clear()`. Audited
every `lock_` acquisition and every intra-class method call in
`xma_context_new.cc`: `Work→Clear` was the **only** reentrant path; `Enable`,
`Block`, `Disable`, `Release` each lock once with no nested locking call.

### Confirmation
Post-fix `sample` of a live GoW1: the XMA worker cycles normally — parked in
`condition_variable::wait` between jobs, occasionally active at
`XmaContextNew::Work()` entry — and **no** thread is blocked on
`std::mutex::lock()` in the XMA path. The GPU command processor renders draws
concurrently. End-to-end menu/Single-Player confirmation requires a real
controller (see §7).

## 4c. GoW1 gameplay lag — GPU debug readback stalling every resolve **FIXED**

Once GoW1 was playable (§4 + §4b), the campaign ran but with heavy lag. A 5 s
`sample` of the live, in-game process pinned it precisely: the **GPU Commands**
thread spent **~75 %** of its time (≈2700 of 3379 samples) in
`MetalRenderTargetCache::Resolve → LogMetalRenderTargetTopLeftPixels →
-[_MTLCommandBuffer waitUntilCompleted]`.

`LogMetalRenderTargetTopLeftPixels` is a **debug-only** helper (its own comment:
"intended only for debugging, not for production") that reads back the top-left
pixels of a render target and logs them. It was being called on **every**
render-target resolve (many per frame) and each call issues a command buffer and
**blocks the GPU thread on `waitUntilCompleted`** — a full CPU↔GPU round-trip
that serializes the entire pipeline. It renders nothing; it is pure diagnostic
overhead that had been left enabled.

### The fix
Added cvar `mtl_debug_log_rt_pixels` (`DEFINE_bool`, **default `false`**,
category `GPU`) in `metal_render_target_cache.cc`, and an early-return at the top
of `LogMetalRenderTargetTopLeftPixels` when it is unset. Both call sites (the
resolve-path inspection ~L4501 and the RT0 wrapper ~L3781) route through that
function, so both are gated by the single guard. Set the cvar to `true` only when
diagnosing render-target contents.

### Other `waitUntilCompleted` sites — left intact (legit sync, not debug)
Swept the whole Metal backend. The remaining waits are correctness syncs, **not**
debug stalls, and were **not** hot in the profile, so they were not touched:
staging-buffer→texture upload completion (`metal_render_target_cache.cc:3166`),
and EDRAM dump/resolve completion guarded by `owns_command_buffer`
(`:4162`, `:4758`) which must finish before the result is read back into guest
memory. Removing these blind would risk rendering corruption. The correct way to
find the next bottleneck is to re-profile the post-fix build in-game.

## 4d. GoW1 color corruption — exposed by the §4c lag fix **FIX UNDER TEST**

After §4c the lag was gone (user confirmed: "there was no lag"), but gameplay now
showed **severe full-screen color corruption** — per-pixel green/magenta garbage
with geometry edges faintly visible underneath (not a coherent tint, so **not**
YUV-as-RGB; it is a GPU **synchronization hazard**, i.e. reading a render
target / resolve target whose producing GPU work has not completed).

### Root cause — the removed debug drain was a load-bearing barrier
The §4c readback (`LogMetalRenderTargetTopLeftPixels`) did far more than log: each
call committed a command buffer and **drained the entire GPU** on
`waitUntilCompleted`, on **every resolve**. That accidental full-GPU drain was
silently serializing the frame, masking a real race. On the old build the picture
was correct *because of* that expensive sync; removing it (the lag fix) let the
presenter read the swap texture while the frame's draws/EDRAM resolves were still
in flight → garbage. Confirmed by timeline: old build = correct-but-laggy; §4c
build = smooth-but-corrupt; the only graphics change between them was gating that
readback.

Topology checked while diagnosing: in `MetalCommandProcessor::IssueCopy` the draw
render-encoder is ended and `DumpRenderTargets` shares the **same**
`current_command_buffer_`, committed without waiting; heap resources are
`HazardTrackingModeTracked` and there are no manual fences. So GPU↔GPU ordering
*within* the buffer is automatic — meaning the masked race is the broader
**CPU↔GPU / cross-frame** drain at present, not one encoder pair.

### Diagnosis chain (each step confirmed in-game by the user)
1. **Per-frame sync** (`metal_sync_on_swap`, `IssueSwap` waits once before
   present): corruption became **intermittent** — most frames clean, some still
   garbage. So the hazard is **intra-frame**, not just at present; a coarse
   per-frame sync can't catch a resolve target consumed mid-frame.
   (Note: macOS `screencapture` of the window always shows clean frames — the
   window-server composites complete drawables — so host screenshots **cannot**
   see this corruption; only the in-game capture path or the user's eyes can.)
2. **Per-resolve sync** (`metal_sync_on_resolve`, `IssueCopy` waits after each
   resolve commit): **fully clean, but laggy again** — confirming the hazard is
   exactly the per-resolve consume-before-complete race, and that a CPU drain per
   resolve is both the cure and the §4c lag source.

### Mechanism — zero-copy unified memory
`metal_shared_memory_zero_copy = true` (default): on Apple Silicon the guest-RAM
MTLBuffer is `bytesNoCopy`-mapped, so a GPU resolve write and a later CPU read hit
the **same physical bytes**. Same-queue command-buffer ordering keeps *GPU↔GPU*
correct, but a CPU consumer (e.g. a texture upload's `memcpy`/readback path) can
read the region before the resolve's GPU write is visible → garbage. This is why
GPU-side fences alone would not fix it.

### The proper fix (under test) — invalidate, don't drain
Mirror the D3D12 backend: after a resolve, call
`shared_memory_->MemoryInvalidationCallback(written_address, written_length, true)`
(+ `primitive_processor_`). That fires the memory watches the texture cache
registers (`texture_cache.cc` `WatchMemoryRange`), marking textures sourced from
the resolved region dirty so they are **re-uploaded from the fresh data** (via a
GPU blit ordered after the resolve on the same queue) — a cheap, targeted
alternative to draining the whole GPU after every resolve. Enabled the previously
commented-out invalidation in `IssueCopy`.

### Cvar levers (all in `metal_command_processor.cc`, category `GPU`)
- `metal_sync_on_swap` (default **true**) — one CPU↔GPU sync per presented frame.
- `metal_sync_on_resolve` (default **true**) — CPU drain after every resolve;
  **known-correct but reintroduces lag**. The safe fallback.
- Post-resolve texture-cache invalidation — always on (the intended fix).
- `metal_shared_memory_zero_copy` (config, default true) — set **false** to make
  guest memory a separate, explicitly-synced copy: another way to remove the
  CPU↔GPU coherency race, at a memory-bandwidth cost.

**Goal config:** invalidation on + `metal_sync_on_resolve=false` (no per-resolve
drain) → clean **and** smooth. Status: **awaiting in-game confirmation** that
invalidation alone holds without the drain.

## 5. Texture corruption (secondary blocker)

Confirmed **upstream of final present** — the present path logs a clean
`present path=gamma src=70 dst=70 pwl=0`; corruption is already in the sampled
textures. Suspects, in the Metal backend: EDRAM render-target cache resolve, and
texture tiling/untiling (the `texture_load` compute shaders) — swizzle / tile-order /
format handling. Not yet attacked in depth; the intro deadlock takes priority.

---

## 6. Reproduction & diagnostic recipes

**Capture a freeze snapshot:**
```sh
"$BIN" "/path/to/Game.iso" > /tmp/run.log 2>&1 &   # launch headless
PID=$!
# wait until /tmp/run.log stops growing, then:
sample $PID 3 -file /tmp/sample.txt                # 3s stack sample
kill -9 $PID
```

**Read a guest thread's wait state** in the sample: each guest thread shows as
`Thread_… : <name>` with a stack ending in `NtWaitForSingleObjectEx` /
`condition_variable::wait*` / `spin_wait`. The Main thread is named `Main XThread`.

**Find unimplemented kernel imports:**
```sh
grep "undefined extern call" /tmp/run.log | sort | uniq -c
```

**Watch VFS path resolution (intro-movie / content lookups):**
```sh
grep -E "ResolvePath|NtStatusToDosError" /tmp/run.log
```

---

## 7. Open task list

1. **Identify the intro deadlock object** via the `NtWaitSingle` trace (in progress).
2. Determine the missing signaler / why the intro-movie path fails to resolve.
3. Attack texture corruption (EDRAM resolve + texture tiling) in the Metal backend.
4. Re-verify all 5 titles after each fix.
5. Once a title is playable for a sustained session, open the upstream PRs
   (FFmpeg list files, Metal deployment target, a64 constant handling, kernel stubs).
