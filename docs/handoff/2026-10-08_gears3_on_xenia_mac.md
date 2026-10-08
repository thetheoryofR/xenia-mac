# Handoff: Gears of War 3 on xenia-canary-mac (2026-10-08)

## Why

The native Gears 3 build (`~/Coding/personal_projects/gears3-recomp`, a static recompilation on the ReXGlue SDK,
which is Xenia-derived and uses Vulkan on MoltenVK) plays into Act 1. But on the Sovereign's deck the **in-engine
cutscene that introduces a new enemy never starts**: gameplay continues through a scripted explosion sequence
and you get "OBJECTIVE FAILED — User has died", every time. To tell a recompilation bug from a shared
runtime/GPU behaviour, we need Xenia to reach the same spot. **The goal of this work: get Gears 3 running in this
Xenia far enough to see whether that cutscene plays.** Playable quality is a bonus.

## Current state (as found today)

- **Build:** [Xenia-Canary.app](file:///Users/robertgraham/Coding/xenia-canary-mac/build/bin/Mac-ARM64/Release/Xenia-Canary.app),
  branch `xenia-canary-mac-rebase` @ `f19e7dbb9` (2026-06-20). It is a Metal port (`MetalGraphicsSystem`); the dev
  log is [docs/mac_port.md](file:///Users/robertgraham/Coding/xenia-canary-mac/docs/mac_port.md). Gears of War 1 is
  known to work.
- **Gears 3 with `--gpu=metal` (the default):** boots, but **the screen is total garbage** (green/black striped
  blocks, screenshot in the gears3-recomp session) and **the audio is noise**.
- **Gears 3 with `--gpu=vulkan`:** `x> 0000B003 Unable to initialize the graphics subsystem.` The Vulkan
  backend isn't usable in this Mac build (not built or no MoltenVK found). Log:
  [xenia_vulkan.log](file:///Users/robertgraham/Coding/personal_projects/gears3-recomp/build/runs/xenia_vulkan.log).
- **Launching:** run the binary directly; `open -a` right after killing it fails with LaunchServices error -600.
  ```
  ~/Coding/xenia-canary-mac/build/bin/Mac-ARM64/Release/Xenia-Canary.app/Contents/MacOS/xenia \
    "/Volumes/Crucial X6/Games/XboX/Gears of War 3 (World) (En,Fr,Pt,Zh,Ko,Pl,Ru,Cs,Hu).iso"
  ```
  Title ID `4D5308AB`, disc version `0.0.0.11`, no title update.
- **Storage:** `~/.local/share/Xenia/`, holding `xenia-canary.config.toml`, `content/` and `cache*`.
  The profile is `E0300000040A45EE`.
- **Save copied in today:** the gears3-recomp checkpoint now sits at
  `~/.local/share/Xenia/content/E0300000040A45EE/4D5308AB/00000001/GearsCheckpoint/GearsCheckpoint{0,1,2}.sav`
  (plus `PlayerStorage.dat` and its header). Copied from
  `/Volumes/Crucial X6/gears3-recomp/userdata/B13EBABEBABEBABE/4D5308AB/`. Xenia had no Gears 3 content before, so
  nothing was overwritten. It is **untested** whether Gears accepts a checkpoint made under another profile
  (ours is `B13EBABEBABEBABE`). If Continue doesn't show it, play from the start of Act 1 (about 10-15 min):
  through the ship, past "Shit! They got Nash!", then up the stairs to the deck. That is where the cutscene should
  trigger.

## Lessons from the gears3-recomp session that likely apply here

The same game was debugged on the same Mac today. Each finding below is measured, and the gears3-recomp file is
the reference:

1. **Native BC (DXT) mips break on Apple GPUs.** Garbage colours on distant surfaces disappeared once BC textures
   were decoded to RGBA8 instead of uploaded natively. This Metal backend already has
   **`--metal_force_bc_decompress=true`**; try it first, since it may explain part of the garbage screen.
   (gears3-recomp: `third_party/rexglue-sdk/src/graphics/vulkan/texture_cache.cpp`, "S3TC" section.)
2. **7e3 render targets need clamping in the host-RT path.** `k_2_10_10_10_FLOAT` is stored as RGBA16F, whose
   maximum is 65504, whereas the 360 caps it at 31.875. Unclamped output caused over-bright objects. Our clamp
   didn't fully cure the glow, so treat this as hygiene, not a fix.
3. **macOS uses 16 KB host pages, the 360 4 KB.** mac_port.md §4 already hit this (the intro hang). Watch for it in
   any memory-watch or invalidation code.
4. **Gears 3 renders its HDR frame in tiles:** about 1,400 EDRAM resolves per second, each followed by a texture
   reload. Expect resolve/texture-cache churn to dominate GPU time.
5. **Check GPU contention before any performance work.** A loaded **LM Studio** model (its `node` worker) took
   about 90% of GPU compute and made the game look GPU-bound. Unload it first.
6. **In a Metal System Trace, attribute GPU time per process.** Join `metal-gpu-submission-to-command-buffer-id`
   with `metal-application-encoders-list`. The script is
   [attrib.py](file:///Users/robertgraham/Coding/personal_projects/gears3-recomp/build/runs/ab30/attrib.py); record
   with `xcrun xctrace record --template 'Metal System Trace' --attach <pid> --time-limit 5s`.
7. **Disc-check imports** (`XamMediaVerification*`) are looked up by ordinal and polled every ~0.5 s. That is a
   harmless background loop. Leave them as stubs (no DRM work).

## Suggested order

1. `--metal_force_bc_decompress=true`: does the garbage turn into a picture?
2. If the picture is still broken, compare the striping against mac_port.md's known Metal issues (render-target
   formats, resolves). The stripes look like a wrong tiling or pitch on the presented frontbuffer or on resolves.
3. Audio noise: check the XMA decoder path. The recomp's audio works on the same disc, so compare decoder
   settings.
4. As soon as it's watchable, play to the deck (or Continue from the copied checkpoint), and record **whether the
   enemy-intro cutscene plays**. Report back to the gears3-recomp session (card BOOT-3 territory; see
   `gears3-recomp/docs/status/BOOT-1.md` and `GFX-3.md`).

## Rules carried over

- Never commit game data, saves, ISOs or extracted assets to either repo. Saves live in `~/.local/share/Xenia`
  and on the X6 only.
- Warn before launching anything that takes focus.
