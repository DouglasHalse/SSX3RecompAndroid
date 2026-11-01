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

## Inside the frame: where the CPU goes

No perf events on this phone: `perf_event_open` refuses every event type for the
shell user (`cpu-cycles`, `cpu-clock`, `task-clock`, all "not supported on the
device"), so simpleperf is useless here. The sampler lives in the app instead
(`src/prof.c`): `RECOMP_PROFILE=<path>` arms `ITIMER_PROF` at 1 kHz,
`RECOMP_PROFILE_DELAY=<s>` holds it off until the loading screens are gone, and
each sample records the interrupted PC plus its host thread. Offsets are
resolved against the unstripped `libmain.so`, and system libraries by their own
`dladdr` module plus a pulled copy of the library.

A 20 s Snow Jam sample at 480p, by thread:

| thread | share | what it is doing |
|---|---|---|
| render / pushbuffer | 31% | `fetch_attr` 14%, `nv2a_exec_tex_info` 7%, `nv2a_pb_exec_method` 6%, `fetch_inputs` 5%, driver 9% |
| guest (recompiled) | 25% | `sub_00234A10` 7%, `recomp_fp_round24` 5%, linker64 11% |
| GPU register poller | 13% | 95% of its own samples in `sched_yield` |
| present (SDL thread) | 10% | `xbox_FramebufferApplyGamma` 27%, driver 14%, `libSDL2` 12% |

By module, and this is the part that decides what to work on:

| module | share |
|---|---|
| libmain.so (guest, renderer, toolkit) | 42% |
| libc.so — of which `sched_yield` alone | 33% — **16% of all samples** |
| vulkan.samsung.so | 12% |
| [vdso] (clock reads) | 4% |
| linker64 | 4% |

So a sixth of the CPU was one syscall: `YieldProcessor()` is `#define
YieldProcessor() sched_yield()` in the POSIX shim, so every spin in the toolkit
— and the GPU register poller is one — asks the scheduler for a context switch.
Turning it into the real pause instruction removed the yields (16% -> 0.1%) and
then measured **worse**: 49.0 FPS against 55.0 for the yield, back to back at
480p/41 C. The thread is latency-critical (the title spins on the PFB flush bit
it clears), so the spin defeats itself through power and cluster contention. The
shim keeps `sched_yield`, with that measurement in the comment; the fix that
would not trade one for the other is to wake the poller from the register write
that needs it instead of polling.

Two smaller notes from the same sample: `recomp_fp_round24` is 5% of the guest
thread (per-operation FP rounding for Xbox semantics, so it is correctness work,
not waste), and 11% of the guest thread sits in `linker64` — consistent with
general-dynamic TLS (`__tls_get_addr`) on the hot path, which the initial-exec
model would remove for a library that is loaded at start.

Sustained play is the other half of the heat story: after an hour of repeated
runs at 480p the same build measured 46 FPS at 44.5 C where it had started at
58 FPS at 39 C. The phone throttles, and every measurement above is only
comparable with an A/B run back to back and at the same battery temperature.

## Reproducing

    scripts/perf_bench.sh A 1080 2      # label, RECOMP_RES, RECOMP_MSAA
    scripts/perf_bench.sh E 480 0

Each run writes `recomp.cfg` itself and starts the game activity directly, so the
launcher cannot overwrite the configuration under test; it then samples the race
and prints FPS, GPU time and temperature. `RECOMP_FPS_LOG=1` and
`RECOMP_VKR_GPUPROF=<ms>` are what make the numbers appear; both report through
logcat as the `SSX3PERF` tag.
