# Frame cost on the phone

Measured on a Galaxy S24 (Exynos 2400, Xclipse 940, 2340x1080 panel), Snow Jam,
Peak 1 Race, the same run every time: `RECOMP_AUTONAV=race` drives boot to the
race by itself, so nothing depends on taps or on a scene change. 75 s sampled
per configuration; FPS from the presenter's own half-second count, GPU time from
`RECOMP_VKR_GPUPROF=8`, battery temperature from `dumpsys battery`.

Everything else was the settings the phone ships with: anisotropic 16x, draw
distance 2x, shadows 8x, widescreen.

| internal target | MSAA | FPS mean | FPS p10 | seconds under 55 | GPU avg ms | battery |
|---|---|---|---|---|---|---|
| 2340x1080 (panel) | 2x | 53.4 | 41.7 | 137/356 | 12.8 | 44.6 C |
| 2340x1080 (panel) | off | 53.7 | 43.3 | 125/358 | 8.8 | 44.9 C |
| 1280x720 | 2x | 53.8 | 44.8 | 147/359 | 8.8 | 43.7 C |
| 1280x720 | off | 55.7 | 47.5 | 97/357 | 9.0 | 44.1 C |
| 1040x480 (original) | off | 58.0 | 56.8 | 26/358 | 8.8 | 44.3 C |

## What the numbers say

* **MSAA is not the frame-rate lever.** Turning it off at the panel's resolution
  cut GPU time from 12.8 to 8.8 ms and moved the mean by 0.3 FPS.
* **Shadows 8x and draw distance 2x are not either.** Dropping both (with the
  original target) left GPU time at 8.8-9.2 ms.
* **The internal target is.** The game only ever draws 640x480; the renderer
  rasterises `RECOMP_RES/480` times that and then, every single frame, copies the
  whole image back to the CPU, blits a 640x480 copy for guest memory, copies
  that too, waits for the GPU to finish, gamma-maps the full frame on the CPU,
  and uploads it again as an SDL texture. At the panel's own height that is
  2340x1080 = 2.5 MPix, 10 MB down and 10 MB up per frame at 60 FPS.
* At 480 the renderer takes its `scale == 1` path: no full-resolution readback,
  no blit, one 640x480 copy. That is the only configuration measured that holds
  60 FPS (p10 56.8), so the present path, not the GPU, is what gives up frames.

## What to do about it

1. **Render resolution is a setting now** (Display, "Render resolution": Panel /
   720p / Original). It was hard-wired to the panel's short side; the row writes
   `RECOMP_RES` so the choice reaches the renderer.
2. **The heat is steady-state.** Battery temperature sat at ~44 C in every
   configuration, including the one that holds 60 FPS, so no setting above cools
   the phone — it converts spare GPU time into FPS, not into a cooler phone.
   Cutting heat needs less work per second, not less work per frame:
   - a 30 FPS mode (pace the guest's flips, not just the present), which halves
     the CPU and GPU work over a session;
   - removing the per-frame readback: present the Vulkan image through a
     swapchain, or read back the previous frame while the next one renders, so
     the CPU never waits for the GPU (`vkr_frame_pixels` -> `vkWaitForFences`).
3. `RECOMP_VBLANK_HZ=30` does **not** do it: the presenter still ran at 60 and the
   guest did not slow down.

## Reproducing

    scripts/perf_bench.sh A 1080 2      # label, RECOMP_RES, RECOMP_MSAA
    scripts/perf_bench.sh E 480 0

Each run writes `recomp.cfg` itself and starts the game activity directly, so the
launcher cannot overwrite the configuration under test; it then samples the race
and prints FPS, GPU time and temperature. `RECOMP_FPS_LOG=1` and
`RECOMP_VKR_GPUPROF=<ms>` are what make the numbers appear; both report through
logcat as the `SSX3PERF` tag.
