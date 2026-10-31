# SSX 3 (Xbox) static recompilation — agent notes

Static recompilation of SSX 3 (PAL, title ID 0x4541003C, XDK 5558, built
2003-09-19, entry point 0x00181AD2) with
[sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp), from the
owner's own disc rip. Target: the toolkit's Windows/MSVC/D3D11 path, driven
from WSL.

**Status (2026-10-13):** playable. Boots, intro, menus, race load and live
races on Snow Jam with HUD, rider shadows and audio; 9-12 FPS in a race
with the toolkit's software rasteriser, now that the GPU executor runs
asynchronously by default (RECOMP_NV2A_ACK_THREAD=1, main.c; =0 restores the
old lock-step mode at 4-7 FPS). `tools\play.bat` starts it with the
keyboard as player 1 (an XInput pad also works). Reliability boot -> race
briefing ~8/9 runs (an intermittent race-load stall at 43% remains). Open:
speed (no GPU path), an intermittent black shadow wedge in races, the
pink-lit start area. Details under "Known open items" below; the toolkit
changes are all in `patches/xboxrecomp-local.patch` (base 193e299).

**Vulkan renderer (2026-10-14, the default; `RECOMP_GPU=sw` = software):** replaces the software
rasteriser with `src/vk/` (from the `linux` branch, docs/VULKAN.html, Linux
notes in docs/LINUX.md). Windows needs the Vulkan SDK (`winget install
KhronosGroup.VulkanSDK`; `tools\build.bat` finds it). Race 53-58 FPS on
Windows (p50 17 ms) vs ~12 in software -- see ROADMAP 2.0c for the steps
(readback memory, trap waits, piecewise submits, render-target textures in
place, vertex cache, and the vblank clock that capped it at ~43). MR !3
(two frames in flight, pre-decoded vertex programs, LensFX nowait) is
merged; `src/vk/vkr_thread.h` maps its pthread calls on Windows. Vertex
programs run on the GPU (ROADMAP 2.0d, `xbox_vsh.vert`, RECOMP_GPU_VSH=0 =
CPU): race at a steady 60 FPS. The black
shapes over the race backdrop were the executor dropping ARRAY_ELEMENT32
(0x1808, the odd last index of an index list) -- fixed in the toolkit patch.

**Modernising (2026-10-19):** [docs/MODERNIZE.md](docs/MODERNIZE.md) is the work queue for the PC upgrades (anisotropic filtering, draw distance, HUD at the edges, shadows, loading, >60 FPS, reverb): one step per commit, first unticked box next.

**Roadmap:** [docs/ROADMAP.md](docs/ROADMAP.md) is the plan and tracker (phases,
gates, checkboxes, progress log). Work from it: pick the next unchecked item of
the earliest open phase, tick it in the commit that lands it, and add a dated
line to its progress log.

## Layout

```
<parent>/                      ~/Code/Xbox on the original machine
  xboxrecomp/                  toolkit clone: v0.13.1 (193e299) + patches/xboxrecomp-local.patch
  ssx3-recomp/                 this repo
  SSX3/                        the rip: default.xbe + data/ (NOT in git; default_analysis.json is written here)
C:\Code\Xbox\{xboxrecomp,ssx3-recomp}   Windows build copy; game data in ssx3-recomp\game\
```

In this repo:

| Path | What |
|---|---|
| `src/main.c` | Host entry (from the template). Fills in SSX 3's entry point, registers the D3D fence mirror, sets the runtime env defaults SSX 3 needs (see below), starts the thread dumper. |
| `src/ssx3_overrides.c` | Hand-written replacements for generated functions (`--exclude-manual` skips their bodies) + bring-up tracing wrappers. |
| `src/recomp_manual.c` | Template file; `recomp_lookup_manual` routes indirect calls to `ssx3_lookup_trace`. Do not use its `extern uint32_t g_eax` — see gotchas. |
| `src/thread_dump.c` | `RECOMP_THREAD_DUMP_SECS=N`: native stack + guest registers of every thread. |
| `src/settings.c`, `src/in_game_menu.c` | The in-game options screen: the row table, `ssx3.cfg`, and the drawing/input for the screen itself. The presenter draws it through the hooks in the toolkit's `src/video/ui_overlay.h`; `tools/menu_shot.ps1` drives it on Windows, `tools/menu_render.bat` renders it on the host (Android branch included). |
| `tools/build.bat` | MSVC + Ninja release build (Windows), `[jobs]` arg, default 16. |
| `tools/run.ps1` | Run with stdout/stderr capture, timeout kill, `-Watchdog N`, `-ThreadDump N`, `-Env @{...}`. |
| `tools/shot.ps1` | Run, screenshot the game window after `-After N` s, kill. |
| `tools/race_test.ps1` / `tools/race_par.ps1` | Boot→race reliability runs; `race_par.ps1 -Instances 8 -Runs 2 -Env @{...}` runs 8 instances at once in `work\i<k>\` (logs in `work\i<k>\logs\` and `work\i<k>\stderr.txt`; summary in `logs\race_par_*.txt`). `live.ps1`/`to_race.ps1`/`race_test.ps1` take `-Dir <instance dir>`. `to_race.ps1` has a watchdog (ends a run with `stuck: [no pad]` / `[freeze]` / `[<state> stall]`, see its header). |
| `tools/play.bat` | **Play it**: starts the build with `RECOMP_KEYBOARD=1` (keyboard = player 1, merged with any XInput pad); key map in the file; logs to `play_std*.txt`. |
| `tools/split_named.py` | `tools.split` with Ghidra names kept (upstream drops them). |
| `scripts/setup.sh` | Clone toolkit at the base commit, apply the local patch, make the venv. |
| `scripts/regen.sh` | Re-run the pipeline: analysis, `decomp/` split, `src/recomp/gen/` lift. |
| `scripts/sync-windows.sh` | rsync to `/mnt/c/Code/Xbox` (`--game` copies the rip once). |
| `analysis/ghidra_names.json` | 143 FidDb CRT/XDK names from Ghidra 12.1.4, so regen needs no Ghidra. |
| `patches/xboxrecomp-local.patch` | Toolkit fixes made during bring-up (upstream candidates). |
| `docs/screenshots/` | Progress screenshots (LFS). |

Not in git (regenerate with `scripts/regen.sh`): `src/recomp/gen/` (77 files,
1.7M lines of C) and `decomp/` (19,096 byte-exact `.s` files) — both are the
game's code. Nor the rip, `build/`, logs.

## Fresh machine

```bash
scripts/setup.sh                 # toolkit + venv (needs git, curl, python3)
scripts/regen.sh                 # ~2 min; needs ../SSX3/default.xbe
scripts/sync-windows.sh --game   # first time; later without --game
cmd.exe /c 'C:\Code\Xbox\ssx3-recomp\tools\build.bat'     # full build ~2 min (32 threads)
cd /mnt/c/Code/Xbox/ssx3-recomp && powershell.exe -NoProfile -ExecutionPolicy Bypass -File 'C:\Code\Xbox\ssx3-recomp\tools\shot.ps1' -After 35
```

Windows needs VS 2022 with the C++ workload (MSVC 14.43 used; CMake and Ninja
come with it). WSL interop drives everything: `cmd.exe`/`powershell.exe` run
fine from a `/mnt/c` cwd. Run from the project root — the exe opens
`game\default.xbe` relative to the working directory.

## Iteration loop

1. Edit in WSL (`src/`, or the toolkit — then refresh the patch:
   `git -C ../xboxrecomp diff > patches/xboxrecomp-local.patch`).
2. Added/removed an override in `src/ssx3_overrides.c`? `scripts/regen.sh lift`
   (recomp_funcs.h stays the same since 2026-10-16, so only the file holding the function recompiles). Otherwise no regen.
3. `scripts/sync-windows.sh` then `tools\build.bat` (seconds when only
   hand-written files or one toolkit lib changed).
4. `tools\run.ps1` / `tools\shot.ps1`, read `C:\Code\Xbox\ssx3-recomp\stderr.txt`.

## Diagnosing

- **Hang**: `run.ps1 -Seconds 50 -Watchdog 40 -ThreadDump 35`. Pipe the
  `[THREADS]` block through `../xboxrecomp/tools/symbolize.py build/ssx3_recomp.map`
  (map is in the Windows build dir) — generated functions are native symbols
  named `sub_<guest VA>`, so the native stack *is* the guest call chain; guest
  regs per thread come from their TLS. The watchdog block goes through
  `tools/stackwalk.py tools/disasm/output/functions.json`.
  The main thread parked in `sub_001ADF40` is the normal frame wait, not a hang.
- **Crash**: `[CRASH]` in stderr, or Windows event log (Id 1000) fault offset;
  `symbolize.py` on `0x140000000 + offset`. The template VEH silently ignores
  faults in native 0xFD000000–0xFE000000.
- **Read code**: `decomp/asm/<section>/sub_XXXXXXXX.s` (disassembly is in the
  comments); `decomp/manifest.json` has callers/callees.
- Toolkit env knobs: `RECOMP_VSH_TRACE`, `RECOMP_VSH_DUMP`, `RECOMP_PB_EXEC_VERBOSE`,
  `RECOMP_PB_UNHANDLED_ALL`, `RECOMP_NV2A_TRACE`, `RECOMP_PB_REPORT_MS`,
  `RECOMP_PEEK`, `RECOMP_POKE`, `RECOMP_WATCH`, `RECOMP_UNIMPL_TRAP`
  (see `../xboxrecomp/docs/pipeline/06-debugging.md`).
  `RECOMP_TRACE_FRAMETIMER=1` (ours) logs the frame-timer object's methods.

## What was fixed, in boot order

| Blocker | Cause | Fix |
|---|---|---|
| Quick-reboot at startup | XAPI media check `sub_00183EBB` reads the DVD security sector and verifies its RSA signature; impossible from a file rip | Override returns 0 (its own "no check needed" path) — `src/ssx3_overrides.c` |
| Spin in D3D `sub_00234BE0` (BlockOnTime) | GPU completion `*[dev+0x30]` never catches up with `[dev+0x2C]`; device ptr at 0x0024B0D8 | `xbox_Nv2aMirrorFence(0x0024B0D8, 0x2C, 0x30)` in main.c |
| XAPI timer thread `sub_0021C1B5` spinning | Toolkit `KeWaitForMultipleObjects` treated KTIMER/KEVENT pointers as handles; thread also builds its KEVENT by hand | Patch (kernel_bridge.c) + `RECOMP_TITLE_KEVENTS=1` |
| D3D waits on vblank event 0x0024CE8C between AvSetDisplayMode steps | No vblank interrupt delivered | `RECOMP_VBLANK=1` |
| Crash AddRef'ing 0xFFFFFFFF in DSOUND `sub_0025FC35` | DirectSoundCreate failed (E_OUTOFMEMORY): AC'97 codec not ready | `RECOMP_AC97_READY=1` |
| DSOUND spins at 0x0025BEE8 after effects download | Posts command 3 to GP DSP at scratch+0x810 and waits for a clear; DSP is a stub | `RECOMP_DSP_ACK=0x83818810` (fragile: allocation-order dependent; real fix = DSP stub acks) |
| All sprite triangles NaN/degenerate | Toolkit executor fed arrayless vertex attributes 0,0,0,1; SSX 3's sprite program reads its transforms from v8..v15 set by SET_VERTEX_DATA4F | Patch (nv2a_pb_exec.c `transform_vertex`) |
| Nothing visible | Window/executor are opt-in | `RECOMP_FB_WINDOW=1`, `RECOMP_PB_EXEC=1` |
| Stuck on "Loading..." (fade never ends) | State machine `sub_001AD880` keeps 0 in ebx; the loading draw `sub_001AD7B0` calls CRT `exp()` (`sub_00213850` → `ctrandisp1`), whose lifted x87 dispatcher returns with ebx = 0x000D1284 (confirmed by the ABI check; most likely its `pop ebx` reads a stale slot because a computed-jump tail leaves guest esp 4 high — not yet verified). `cmp [esi+0x38], bl` then never matches → no render → update counter (`app+0x20`) never resets → updates stop at the 12-per-frame cap. Found with `tools\build.bat 16 abi` (`[ABI] sub_00213850: ebx 0->000D1284`) | Native `exp`/`fmod` overrides (`sub_00213850`, `sub_00212C18`, the only stubs into ctrandisp1/2) in `src/ssx3_overrides.c` |

All env defaults are set in `src/main.c` only when unset, so a run can override them.

## Known open items / next steps

- Status (2026-10-13): PLAYABLE to gameplay, reliably (3/3 boot→race) —
  boot → intro → menus → race load → live race on Snow Jam with full HUD
  (docs/screenshots/12; 13-15 in git history), 6-8 FPS (was ~4-5) with the toolkit's software rasteriser (multithreaded; the D3D11
  pgraph translator is not used for vertex-program titles). The fix that got
  the race to load was seeding script native 0x69 (below). Next: speed
  (hardware rendering), audio output check, real-pad/keyboard play.
- Performance (measured with the profiler, `tools\live.ps1 dump -Mem profile`
  + `tools/profile_tally.py build/ssx3_recomp.map stderr.txt`): in a race the
  main thread spends ~86% spinning in D3D kickoff `sub_00234A10`, waiting for
  the PFB flush bit (NV2A 0x100410 bit 16) to clear. The toolkit clears it in
  `nv2a_ack_thread`, the same loop that executes and software-rasterises the
  pushbuffer, so title and "GPU" run strictly in turn and FPS = rasteriser
  speed (~5 with 300-850 draws; most triangles are below the worker-pool
  threshold and are drawn on the executor thread). A dedicated ack thread
  (toolkit patch, opt-in `RECOMP_NV2A_ACK_THREAD=1`) removes the serialisation
  but then the title wraps the pushbuffer ring more than once per executor
  pass and stalls in MakeSpace (0x00234BE0 → 0x00234850): needs multi-wrap
  ring tracking or PUT throttling. Follow-up (in the toolkit patch, active
  only with that flag): the executor now performs BACK_END_WRITE_SEMAPHORE_
  RELEASE (0x1D70, D3D's fence "time"; SSX 3 emits thousands) via
  `xbox_Nv2aSemaphoreRelease`, writing the fence (`*[dev+0x30]`) and PGRAPH
  0x400B10, and the fence mirror stands down. Fences are then honest, but the
  title stalls in BlockOnTime `sub_00234BE0` waiting for a time whose
  commands were written (dev write ptr past PUT) yet never kicked off — the
  lock-step mirror hid that D3D expects something else to kick them. Next
  step there: work out sub_00234BE0's kick conditions.
  UPDATE 2026-10-13 (profile: main thread 65% in the kickoff spin
  sub_00234A10, executor busy only ~30% -- the hand-off, not raw raster
  speed, is the limit). Async-mode findings, toolkit patch:
  * The fast ack thread cleared the interrupt-status registers too; it beat
    the vblank tick to PCRTC_INTR and the title waited forever on its vblank
    event before the first frame. Now it acks only the PFB flush bit.
  * BlockOnTime (sub_00234BE0), when the wanted time is >= 0x2400 bytes
    ahead of the GPU, patches NO_OPERATION(5) (+ 0x40110) into the
    pushbuffer at that time's position, kicks off, and sleeps on event
    dev+0x1DBC. NV2A traps a non-zero NOP as a software method (xemu:
    TRAPPED_ADDR 0x400704 / DATA 0x400708, NSOURCE 0x400108 = 1,
    PGRAPH_INTR bit 20, PMC bit 12, pushbuffer stalls); D3D's DPC
    0x0023BC60 -> 0x0023BA20 -> 0x0023B7B0(data) sets the event. Now
    implemented (xbox_Nv2aSoftwareMethod + kernel_swm_tick/_done on the
    timer thread; registers cleared only after the DPC drain, since the ISR
    just queues the DPC) -- async mode only.
  * Still broken: async mode then sees NOP parameters like 40, 9,
    106955041 -- the executor appears to read pushbuffer the title has not
    finished writing (or misparses after a wrap) -- and each such NOP stalls
    the executor up to the 500 ms timeout: 2.5 s/batch, FPS ~0. Next: check
    the executor's scan bounds against D3D's write pointer in async mode
    (it must never read past PUT), then retest.
  UPDATE 2 (same day, later):
  * Walker: async mode now runs nv2a_pb_run (nv2a_pb_scan.c) -- GET -> PUT
    following JUMPs, method state (remaining data words) kept across kicks
    because D3D sets PUT mid-method; resync guard if GET leaves RAM. This
    removed the garbage NOPs (they were data words read as headers) and a
    crash.
  * NOP software methods are frequent and real: D3D schedules FLIPS with
    them (sub_0023B7B0: data & 0x1F = opcode, 1 = flip, data >> 5 = surface
    address | interval), ~55/s. Timer thread now wakes on an event, not its
    10 ms tick (was 100% executor wait).
  * Interrupt acknowledge emulation (both modes): nv2a_intr_thread
    (xbox_memory_layout.c) emulates write-1-to-clear of PCRTC_INTR via a
    marker bit (0x100) set by the vblank tick; PMC/PCRTC no longer held at
    zero by the slow loop. Default mode verified unchanged.
  * The PGRAPH handler acks by writing back what it read (undetectable), and
    D3D's DPC re-runs it while PMC bit 12 is set: one flip trap became
    thousands (flip seq hal+0x1CC reached 0x45604; flip targets 280k vblanks
    ahead; display reset in sub_0022E8C0 waits forever on the flip-pending
    flag). FIX IN PROGRESS: src/ssx3_overrides.c wraps sub_0023BA20 (_gen)
    and calls xbox_Nv2aSwmHandled() to clear the trap right after the
    handler; the timed pulse is now only a 50 ms backstop. Lift regenerated
    (`scripts/regen.sh lift` done); the FULL REBUILD was interrupted by a
    reboot -- run `scripts/sync-windows.sh` + `tools\build.bat` again, then
    test `live.ps1 start -Env @{RECOMP_NV2A_ACK_THREAD='1'; RECOMP_SWM_STATS='1'}`.
  * The visual glitches seen in async runs (whole font atlas drawn, stray
    polygons, flashing scores) were async-only: mis-executed commands and
    the repeated flips. Default mode does not show them.
  UPDATE 3: with the sub_0023BA20 wrapper built, async mode boots, runs
  menus and reaches the race load (SWM ~85/s, param 5 = BlockOnTime, no
  timeouts; fence advances steadily). It then hit the 43% race-load stall:
  resource entries 0 and 40 (0x00312AC8, 0x00312D48) stuck in state 8,
  streamer thread polling in sub_0020DB60 <- sub_0020DF80 <- sub_000CC0A0,
  I/O worker idle in NtWaitForSingleObjectEx (sub_0017BB30 <- sub_00212030).
  Ruled out: the 60 Hz service thread sub_00212030 (self-rearming XAPI timer
  sub_002120B0 -> sub_0021C39D) is alive -- counters 0x39E9D0/D4 advance
  ~60/s -- and its callback table 0x39E9AC is empty.
  More on the 43% stall (logs/20261013-162519_stderr.txt is one):
  * Resource state machine: sub_000E0060 moves a resource to 8 and starts
    streaming (sub_000DFF70); the loader loop sub_000CC0A0 polls each
    pending one in sub_000E0490, asking the world streamer (sub_0020B6A0 /
    sub_0020B750 / sub_0020B6D0) whether it is done. The streamer thread
    runs sub_0020D370 / sub_0020C290 / sub_0020CAB0 and sleeps
    (KeDelayExecutionThread) between passes -- it is polling, not stuck.
  * File reads continue during the stall, but they are audio: the repeated
    chunk starts "SCHl" (EA streamed-audio block). No world-data read is
    issued for the two stuck resources; the request is lost before I/O.
  * EA job workers (sub_00178CD0, request object e.g. 0x8062D440, wait
    handle at obj+0x88) dequeue the highest-priority job (sub_0017BDA0) and
    hand back any job whose priority (job+0x11) exceeds the worker's
    threshold (obj+0xB4) via sub_0017BE60, then sleep. Hypothesis: on the
    Xbox's single, strictly priority-scheduled CPU a higher-priority thread
    always picks such a job up at once; our guest lock hands the CPU over
    in arrival order and ignores thread priorities, so an interleaving the
    console never produces can leave a job queued with every worker asleep.
    RULED OUT (RECOMP_JOB_TRACE=1 ring, printed with every thread dump;
    wrappers on sub_0017BE60 insert / sub_0017BDA0 pop in ssx3_overrides.c):
    at a stall the queues are healthy -- pool 0x0039E6C4 -> worker queue
    0x8062D454 -> popped within the same ms by the file worker; users are the
    main thread (one job / ~2 s), the file worker and the audio thread. The
    world streamer does not go through these queues; the lost request is
    inside the streamer itself (sub_0020D370 / sub_0020C290 / sub_0020CAB0 on
    the loader thread). Next: trace the streamer's own request state
    (object +0xAC / +0xC0 / +0xC8 in sub_0020DB60) across a stall.
  UPDATE 4 -- ASYNC IS NOW THE DEFAULT (main.c). Last fixes:
  * Progressive GET: nv2a_pb_run writes DMA_GET at every command boundary
    (D3D decides from GET whether it may patch a wake-up NOP in ahead of the
    GPU).
  * Idle wake (xbox_Nv2aIdleWake): when the executor sits at PUT for 30 ms,
    deliver NO_OPERATION(5) (D3D's handler 0x0023B933 = KeSetEvent on
    dev+0x1DBC) -- the executor can still overtake the patch position in the
    check->patch window; idle at PUT means every kicked fence is reached.
    Gated on the GPU ISR being connected and non-blocking (posting it during
    boot hung the boot).
  * Reliability: async race_test 5/6 (the only failure the 43% loader
    stall, which default mode also has); visuals clean in menus, briefing
    and race. tools\race_test.ps1 now dumps threads on a FAIL before it
    kills the run; logs are kept in logs\ (tools\archive_logs.ps1).
  Real speed needs hardware rendering
  (vertex-program titles into the D3D11 pgraph translator) or batch-parallel
  rasterising.
  DONE (toolkit patch): batch-level parallel rasterising — vertex-program
  draw calls queue their set-up triangles and the pool draws the batch split
  by screen row (y % n), preserving per-pixel order; opt-out
  `RECOMP_RASTER_BATCH=0`. Race: 4-5 → 6-7 FPS on heavier scenes; executor
  ~10.5 ms/batch (was ~15). `RECOMP_EXEC_STATS=1` prints executor busy %,
  batches, KB and ms/batch every 5 s. `tools\to_race.ps1` drives a live
  session from boot into a race by draw count (start it after
  `tools\live.ps1 start`; press A at the briefing if it lands mid-transition).
  Executor phase split in a race (RECOMP_EXEC_STATS): raster ~65-70%, vertex
  programs ~15%, setup ~2%, other (parsing etc.) ~15%. Raising the toolkit's
  NV_RASTER_MAX_THREADS cap 16 → 32 (pool = cores-4 = 28 here) gave 7-8 FPS.
  Flush accounting (RECOMP_EXEC_STATS "[EXEC] flushes:" line): ~96% of
  raster area already goes through the parallel pool; in a race the title
  draws ~3.5-5 M bounding-box pixels per frame (11-17 screens; the race
  briefing ~25 M). What remains is the per-pixel cost of software register
  combiners + texturing + blending (a few hundred ns/pixel/thread). The
  toolkit's D3D11 pgraph translator is a 650-line stub; a real GPU path means
  porting xemu-style vertex-program and combiner shader generation (weeks).
- Race-briefing glitch (pink ground / torn shapes behind the briefing panel):
  ruled out so far — texture formats (all DXT1/DXT3, supported and cached),
  missing texture stages (0 batches), and the vertex programs:
  `RECOMP_VSH_BADSTATS=1` (toolkit patch) counts non-finite/absurd output
  positions per program hash and dumps the worst; every program is <=0.1%.
  The executor's large "degenerate/NaN" count is EA's stitched triangle
  strips (zero-area joins), not a bug.
  `tools\to_race.ps1 -StopAtBriefing` parks a live session on that screen.
  FIXED (2026-10-13, toolkit patch nv2a_pb_exec.c):
  * Torn streaks at the screen edges: near clipping. The executor clipped only
    at w = 0.001 and rebuilt clip space from c[58]/c[59], which are 0 for
    SSX 3 (its programs keep the viewport elsewhere), so every clipped
    triangle collapsed to a point and geometry between eye and near plane
    was drawn at huge coordinates. Now clipped in (x*w, y*w, z*w, w) — an
    affine image of clip space, no constants needed — against w > 0.001 and
    z >= 0. `RECOMP_CLIP_NEAR_Z=0` = old w-only behaviour.
  * Swizzled render targets (SURFACE_FORMAT type 2): SSX 3 draws six 128x128
    rider shadow maps per frame (124x124 clip) and samples them projected
    onto the snow; the executor wrote them row-major. Now written at Morton
    offsets (put_pixel / clear_surface), cleared over CLEAR_RECT (0x1D98/
    0x1D9C, swizzled surfaces only). Rider shadows now render.
    `RECOMP_SWIZZLED_RT=0` = old behaviour.
  * Texture address modes MIRROR (2) and BORDER (4, with
    SET_TEXTURE_BORDER_COLOR 0x1B24 when FORMAT bit 3 = border source colour).
  The pink ground itself is (very likely) real: the start gate is lit by pink
  neon (see the race-start cinematic), and the combiner (t0*t1 modulate-2x +
  fog lerp) and A8R8G8B8 decode were verified. Its stage-1 128x128 ARGB
  texture holds unrelated DXT bytes (never written during the briefing —
  watchpoint), origin unknown.
  OPEN: an intermittent hard-edged black wedge in races (~1-3% of the screen
  in 2 of 8 frames, worst with an airborne rider over a falling slope) = a
  rider's shadow map projected onto the terrain where the map is black with
  alpha 1. What is known (2026-10-13):
  * Per frame and rider: CLEAR (format 0x07070128, pitch layout, 128x128,
    colour 0 -> whole map transparent), then ~6-8 batches of the rider with
    shadow program BC25EF09 (oD0 = c[57].yzwx; c[57] = (1,.3,.3,.3) -> grey,
    alpha 1), then 2 batches (66 and 72 indices) with c[57] = (1,0,0,0) ->
    black, alpha 1, identical render state. The game uploads both c[57]
    values itself ([C57] trace). Those two meshes span the map (y ~0..127
    at x ~50..80) -- probably the board, long under a top-down light. Clip
    is (2,2) 124x124 so the border stays transparent.
  * Receiver pass (vsh CF752D6F): stage 0 = the map, PROJECT2D, clamp,
    colour r0 = t0, alpha r0.a = t0.a (no distance attenuation), final =
    fog.a * r0, alpha test GREATER 0x14, blend ZERO/SRC_COLOR. Texture
    coordinates are sane (q = 1). So black map texels darken to black.
  * Checked and matching xemu: vsh swizzle fields (C swizzle bits 2-9),
    disabled stage reads (0,0,0,1), r0.a init. Not the cause: stale c[57]
    (SET_FOG_PLANE 0x09D0 -> c[57] and SET_EYE_POSITION 0x0A50 -> c[56] are
    now mapped like xemu, no change), stride-0 attributes (tried, reverted),
    clear ring, border colour.
  Reliability with this build: race_test 5/6 (one timeout, log lost; the
  next 3 were OK).
  Next idea: compare against real footage/xemu for whether the board shadow
  is meant to be black, and check the receiver depth test / the board
  meshes' role. `RECOMP_SWIZZLED_RT=0` removes shadows and wedges together.
  Instruments: `RECOMP_RT_TRACE=1` ([RT] draws sampling a render target,
  [RTDRAW] draws into one, [RTBLACK] black samples with combiner/tex coords,
  [RTPIX], [RTWRITE] opaque-black writes into a render target, [RTCLR]
  clears of shadow maps, [RTC57] c[57] per surface, [RTSEQ] shadow-related
  batch sequence once a black sample is seen, [RTVTX] caster inputs,
  [RTEXT] caster screen extents), `RECOMP_C57_TRACE=1` (every value change
  of c[57], method vs upload), `RECOMP_SURF_TRACE=1` (distinct surface/format pairs),
  `RECOMP_PIXEL_PROBE=x,y[,delay_ms[,max]]` (every write to one pixel with
  src/dst/blend/combiner/texture state, [PROBE]/[PROBE-RC]),
  `live.ps1 dump -Mem "watch <va>"` (hardware write watchpoint on a guest
  dword, all threads, [WATCH] with native RIP + guest stack; ours,
  src/thread_dump.c). Unhandled-method report keys now include the
  subchannel (0xS_MMMM); SSX 3 binds image-blit/surfaces-2D objects on
  subch 2/3 but never blits.
- AUDIO (fixed 2026-10-13, toolkit patch src/apu): SSX 3 was silent. Its
  statically linked DirectSound drives the emulated APU directly; the voice
  processor's mix was written into `monitor.frame_buf` and then cleared by
  the XAudio2 output path, which only ever submitted the HLE software mixer
  + test tone. Now a VP frame (256 samples) is kept, the software mixer is
  added on top and exactly those samples are submitted (up to 12 queued).
  The frame thread's throttle also reset its schedule whenever a wake came
  more than one 5.3 ms frame late (Windows 15.6 ms timer tick), running the
  APU at ~85% of real time; it now resyncs only when >64 ms behind and asks
  for 1 ms timer resolution. `RECOMP_APU_LEVEL=1` prints the submitted peak
  and buffers/s once a second: 188/s = 48 kHz, peaks 12-23k in intro,
  menus and race. Not yet checked by ear. The waveOut fallback path still
  has the old behaviour (XAudio2 is used on Windows).
  Reliability after the audio fix: race_test 3/3; one live session stalled
  at the race-load 43% (the old intermittent stall) -- watch for it.
- Guest CPU lock (`RECOMP_GUEST_LOCK=1`, toolkit feature, now a main.c
  default): one guest thread runs guest code at a time, switching at kernel
  calls, like the Xbox's single CPU. Without it the race load intermittently
  hung at 43% (resource-table entries 0 and 40 stuck in state 8 = "streaming",
  set by `sub_000E0490` → streamer `sub_0020B700`; poll loop `sub_000CC0A0`)
  or crashed in the world streamer (0x0020D26A). `tools\race_test.ps1 -Runs N`
  repeats boot→briefing and reports OK / CRASH / FAIL per run.
  The lock alone deadlocked 2/2 runs at the race load: the loader thread
  polls its I/O slots in `sub_001795A0` (called from `sub_00175500`) with no
  kernel call, holding the guest CPU, while the worker that completes them
  waits for it. Fix (toolkit patch): the recompiler emits
  `RECOMP_LOOP_YIELD();` at every loop head (label with a back-edge
  predecessor, translator.py); it is a TLS countdown that every 4096
  iterations releases the guest CPU, SwitchToThread()s and re-takes it —
  preemption at loop granularity, like the console's quantum. Regenerating
  needs the new templates/runtime/recomp_types.h: delete
  src/recomp/gen/recomp_types.h first (the pipeline never overwrites it).
  Result: `race_test.ps1 -Runs 3` → 3/3 reached the race briefing (lock
  alone: 0/2). Gameplay 6-7 FPS with the yields (7-8 without; within run
  variance). RECOMP_DSP_ACK is no longer needed: the `sub_0025BD81` `_gen`
  wrapper registers the DirectSound DSP command word at run time via the
  toolkit's new `xbox_DspAckAdd` ("DSP ack: command word 0x83818810
  registered at run time").
- Known intermittent crash: race load sometimes faults in the loader worker
  (thread #4) in the world-streaming code (guest 0x0020D26A ← 0x0020D788 ←
  0x0020DD16 ← 0x0020E089 ← loader poll 0x000CC2C0) on a garbage pointer
  while processing worlds/BAM.big; an identical rerun passes. Suspect a data
  race that a single-core Xbox never exposes (guest threads run truly in
  parallel here).
- History of the race-load stall (fixed): entering the race (Snow Jam,

  Zoe) stalled in one of two ways, timing-dependent:
  (a) race loading screen stops at 43%: job worker thread #4 polls a resource
      package in `sub_000CC0A0` (start `sub_000E0370`, done? `sub_000DFE40`,
      per-resource state at 0x00312AC8 + id*16, 1/2 = loaded) while pumping
      `sub_000E0490` / `sub_0020AF20`;
  (b) black screen with a small yellow "Loading...", 0 FPS: the main thread
      is inside the script VM `sub_0016AB50` (fetch/dispatch: opcode <= 0x2A,
      jump table 0x0016B444; reached via sub_000CC8F0 → sub_000CABD0 →
      sub_000D4FB0 → sub_00166E30 → sub_00166C60 → sub_0016B4F0), spinning a
      "short script loop" — actually it runs from a NULL bytecode pointer.
  Root cause: with `RECOMP_GUARD_NULL=1` (guest page 0 inaccessible) the
  load faults at guest VA 0x8 in VM opcode 0x21 ("call native": table of 111
  script natives at 0x003319F0, entry = instruction byte 2). Native 0x69 =
  `sub_00159930` was the only one tools.disasm never detected; its ICALL
  failed ("[ICALL] Failed to resolve VA 0x00159930"), the fallback returned
  0, and `sub_00169A90` read the NULL result's type at +8. Now seeded
  (plus 0x000267B0, found by scanning .data/.rdata for pointers to
  undetected code after a ret/padding — only 2 real hits). An allocation-
  failure theory was ruled out: EA's allocator `sub_001A9BD0` is wrapped
  (`_gen`) to log `[ALLOC] FAILED`, and it never failed.
  Lesson: grep every run for `Failed to resolve` — each is a missed function.
- GAME STATE HOOK (src/game_state.c, on by default; RECOMP_GAME_STATE=0 off):
  a host thread samples the app object's state machine every 100 ms --
  [0x3A930C] -> app (0x00369550), +4 current state object (its vtable names
  the screen), +8 pending next state, +0x5C phase, state+0xA8 a sub-state --
  logs every change as `[STATE] t=.. vt=.. name sub=.. next=.. phase=..` and
  keeps `game_state.txt` (vt name ms seconds-in-state sub) current for the
  scripts (`tools\game_state.ps1`: `Get-GameState $root`). Top-level states:

  | vtable | name | screens |
  |---|---|---|
  | 0029AD1C | boot | before the intro |
  | 0029ACD8 | intro | EA / intro movies |
  | 00289618 | frontend | every menu, title screen to rules |
  | 0029AD48 | event_enter | short transition into an event |
  | 0029B0DC | loading | race loading screens |
  | 00299D7C | ingame | briefing, race, results, replay, pause/restart dialogs |

  game_state.txt has a 6th field, the GPU executor's total draw count
  (xbox_Nv2aDrawCount). tools\to_race.ps1 navigates from these fields only
  (no window title, no draw counts per screen): every frontend screen from
  the title to the rules takes A (Start on the title), and ingame after a
  loading state is the briefing. Its "seconds in state" restarts on every
  sub-state change, so the scripts time states by name themselves. The
  title starts an attract demo after ~40 s idle. RECOMP_FE_PROBE=1 logs
  changing fields of the frontend state object (no per-menu page field in
  its first 4 KB). Intro movies are skipped by default (main.c holds the
  movie mask 0x3083B8 at 0; RECOMP_SKIP_INTRO=0 plays them).
  Frontend screens (2026-10-14): a _gen wrapper on sub_00202460 (every
  screen state change; state 5 = active, bits 8-13 of screen+0x1C) records
  the active screen's vtable and id (+0xC) as game_state.txt fields 7/8
  ($gs.Screen "vt:id"; RECOMP_UI_TRACE=1 logs [UI] active screen). Ids: 0
  title, 1 hard-disk check, 2 main menu, 9 Select Character, 12 Setup
  Character, 19 Select Peak/Mode/Event (one screen, three pages), 20 My
  Rules, 24 Options (28 Game, 29 Sound, 30 HUD, 31 Controller, 32 Save/Load,
  35 Load game, 36 Credits), 13 Rider Details, 14 Equip Gear, 16 Music.
  to_race.ps1/race_test.ps1 -Rider/-Mode/-Event use them;
  menu_test.ps1 waits for id 2. The in-game HUD screen (vt 00293000)
  re-activates all the time and is left out of the screen field; field 9 is
  the recorded screen's live state (5 = up). In a race: briefing panel 00297E30 (after the course fly-over),
  pause menu 002981D8, Quit dialog 00297B60. tools\cycle_test.ps1 drives
  race -> pause -> Quit -> Yes by these.
  Inside ingame, state+0xA8 moves with the phase (5 at the briefing, 8 and
  0xC during a race); the remaining values (finish, results, replay) still
  have to be mapped -- to_race.ps1 prints the state for every new screen.
  Draw counts (the old screen detector) are ambiguous during replays: the
  restart dialog over a replay never shows its fixed count.
- Diagnostics added 2026-10-14: `RECOMP_BLOCK_TRACE=1` (wrapper on D3D
  BlockOnTime sub_00234BE0: time waited vs fence / current time / write ptr /
  PUT / GET), `RECOMP_KEVENT_WATCH=<va>` (every KeSetEvent/wait on one guest
  event), `RECOMP_SWM_STATS=1` also prints `[SWM-TICK]` deliveries per second,
  `RECOMP_RASTER_SKIP=1` (no pixel work: CPU-floor measurement),
  `RECOMP_FRAME_STATS=1` ([FRAMES] p50/p99 per second), tools/bench.ps1,
  tools/shots.ps1, tools/soak.ps1 (both removed 2026-10-16; race back to back; restart dialog
  handling still uses draw counts -> move to the state hook).
- WORLD STREAMER CORRUPTION (Phase 1, top priority): the 43% load stall and an
  intermittent crash are one bug. Crash: sub_0020CAB0(i) = "slot i of the
  streamer object (ecx, e.g. 0x812B4410) is in state 3", called with
  i = 0x171B2880 -> read of [this + i*24 + 0x3EC] faults. The streamer's
  chunk list holds garbage. Same object as the stalls (resources 0/40 stuck
  "streaming", sub_0020DB60 polling). Callers: sub_0020BB10 / BE60 / C120 /
  C290. RECOMP_STREAM_CHECK=1 (sub_0020CAB0 wrapper) logs a bad index with
  the guest call chain and the object and answers "not ready"; in the
  following 6-run round it never fired (no crashes; 2 load stalls, loader in
  sub_0020C290 <- sub_0020AF20). Timing-sensitive (more frequent with
  RECOMP_BLOCK_TRACE's logging), both GPU modes.
  Hypothesis being tested: thread priorities. RECOMP_PRIO_TRACE=1 shows XAPI
  thread start (0x00182719) calling KeSetBasePriorityThread(current, n): one
  thread +16 (time-critical), another 2 -> 1 -> 0 during the load. The guest
  lock ignores priority (arrival order); KeSetPriorityThread is a no-op and
  KeGetCurrentThread returns 0. On the Xbox the highest-priority ready thread
  always runs, so lower ones never see its updates half done.
  TRIED: RECOMP_PRIO_LOCK=1 (toolkit patch, opt-in) -- a thread taking the
  guest CPU defers to higher-priority waiters (KeSetBasePriorityThread on
  itself; ISRs/DPCs highest; 50 ms valve). Result 3/6 with 2 crashes in
  another streamer function (sub_0020D050+0x69, a write) and 1 stall --
  worse, so it stays off. IRQL-based exclusion is not it either: only three
  XAPI-area functions (0x189018, 0x189107, 0x21D1AF) raise IRQL.
  Data path: sub_0020C290 takes the slot index from the streamer's request
  list -- entries at [mgr+8] + k, field +0x18 -- so the garbage is written
  into that list. Streamer code (sub_0020D7C0, reached by a tail jump from
  sub_0020AF20) runs on the loader thread; sub_0020CAB0 is also reached from
  game code (0x0005B7B1, 0x000D2690, ...). Next: a watchpoint on the
  request-list entry that goes bad (log the list each pass, watch the entry
  whose index field changes to an impossible value).
  UPDATE 2026-10-14: the two-thread hypothesis is RULED OUT. Two guest
  threads do drive the streamer -- the main thread's update (sub_000CBB10 ->
  pump sub_0020AF20, sub_0020D4C0) and the loader (sub_000CC0A0 -> pump, the
  resource poller sub_000E0490 posting requests via sub_0020B700/B7B0, and
  the world reload sub_0020B3F0, which frees the world file holding the
  request list and re-points it only after the new file is read) -- but with
  every entry wrapped (RECOMP_STREAM_CHECK=1 logs [STREAM-OVERLAP] when a
  second thread enters; RECOMP_STREAM_SERIAL=1 would make it wait) 16 runs
  logged no overlap at all (the poller was wrapped for the last 16 of them;
  the streamer stalls among those runs had no overlap either). Every thread dump now prints
  [STREAMDUMP]: the request list (entry name, state 0 idle / 1 requested /
  2 loading / 3 unloading / 4 off / 5 ready / 6 in use, slot and the slot's
  record), the resource state table 0x312AC8, and D3D's fence wait if the
  main thread is in one (time, ms waited, fence/cur/wr/PUT/GET). Next: catch
  a stall with it.
  NOT EVERY "43%" IS THIS BUG: check whether draws continue after the dump.
  If the loading screen still renders (draw counter keeps rising) it is the
  streamer stall (loader thread polling sub_000E0490); if draws stop and the
  main thread sits in BlockOnTime (sub_00234BE0 <- sub_00234F90) it is the
  async GPU freeze below.
  STREAMER STALL, what the dump shows (2 stalls, 2026-10-14): request entries
  stuck in state 2 (loading), their slots in slot state 1 (queued, no buffer)
  or 2 (reading). sub_0020D370 starts at most one read at a time, through the
  reader at streamer+0x300 (busy byte +0x90, EA stream handle +0x88), and
  marks the slot 2 without checking that the read started; sub_0020DF80
  pulls completed chunks (sub_00177F80) until the stream reports done
  (sub_00178070: manager busy [M+0x48] == 0 and nothing left), then slot 3
  and reader idle. So the world stream's data never arrives. EA's 'STRM'
  stream manager M ([handle] = M, lock at M+4 via sub_0017B210/B220): opening
  (sub_00177E10) queues the request, then kicks the I/O (sub_00177990) only
  if [M+0x48] was 0 -- if busy it relies on the running I/O to take the next
  request. [M+0x48] is cleared under the lock in sub_00177990 (no request
  left) and, when it is 2, in sub_00178800 (from sub_001789B0 and
  sub_0018FE30) -- next: check whether those hold the lock and whether a
  request queued in between is ever kicked (suspected lost wake-up).
  UPDATE (later 2026-10-14) -- IT IS A STREAM PARSE FAILURE. The reader
  does get its data (EA STRM manager, ring full: avail 0x48000); it retries
  forever because sub_0020D6B0 (place an asset) returns -1: the first asset
  header of the first world stream (entry #10 ASKY, slot 36, BAM.xsb +
  0x01390000) is garbage (type 156, bank 222, size 16 MB), so its bank pool
  ([pool+0x3C+bank*4], created per request entry i by sub_0020C9B0, destroyed
  by sub_0020D2A0) is NULL. The real first block there (CBXS, 0x8000 bytes,
  RefPack 10 FB) decompresses to 0x14000 bytes with a valid header (type 24,
  size 0x334, bank 10); the stalled reader got 0xA16E bytes from sub_0020DA80.
  Both caught stalls are byte-identical (same ring pointers, same 0xA16E). The
  pool itself is fine (wrappers: sub_0020C6A0 [POOL] exhausted -- only ever
  seen for the NULL bank pool, RECOMP_STREAM_POOL_EXTRA=N knob). bam.big
  holds bam.xsb at +0x8680. Next: [CHUNK] log (sub_0020DA80 wrapper,
  RECOMP_STREAM_CHECK=1: src bytes/len/result) to see which bytes the
  decompressor was given in a stall.
  FOUND WHERE (2026-10-14, [CHUNK] log, good vs stalled run): the reader's
  chunks 1-5 are identical in both (xsb +0x1390000..+0x13B0000; block 5 at
  +0x13B0000 is the 'CEND' block that ends the ASKY request). Next, the good
  run gets the next request's data (xsb +0x0, +0x8000: bank 0 = TRAN); the
  stalled run gets xsb +0x13B8000, +0x13C0000 -- the blocks that merely
  follow on disk. EA's STRM manager read the next request at the OLD file
  position (a seek was skipped). EA's file layer sub_0017A5A0 (file table
  [0x39E884], 20-byte entries: +0 handle, +4 cached position, +8 size, +0xC
  archive base) seeks only when it thinks the position differs; for a file
  inside an archive (base != 0) it compares base - cached with the wanted
  position, so for position 0 the seek is skipped whenever cached == base --
  (superseded: see ROOT CAUSE AND FIX below) [EAREAD] (sub_0017A5A0
  wrapper: fd, pos, len, cached, base, result) and [STRM] (sub_00177990
  request start: request, pos, tag, open file) are built and log with
  RECOMP_STREAM_CHECK=1; run `tools\race_par.ps1 -Instances 8 -Runs 2 -Env
  @{RECOMP_STREAM_CHECK='1'}` until a stall (~1 in 10) and compare its
  EAREAD/STRM sequence around the ASKY CEND with a good run's.
  ROOT CAUSE AND FIX (2026-10-14, race_par 8x2 with [EAREAD]/[STRM]): in
  the good run the STRM manager sees the 'CEND' block, marks the request done
  and starts the next ([STRM] start ... state 3, next read at the archive
  base); in the stall it never does and reads on (0x13C0680 .. 0x13D0680)
  until the ring is full. The end-tag scan sub_00177760 (from the read
  completion sub_001781B0, itself reached from both the I/O completion and
  the reader's chunk release sub_00178540 -> sub_00178280 -- two threads)
  reads and rewrites block headers and the scan pointer partly OUTSIDE the
  manager's lock. On the Xbox an uncontended RtlEnter/LeaveCriticalSection
  never switches threads; the toolkit let go of the guest CPU on every
  kernel call, so each lock call (sub_0017B210/B220 -> ordinals 277/294)
  was a preemption point and the two scans interleaved. FIX (toolkit,
  kernel_thunk_dispatch, default on, RECOMP_CS_KEEP_CPU=0 off): Leave never
  lets go; Enter keeps the guest CPU when the lock is free (try-enter held
  across the bridge's own Enter) and lets go only to wait. Result: race_par
  8x3 -> 0 load stalls in 24 runs (before: 2 in 16). The [EAREAD] wrapper's
  pos/len columns are actually len/garbage -- the args are (fd, buf, pos,
  len); the cached-position columns are right.
- USB ENUMERATION UNDER LOAD (open, 2026-10-14): with 8 instances in
  parallel ~25% of runs stay on the title screen ("[title: no input]",
  1 draws): enumeration stops after ~11 OHCI interrupts (one with WDH|SF
  0x06) instead of 20; the pad never appears, so RECOMP_PAD_LIVE presses go
  nowhere. Not the OHCI 200-deliveries give-up; the OHCI loop is
  level-triggered. Suspect a transfer timeout in the XAPI USB stack when
  the host is busy. With the watchdog these runs show as [no pad] or
  [freeze]: the title screen sits at 1-2 draws with FPS 0, so the frontend
  itself stops (likely waiting on the device), not just the input. Traced
  logs (RECOMP_USB_TRACE=1): race_par batch of 13:0x on 2026-10-14, stuck
  runs i2 #2, i3 #2, i5 #1, i6 #2 in work\i<k>\logs\. Next: compare
  their USB traces with a good run's.
- TITLE-SCREEN MOVIE FREEZE (open, 2026-10-14; the "[no pad]"/"[freeze]"
  runs, ~1 in 6 at 8 parallel instances, the only failure class left in
  race_par 8x3 batches): the frontend plays a background movie through EA's
  STRM manager (M 0x814F3EC0, handle 0x814F42BC). The main thread waits in
  sub_00177F80 (next chunk) <- sub_00173B20 <- ... <- sub_001706A0 <-
  sub_00080730; its walk once ran off the ring (main-thread crash at
  0x00173B36). Ring trace ([STRMEV]/[STRMRING] in every thread dump,
  wrappers on sub_00177760 scan, sub_00177F80 fetch, sub_00178540 release,
  sub_00178800 flush, sub_00178280 schedule, sub_001781B0 read completion)
  of one freeze: rd = scan at an unscanned 'MADk' block of 0xA41C bytes that
  is 0x184 bytes short of the write pointer, busy = 1, ring has room -- the
  read that should complete it was issued (or not) and never finished /
  never rescheduled. Scheduler sub_00178280: rd > wr and gap < read size ->
  busy 2 (wait for the consumer); no room before the ring end -> copy the
  unscanned tail to the ring start and wrap; else async read via
  sub_00179E40 with completion sub_001781B0. RECOMP_PRIO_LOCK=1 makes it
  worse (16/24 vs 20/24).
  CAUSE FOUND (2026-10-14): not a stream bug but guest-CPU starvation. In
  the freezes the file worker has finished its read (kernel_thunk_dispatch
  after the ReadFile bridge) and waits in guest_cpu_acquire, while the main
  thread holds the guest CPU spinning in the movie player (sub_00173B20 ->
  sub_0017B3B0 / sub_00177F80, no blocking kernel call). Windows critical
  sections are not fair: a loop yield let go and took the CPU straight back
  before the woken worker ran -- reads took 0.5-1.2 s, then one never
  finished. Toolkit fixes (RECOMP_FAIR_YIELD=0 off): a loop yield with a
  waiter present waits (<= 5 ms) until someone else has taken the CPU; and
  while a guest thread waits, the uncontended-lock fast path is off and every
  kernel call hands over the same way. race_par 8x4: 29/32 before and after
  the second step -- failures remain (frontend stall with the worker still
  waiting for the CPU while the main thread is in a GPU wait, i.e. a third
  thread held it). [GUESTCPU] in every thread dump now names the holder,
  how long it has held it and how many wait. Thread dumps end with
  [THREADS-END]; race_test waits for it (<= 90 s) before killing a run.
  CORRECTION (later): with the dump reordered (snapshot first, traces
  after -- printing first let threads move on and block on the stderr lock)
  the remaining title freezes show NO guest-CPU holder and no waiter: the
  main thread is in D3D BlockOnTime (fence time not reached, [BLOCK-STUCK],
  GET 0x9F0 bytes behind PUT) and the GPU executor is busy rasterising one
  draw (nv2a_pb_run -> draw_primitive -> raster_batch_program -> rc_texel)
  that does not finish -- the title screen's "1 draws" frame. Next: how big
  that draw is (RECOMP_EXEC_STATS=1).
- ASYNC GPU FREEZE (map select / rules / loading 31-33%): main thread loops in
  D3D BlockOnTime: the idle wake does reach it (KeSetEvent on 0x0024CE9C,
  ~10/s) but the time it waits for never completes; 0x5C bytes are written
  past PUT and never kicked. RECOMP_BLOCK_TRACE (now quiet: logs only a
  time waited for repeatedly, with D3D's fence record for it -- time, pushbuffer
  position, the patched words) has not caught one yet. BlockOnTime's far path
  (0x00234D21) waits on dev+0x1DBC with no timeout and returns after ONE wake
  without re-checking the time; its near path spins on the fence. The GPU
  position it uses is DMA_GET ([[dev+0x1C10]+0x44]); positions at or below
  D3D's write pointer count as the current lap (+ ring size [dev+0x40]).
  CAUGHT (2026-10-14, [STREAMDUMP]): main thread waited 417 s for time
  0x1ECF with the fence already at 0x1ED3 and the executor idle -- the wake
  was simply lost. D3D's NOP handler (0x0023B933) sets the event
  unconditionally, so the loss is upstream (idle wake / ISR / DPC). WORKAROUND
  (default on, RECOMP_BLOCK_RESCUE=0 off): a watchdog thread in
  ssx3_overrides.c sets the event from the host when a wait is >200 ms old
  and its time is reached, logging [BLOCK-RESCUE] with the wake chain's
  counters (xbox_Nv2aSwmDiag); a wait whose time is not reached after 2 s is
  logged as [BLOCK-STUCK]. 10 runs: 2 rescues, both runs reached the race; the
  two losses differed (one trap raised 141 ms earlier and never completed,
  pending 2; one with a flip in the trap registers, pending 0).
- Live driving: `tools\live.ps1 start|snap|press -Buttons a|key -Key 0x0D|dump|stop`
  (start sets RECOMP_NO_HOST_PAD=1 and RECOMP_MUTE=1: scripted runs ignore the
  machine's real controllers and are silent, so the user can use the PC while
  tests run; play.bat is unaffected. Audio measurements need -Env @{RECOMP_MUTE='0'})
  (`key` posts WM_KEYDOWN/UP to the window, exercising the real keyboard
  path; verified 2026-10-13: Enter presses reach the menus with
  `RECOMP_KEYBOARD=1`. `RECOMP_INPUT_DIAG=1` logs what the pad layer sees.)
  (RECOMP_PAD_LIVE + a trigger-file thread dump; `RECOMP_DUMP_MEM=va:len`
  adds guest memory to each dump). Menu path from boot: Start presses until
  the main menu (~40 s), then A ×7 (title/main, Single Event, Zoe, Continue,
  Peak 1, Snow Jam, My Rules). Window-title draw counts identify screens:
  main 339, character 984, setup 382, peak 275, event 367/413, rules 329,
  race loading 309, black race-load 22.
- Game flow: app runner `sub_001ADAC0` → state machine `sub_001AD880`, app
  object 0x00369550 = `[0x3A930C]`, phase `+0x5C`, current/next state `+4`/`+8`,
  updates `+0x20`; renderer object `[0x369528]` (vtable 0x002F7E50: `+0x44`
  "draw list empty?" `sub_001EA6A0`, `+0x50` flush/present `sub_001F3020`).
- Game logic is clocked by input: app `vtbl[5]` = `sub_000E1A80` consumes one
  sample from a 30-entry ring (input manager `[0x36952C]`, read `+0x2EE0`,
  write `+0x2EE4`) filled at ~60 Hz on the timer thread by `sub_000E1AE0`.
- Other `[ABI]` reports in the abi build are expected (SEH_prolog/epilog,
  `_chkstk`, CRT internals) except any new one in game code.
- USB/controller (works; `RECOMP_USB=1` default): main.c now calls
  `xbox_OhciInit()` and routes OHCI register faults (0xFED00000/0xFED08000)
  in the VEH — the template did neither. XAPI's USB class-driver table is at
  0x00279794 (entries: memory unit 0x08, XID 0x58, HID 0x03, hub 0x09; each
  `{0x82, class, .., Init, AddDevice, Remove}`). Its method pointers live in
  the XPP *code* section, which tools.disasm does not scan for pointers, so
  Init/AddDevice/Remove were missing; they are seeded in
  `analysis/icall_seeds.json` (8 entries). Enumeration stage byte is
  0x003A1B1B (stage 6 = config descriptor parsed → class AddDevice).
  Pad probes: `RECOMP_PAD_PRESS=0x10` (Start pulses), `RECOMP_PAD_SCRIPT`,
  `RECOMP_PAD_LIVE`, `RECOMP_KEYBOARD`; traces `RECOMP_USB_TRACE`,
  `RECOMP_USB_STATS`, `RECOMP_DPC_TRACE=<n>`, `RECOMP_TIMER_TRACE` (ours).
- Intro movie data/movies/Intro_DJ.mad is 155 MB (minutes long); skip with Start.
  The EA logo and THX movies cannot be skipped (~11 s). 0x3083B8 is the
  intro's mask of movies still to play (bit 0 eabig, 1 thx, 2 intro_dj;
  initial 7; table of names at 0x3083BC; sub_00080730 plays them).
  `RECOMP_POKE=0x3083B8:0` plays none (a missing .mad file hangs instead).
  `scripts/race_loop.sh` uses it: boot -> race in ~15 s on Linux/Vulkan.
- PLAYTEST FIXES (2026-10-14, user playtest):
  * Options menu crash (Y on the main menu): the Options screen's message
    handler sub_00091640 jumps (jne) into the shared epilogue of the next
    function, 0x0009186C `pop esi; ret 8`. tools.recomp emitted that
    unresolved target as a stub `g_esp += 12` -- esi never restored (and esp
    4 off), so the list code (sub_002055C0) carried on with the screen as
    `this` and called through its NULL +0xA0. Toolkit fix (translator.py
    `_stub_epilogue`): a stub whose target is `pop r32`* + `ret N` restores
    the popped registers. Only two stubs had that shape (0x0009186C and
    0x0008773F, which skipped three pops). Found with: the null-ICALL report
    (src/recomp_manual.c + print_guest_regs_and_stack in main.c: registers,
    symbolised native chain, esi object; RECOMP_ICALL_NULL_MEM=va:len,
    *va:len, @espoff:len for more memory) and RECOMP_UI_TRACE=1 (_gen
    wrappers on the list select sub_00205D40 and the screen restore-focus
    sub_0008C540). Lesson: check recomp_stubs_unresolved.c for stubs whose
    target is the middle of a real function.
    GENERALISED (2026-10-15, Equip Gear hang/crash): of 87 unresolved
    stubs most were real code -- jump targets inside a function that
    tools.disasm split in two, or code it left in no function at all. A
    returning stub skips that code silently. Toolkit translator
    `entry_stub`: a target inside a known function is emitted as that
    function's translation entered with `goto loc_<target>` (registers are
    globals, ebp = g_seh_ebp as for any tail jump) -- 61 stubs. Code in no
    function is seeded in analysis/icall_seeds.json (0x000877B1 was the
    Equip Gear screen's setup, after sub_00087706's early end; plus four
    CRT/DSOUND/thunk pieces). Seeding targets *inside* functions instead
    splits them and makes it worse (more stubs; loops become calls). The 19
    stubs left are mid-instruction or outside the image. RECOMP_UI_TRACE=2
    logs every screen state change with the screen's children.
    `tools\menu_test.ps1 -Steps "y,wait,down,a"` drives the menus with a
    screenshot per step (Start on the title is sometimes missed -- rerun).
  * Audio crackle: the output queue kept whatever depth it started with --
    near full in some runs (a 5.3 ms buffer dropped now and then = click),
    near empty in others (underruns). Now: wait for room instead of
    dropping, refill a (nearly) dry queue with silence to 8 buffers (43 ms),
    and the APU frame thread runs at THREAD_PRIORITY_HIGHEST. A 40 s race:
    0 underruns / drops / refills, queue 6-9. RECOMP_APU_LEVEL=1 prints
    underruns, drops, waits, refills and min queue depth per second.
  * Vulkan cube maps (stage mode 3) were sampled as plain 2D; now the six
    faces are decoded stacked (Nv2aTexInfo.cube, nv2a_exec_tex_decode) and
    the shader picks the face. SSX 3's races and character screen use no
    cube maps (RECOMP_VKR_STAGES=1 lists each stage program once), so this
    did not change the character look; character lighting matches the
    software renderer (both use the CPU vertex programs).
  * Display gamma ramp: D3D uploads one (sub_00239B76, 256 R/G/B bytes
    through the VGA palette port 0x6813C8/9, which our MMIO drops). Now a
    _gen wrapper hands it to the toolkit's presenter (fb_gamma.c,
    xbox_FramebufferSetGammaRamp; [GAMMA] logs it; RECOMP_GAMMA=0 off).
    SSX 3's ramp is a mild contrast S-curve (64->52, 128->125, 192->198):
    darker shadows, closer to console captures of the character screen.
    The user's console captures (TV video) still show riders with stronger
    directional shading and a more saturated picture.
  * Character lighting (2026-10-15, compared with xemu 0.8.136 on the Select
    Character screen, Griff): riders were washed out because the vertex
    colour outputs oD0/oD1 were interpolated unclamped -- SSX 3's lighting
    programs push them well above 1. The NV2A (and xemu) clamp them to 0..1
    per vertex. Now clamped in nv2a_vsh_interp.c (CPU/software path),
    xbox.vert and xbox_vsh.vert. The race (pink-lit start gate included) matches xemu too.
- FAST TEST RUNS (2026-10-15): `RECOMP_AUTONAV=race[,rider=N][,mode=N][,event=N][,brief]`
  (src/autonav.c) drives the frontend from inside the process by the active
  screen id/state, pressing through the toolkit's new `xbox_PadInject`:
  boot -> briefing ~27 s (was ~40 s with to_race.ps1's pad-file presses);
  re-arms after each race. race_test/cycle_test use it by default
  (-NoAutonav = old way; to_race.ps1 -Autonav waits for its
  "[AUTONAV] t=<ms> at the briefing" line). The rest is the game's own (boot
  4.5 s, title logo 5 s, "Checking hard disk" 5 s, fly-over). Process
  snapshots are not an option: guest call stacks are native stacks (return
  addresses change with every build), plus Vulkan/XAudio2/kernel handles.
  Saves: `RECOMP_SAVE_DIR=<dir>` (toolkit) moves UDATA/TDATA out of game\;
  live.ps1 gives every scripted run a fresh copy of work\saves\<-Save>
  (default "unlocked": every peak unlocked by cheat; "none", "user").
- LINES AND POINTS (2026-10-15): SSX 3 draws trails and rail lights as line
  strips and snow spray / fireworks as point sprites; the Vulkan backend now
  draws both (topology in the pipeline key; xemu's point rules). The
  briefing screens after a load: 00297E30 race, 00297F68 one-on-one,
  00297E98 freestyle. `RECOMP_VKR_PRIMLOG=1` logs each distinct state a
  line/point batch is drawn with; any primitive type the backend cannot draw
  is logged once as `[VKR] primitive N`.
- METRO-CITY 30 FPS (2026-10-15): in Metro-City's glowing tunnel section
  (~35-65 s into the race) the game renders every other 60 Hz tick: exactly
  30 FPS. xemu does the same at the same point (tools\xemu_fps.ps1 reads the
  game's renderer frame counter [0x369528]+0x771C through xemu's monitor) --
  game behaviour, not ours. Mechanism: app loop sub_001AD880 renders after an
  update only if the state's render method (ingame vtable +0x18 =
  sub_000D3880) gets past renderer +0x44 = sub_001EA6A0 (current draw list
  empty); the main thread idles in its frame wait (sub_001ADF40) ~85% of the
  time meanwhile. Not the vblank phase (RECOMP_VBLANK_HZ=59/61: same).
  Tools from the hunt: RECOMP_WAIT_STATS=1 ([WAITS]: kernel-call time per
  thread/ordinal/guest call site), RECOMP_CPU_STATS=1 ([CPU]: host CPU per
  thread, no suspension), `live.ps1 dump -Mem "profile main"` (main thread
  only, 16 frames), RECOMP_GUESTCPU_STATS share per holder, tools\burst.ps1
  -Taps/-TapKeys.
- 30 FPS MODE REMOVED (2026-10-16): the frame-every-other-tick above is the
  app object's 30 Hz flag, app+0x38 (0x00369588): sub_001AD880 renders only
  after >= 2 updates while it is set. The in-game update sub_000CC8F0 sets it
  every update -- always with more than one player (split screen), else when
  sub_00148CB0(6) (slot 6 of the 0xF0-byte records at 0x3992D0, an object's
  vtbl+0x64 float; meaning not identified) is above 0.9 (0x282274). A _gen
  wrapper (ssx3_overrides.c) clears it after each update; RECOMP_FPS30=1
  keeps the game's choice; [FPS30] logs each change of what the game wants.
  Metro-City: the game asks for 30 Hz twice (~20 s and ~37 s stretches), the
  race stays at 59.8-60 FPS; tunnel shots at 60 vs 30 look alike (shots\
  metro_fps60/30 on the Windows copy). tools\fps30_scan.ps1 runs every
  Race/Freestyle event on Peaks 1-3 and logs where it asks; interrupted after
  2 events (Snow Jam: never; Metro-City: twice) -- rerun for the rest. Split
  screen at 60 not tested yet.
- HD TEXTURES (2026-10-19): hd-textures/ = Bl4ckH4nd's SSX 3 PS2 pack
  (PCSX2 replacement pack, BC7 DDS, LFS; https://gbatemp.net/threads/my-ps2-packs.621422/,
  included with EA's approval). PCSX2 names are PS2 GS hashes, so the pack
  is matched by content: RECOMP_TEX_DUMP=<dir> (src/vk/vkr_hdtex.c) writes
  each decoded texture once as <pixel-hash>_<w>x<h>_f<fmt>.tga;
  tools/hdtex_match.py (32x32 RGBA signatures, same aspect, clear winner,
  twin files allowed, flat/atlas dumps rejected: match <= 0.05 and <= half
  the dump's flat-colour distance) writes hd-textures/ssx3_map.txt; "a2" =
  the file's alpha is on the PS2 0..128 scale (doubled in xbox.frag via
  tex_scale.z = -1). Runtime: bind_textures (vkr_backend.c) hashes a newly
  decoded texture's pixels, hdtex_get loads the DDS as a BC7 image with all
  mips (vkr_tex_create_bc7; staging full -> retried next frame). Samplers
  now use mips. 507 textures mapped from Peaks 1-2 (tools/fps30_scan.ps1
  -Env @{RECOMP_TEX_DUMP=...}); Peak 3 not dumped yet. Rider/hair textures
  have no match.
- MSAA (2026-10-19): RECOMP_MSAA (default 4, launcher dropdown): color and
  depth multisampled, resolved once per flip into c->resolved (vkr.c),
  which feeds window, guest copy and CRT. Alpha test GREATER/GEQUAL becomes
  partial coverage (gl_SampleMask from (a - ref) / fwidth) for smooth
  cut-out edges; main() sets gl_SampleMask = -1 first (a shader that writes
  it leaves it undefined elsewhere: MSAA off showed pure noise). 5120x1440:
  60 FPS, GPU 8.5-14 ms either way (scene variance dominates).
- PARALLEL BOOT COLLISION (fixed 2026-10-15): with 8 instances, ~1 boot in 6
  opened \Device\Harddisk0\Partition5 with 0xC0000001 and the title quit to
  the dashboard ("exit requested", state ? and 0 draws -> reported as
  [no pad]): every instance shared the partition images in
  %LOCALAPPDATA%\xboxrecomp. RECOMP_SYSTEM_DIR (main.c) moves them; live.ps1
  gives each run directory its own (<dir>\system). Some older "[no pad]"
  failures under parallel load were probably this.
- INTERNAL RESOLUTION (2026-10-15): RECOMP_RES (main.c default 1080 =
  1440x1080; live.ps1 sets 1 for scripted runs; a value >= 240 is a frame
  height, else a factor). src/vk/vkr.c renders into sw x sh = guest size x
  scale (positions stay guest pixels: the shaders divide by the guest size),
  scales scissor/clears/point size/line width, and at a flip reads back the
  full image (published to the window via xbox_FramebufferPublishImage) plus
  a linear-blit 640x480 copy for guest memory. Software-drawn surfaces
  (shadow maps, offscreen) stay native. SSX 3 is 4:3 only (no widescreen
  option; the 4/3 float at 0x284E80 is not the camera aspect).
- WIDESCREEN (2026-10-16, branch `widescreen`): RECOMP_WIDESCREEN=auto (main.c
  default there; live.ps1 sets 0) -- main.c turns it into RECOMP_ASPECT
  (auto = primary monitor, e.g. 32:9 on 5120x1440; 1 = 16:9; W:H) and
  RECOMP_RES defaults to the monitor's height. Everything below scales by
  factor = aspect / (4/3) (16:9 = 4/3, 32:9 = 8/3). The main camera's projection is D3DX
  PerspectiveFovLH 0x0024EA13, called only by the renderer's camera setup
  sub_001EAB40 with aspect 640/480; a _gen wrapper (ssx3_overrides.c)
  multiplies it by 4/3 while the app state is ingame (ssx3_WidescreenNow), so
  the game renders (and culls) a 16:9 view into 640x480. src/vk: target x
  4/3 wider (scale_x), 2D vertices (w == 1) narrowed back to 4:3 except on
  the screen edges (full-screen passes); outside ingame everything is
  pillarboxed (vkr_set_pillarbox: all vertices narrowed, scissor mapped to
  the middle, bars cleared black at the flip). With auto the frame follows
  the window (2026-10-16): the toolkit window reports its client size
  (xbox_FramebufferClientSize), vkr_backend.c follow_window checks it at
  flips and, once a new shape has held for 250 ms, sets ssx3_SetAspect +
  vkr_set_aspect and rebuilds the target between frames ([VKR] window WxH:
  frame aspect A). Narrower than 4:3 stays 4:3. Probe:
  work\probe\resize.ps1. Finding it: RECOMP_DUMP_CONSTS=1
  (each program's constants), RECOMP_VSH_DUMP=N (disassemble N programs).
- DEBUG QUICK START MENU (2026-10-16): RECOMP_DEBUG_MENU=1 -- the retail
  "quick start" menu (Xbox PAL code 0009C7C2 = 01: the title handler
  sub_0009C7A0 stores 1 instead of 0 at obj+0x18, done by a _gen wrapper).
  Press Start on the title. App state vtable 0028825C ("DebugMenu"), menu
  object vtable 0028810C, input in sub_0006FED0: EA action map at
  [0x369534]+0xA8 (4 controller slots of {state, map}), action 0x65 =
  confirm, 0x3D = up/down (-1 none, 0 up, 1 down). It reads controller
  slot 0 only -- our pad used to sit on root port 1 = controller port 3;
  the toolkit OHCI now has 4 root ports and plugs pad 1 on root port 3 =
  controller port 1 (then 4, 1, 2), like the console. RECOMP_ACTION_TRACE=1
  logs each action id queried per second with its hits ([ACTION]).
- LODGE FLOW (2026-10-16): a station gate wakes a station object (vtable
  method sub_000D5A60: +0x10 state, +0x14 kind 0 = lodge / 1 = transport,
  +0x18 station id, e.g. 0x17 = Green Station lodge). State 0 opens in-game
  screen 0x1F "Would you like to enter the lodge?" through the in-game
  screen opener sub_000C4C20(type). On Yes: state 2 -> app state 0029B0F8 ->
  "go to frontend" sub_000E1B90 (real frontend sub_0007E4D0 only if
  app+0x60 is set, else the DebugMenu) -> the frontend's screen switch
  sub_0007FB60 opens the lodge, screen 39 (vtable 002938C0, built by
  sub_000B4350). Verified in normal play by the user: Equip/Buy Gear, Buy
  Attributes, Rider Details, Music, Save all work. From a debug-menu start
  (Track = Green Base Station; the rider usually bounces off a rail into the
  gate, sometimes grinds it and overshoots) the prompt appears; its Yes
  handler sub_000B58C0 sets the frontend start screen [0x36843C] = 0x27 (read
  by the frontend's first-screen setup sub_00080A70: 0 = title, 0x27 =
  lodge) and ends the event. app+0x60 is 0 after a debug start, so the
  DebugMenu came back instead; a _gen wrapper on sub_000E1B90 now sets
  app+0x60 when the debug menu is on and a lodge is pending. Debug start ->
  lodge in ~35 s (Start on the title, Right x3 = Green Base Station, Start,
  A at the prompt); Equip Gear, the sub-screens, Save game (slot list, name
  keyboard) and Return to Game (Save progress? No -> back at the gate, the
  prompt again) checked with work\probe\dbgmenu.ps1. RECOMP_STATION_TRACE=1 logs station objects
  ([STATION]) and every in-game screen opened with its callers
  ([INGAMESCREEN]). Debug menu World Circuit tracks: Happiness, Ruthless,
  The Throne, Green/Blue Base, Yellow/Red Mid, Black Top Station; Game Path
  World Circuit / Quick Play / Multiplayer.
- LOG WRITES STALLED FRAMES (2026-10-16, user: "extreme FPS drops when
  grabbing items"): stderr was an unbuffered file, so every fprintf was a
  WriteFile on the printing thread, and such writes now and then block for
  100-300 ms. In a 40 s freestyle bench every run had one 250-300 ms freeze
  in game with the GPU executor inside fprintf (its progress line "[GPU]
  clear #N", a pipeline line, ...). Fix (main.c log_async_start, default on,
  RECOMP_LOG_ASYNC=0 off): when stderr is a file it writes into a 4 MB pipe
  and a pump thread moves it to the file; log_drain (crash handler, and the
  toolkit's exits through xbox_before_exit / xbox_BeforeExit before
  ExitProcess/TerminateProcess) waits for the pump. Not PeekNamedPipe on the
  read end: it waits behind the pump's pending ReadFile forever. After: no
  in-game frame over 57 ms, none in fprintf (2+2 runs A/B). The pickup drop
  itself was not reproduced by a script (the rider must be steered into
  items); `tools\play.bat diag` turns on RECOMP_FRAME_STATS, RECOMP_VKR_STATS,
  RECOMP_GUESTCPU_STATS, RECOMP_APU_LEVEL and RECOMP_SLOW_STACKS=50: in game,
  a frame still open after 50 ms logs the native stacks of the main thread,
  the GPU executor and the flip thread ([HOLDSTACK] slowNNNms-main/-exec/
  -flip), named by `python tools\slowstacks.py play_stderr.txt`. Vulkan
  pipelines are logged with their creation time ([VKR] pipeline N: x ms):
  all under 4 ms, so not a stutter source here.
- PICKUP SPARKLE FPS DROPS (fixed 2026-10-16): grabbing a trick
  multiplier dropped to ~10-20 FPS at 5120x1440 (user's play with `play.bat
  diag`: main thread in BlockOnTime, executor waiting for a frame slot, flip
  thread on the GPU fence -- GPU bound). RECOMP_VKR_GPUPROF=<ms> (vkr.c: a
  timestamp after every draw, per frame slot; a frame over <ms> of GPU time
  logs its 8 costliest draws as [GPUPROF] with topology, count, blend,
  stages, textures, clip) showed four point-sprite draws of 56 points
  (program point size oPts, additive SRC_ALPHA/ONE, stage 3 point sprite) at
  13-25 ms each. xbox_vsh.vert used max(oPts.x, 1) * scale with no upper
  bound; the NV2A tops out at 64 px (SET_POINT_SIZE 6.3 fixed point, Xbox
  D3D MaxPointSize 64), so sparkles near the camera became screen-sized.
  Now clamp(oPts.x, 1, 64) * scale: Peak 1 freestyle event 2 (Mode 1 Event
  1, the rider grabs one in the air) at 5120x1440 went from 102-126 ms worst
  frames to 22.5 ms, no GPU frame over 25 ms.
- GPU COST AT HIGH RESOLUTION (2026-10-16, user: choppy in the air at
  5120x1440 after the sparkle fix): ordinary frames needed 12 ms of GPU on
  average and up to 21 ms (RECOMP_VKR_GPUPROF also prints "120 frames: GPU
  avg/max"), cost spread over ~650 draws. xbox.frag wrote gl_FragDepth for
  every draw (gl_FragCoord.z unless W-buffered), which turns off early depth
  testing: every hidden pixel was shaded. Now only the xbox_frag_wdepth
  variant (built with -DW_DEPTH, CMake vkr_shader_variant; pipeline key[7]
  = w_buffer) writes depth. Freestyle event 2 at 5120x1440: avg 12.2 ->
  9.35 ms, max 21 -> 16 ms. Neither that event nor the Snow Jam race has a
  W-buffered draw at all. Next lever if needed: the per-pixel combiner
  interpreter (dynamically indexed R[16], words decoded per fragment) --
  specialization constants per combiner setup would let the compiler fold
  it; and the ~1.4 ms per frame readback of the full image for the window.
- CHOPPY AT A STEADY 60 (2026-10-16): with the GPU cost down the game ran
  55-60 FPS, yet still felt choppy at 5120x1440. The window loop
  (fb_present.c) copied the 29 MB image, gamma-corrected all 7.4 M pixels on
  the CPU and uploaded it on every pass -- 8.4 ms of work, ~90 passes/s,
  unsynchronised with the 60 Hz frames (and redoing repeats), so each frame
  reached the screen 0-11 ms late, unevenly. Now: a triple buffer
  (xbox_FramebufferPublishImage: back/middle/front, flag 4 = new; no copy on
  the window side), the window thread waits (MsgWaitForMultipleObjects on
  s_hi_event) and presents each new frame once, at once; the gamma ramp is
  applied by the presenter's pixel shader (gpu_present.c psg + 256x1 LUT,
  xbox_FramebufferGammaTexels; the 640x480 path keeps CPU gamma). Result:
  60 passes, 60 new frames, 0.1 ms work per pass. FPS overlay (F3,
  RECOMP_FPS_OVERLAY=1): "FPS n SHOWN n MAX n MS", drawn into the image;
  RECOMP_FRAME_STATS adds "[PRESENT] loops/new/work/present" per second.
  RECOMP_SNAP_PRESENTED=1 makes the snapshot trigger capture the presented
  back buffer (gpu_present_capture) -- PrintWindow cannot read the swap
  chain, and grabbing the screen captures whatever is in front.
- STENCIL (2026-10-16, user: other riders' board trails show through the
  slope in front -- Snow Jam race, brake ~4 times after the start, trails
  right of the rider at ~0:09). The snow trail surfaces fix up depth with
  the stencil buffer: pass 1 (colour off, depth ALWAYS, stencil zpass
  ZERO), pass 2 (depth LEQUAL, zpass REPLACE ref 1), pass 3 (depth ALWAYS
  with depth WRITE, stencil EQUAL 1) -- then the visible surface. The
  executor recorded stencil but nothing applied it, so pass 3 wrote the far
  surface's depth over everything in its box, the near crest included, and
  the trails behind the crest passed their depth test there. Now: Nv2aTarget
  carries the stencil state (toolkit, Z24S8 only), the Vulkan depth buffer
  is D32_SFLOAT_S8_UINT (or D24S8; none -> stencil ignored, logged), stencil
  test/op/masks/ref are dynamic state (no new pipelines), clears clear the
  stencil (vkr_clear_ds). GPU time unchanged (8.3 ms avg at 5120x1440).
  Found with: RECOMP_VKR_PRIMLOG=2 (now with depth and stencil state),
  RECOMP_GPU_VSH=0 + RECOMP_VKR_TRACE_BRIEF=<ms after the briefing> +
  RECOMP_VKR_TRACE_EVERY (per-draw boxes with depth range), RECOMP_VKR_CLEARLOG=1,
  work\probe\trails.ps1 (race start, N short brakes, shots).
- COMBINER SPECIALIZATION (2026-10-18, Linux/Asahi M2 Max: race 37-55 FPS
  at 1728x1080, GPU-bound): xbox.frag interpreted the register combiners
  per pixel (R[16] indexed by uniform words) -- ~80% of the GPU time on
  Honeykrisp (12 ms/frame vs 2.4 ms with a trivial shader; a switch-based
  register file instead of the array was far worse, 145 ms). Now the
  combiner words, stage count and alpha func are specialization constants
  (xbox.frag SPEC + S0..S37, vkr.c spec_words; stages unrolled by hand) and
  part of the pipeline key (hash-indexed). A new specialized pipeline takes
  ~90 ms to compile cold on Honeykrisp, so a worker thread builds it
  (pipe_worker) while draws use the interpreting pipeline; Mesa's disk cache
  has it after the first time. Race: GPU 12 -> 2.2 ms/frame, steady 60 FPS.
  RECOMP_VKR_SPEC=0 = interpreter only, RECOMP_VKR_SPEC_SYNC=1 = compile
  on the spot. RECOMP_VKR_GPUPROF_FRAME=1: frame start/end timestamps only
  (per-draw timestamps split the render pass on a tiler and inflate the
  numbers); the 120-frame line also prints render passes per frame (~3).
  Linux snapshots (RECOMP_SNAP_TRIGGER) now write the full-resolution frame
  the window shows (it wrote a stale 640x480 one).
  Video: RECOMP_RECORD=<file.mkv> (vkr_backend.c record_frame) pipes every
  full-resolution frame, gamma applied, to ffmpeg as 60 FPS H.264;
  RECOMP_RECORD_INGAME=1 starts at the first in-game frame.
  OPEN: intermittent abort at boot, `malloc(): unaligned tcache chunk
  detected` in the SDL window thread's SDL_CreateTexture (fb_thread, first
  full-resolution frame), ~3 in 35 boots, with RECOMP_VKR_SPEC on and off;
  a core from before this work exists too (coredumpctl list ssx3_recomp).
  Heap corrupted earlier by something else; suspect around the target
  resize at boot (vkr_set_target / flip_thread reading c->sw x c->sh).
- MODERN RACE CAMERA (2026-10-18, off by default; launcher checkbox or
  RECOMP_CAM=1):
  src/camera_mode.c around the in-game update (sub_000CC8F0 wrapper): the
  game's camera values are restored before each update and the chase
  camera ([[state+0x84]+4], vtable 0x28630C) is rewritten after it --
  spring-filtered eye offset (height slower) and view, look-ahead and roll
  from the travel direction's turn rate, shake on landings (impact from
  the vertical speed before Air -> Ground) and at speed; wider fov with
  speed through the sub_0024EA13 wrapper (ssx3_CamFovScale). The eye is kept
  out of the terrain with the world collision query (sub_001BD340 on a set
  built by sub_001C0310 for the segment's box, as on the fps-mode branch). Knobs
  RECOMP_CAM_SMOOTH/_FOV/_ROLL/_SHAKE, RECOMP_CAM_TRACE.
- MOTION BLUR (2026-10-17, off by default; launcher checkbox or
  RECOMP_MBLUR=<strength>): scene pass before the HUD (src/vk/shaders/
  scene.frag) -- camera blur by reprojection plus radial speed streaks from
  the heading, scaled by camera speed and distance; McGuire 2012 depth
  weights; rider and board draws (E9FDBED0, 5D983485) are matched to a rider
  slot by their first vertex, unprojected through the fitted camera, within
  3 m of ssx3_RiderPos (rider manager slots: 0 player 1, vtable 0x00282B08;
  1-5 computer riders, 0x00282520) and tagged in alpha (tag / 255: 255
  player 1, kept sharp; 1..7 another rider). Other riders blur by their own
  motion on screen (scene_flip: position through this frame's camera minus
  last frame's through last frame's; none across a >5 m jump or near the eye
  plane), so one riding alongside stays sharp and one passing streaks
  ([MBLUR-RIDERS] with RECOMP_MBLUR_TRACE; RECOMP_MBLUR_RIDERS=1: all
  riders sharp). Camera fitted from the terrain program each frame (scene_camera,
  +-h steps: a unit step jittered). Knobs: RECOMP_SCENE_PARAMS (speed, s0,
  s1, reach, edge, cap, near, mindist). Test: RECOMP_AUTONAV=race +
  RECOMP_MBLUR_TEST=<dir> (blur/ref PPM pairs, [MBLUR-TEST] speed and
  reprojection; _PLAN=brake=,shots=); RECOMP_MBLUR_TRACE, _PROGS. ~1 ms GPU.
- LAUNCHER (2026-10-16): build\launcher.exe (src/launcher/launcher.c,
  plain Win32, CMake target ssx3_launcher) -- game folder picker that reads
  default.xbe's certificate (title id 4541003C = SSX 3 PAL, title name) and
  checks data\, resolution / aspect / fullscreen (toolkit RECOMP_FULLSCREEN,
  new) / CRT / FPS counter / skip intro / debug menu, XInput controllers
  (type, battery; refreshed 1/s), Play = launcher.ini + RECOMP_* env +
  CreateProcess(ssx3_recomp.exe, cwd = its folder) and exit. Static CRT
  (CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded): no VC++ redistributable needed.
  tools\package.ps1 -> dist\SSX3-recomp(.zip): both exes, LICENSE, keys
  example, docs/PLAYER-README.txt. Probes: work\probe\launcher_shot.ps1
  (PrintWindow capture, -Play posts the Play button), launcher_play.ps1.
- XEMU REFERENCE (2026-10-15): `tools\xemu.ps1` (skill: .claude/skills/xemu)
  boots a separate, muted xemu instance with the ISO (own config + HDD copy
  in work\xemu\, QMP on 127.0.0.1:4455), presses pad buttons by posting key
  messages (no focus change), captures the window (PrintWindow) and saves/
  loads VM snapshots: charsel_griff, race_briefing, race_go -- e.g.
  `xemu.ps1 start -Snapshot race_go -Paused` is at the Snow Jam start in
  ~2.5 s. The snapshots live in work\xemu\xbox_hdd.qcow2 (not in git).
- FRAME PACING (2026-10-14): SSX 3 paces frames with a self-correcting
  one-shot XAPI timer (sub_001AE480 sets it up, callback sub_001AE020 ->
  sub_001AE030: period 1000/60 ms, re-armed from KeTickCount, wakes the main
  thread parked in sub_001ADF40). KeTickCount was written from GetTickCount()
  (15.6 ms steps) and only between executor passes, so it stood still for
  ms at a time; the timer swung between ~3 and ~30 ms (40% of intervals over
  20 ms). Toolkit: xbox_TickCountMs() (QPC, 1 ms) and a 1 ms tick thread;
  kernel timers have sub-ms due times and arming wakes the timer thread; the
  guest-CPU handover's 5 ms waits use the 1 ms clock too. Now every interval
  is 14-19 ms; race ~57.5 FPS, p99 ~27 ms (was ~55 / 32-35). Remaining
  hitches are elsewhere (flip/vblank phase?). Probes: RECOMP_FRAMETIMER_STATS=1
  (intervals, how late the timer fired, fire->callback), RECOMP_TIMER_STATS=1
  (kernel timer lateness), RECOMP_VKR_SKIP_RT=1 (drop shadow-map draws:
  only ~2 FPS, so GPU shadow maps are not the 60 FPS lever).
- LONG-SESSION SLOWDOWN (fixed 2026-10-15): in one session race FPS fell
  from 60 to ~20 after ~5 minutes (cycle_test soak). Found by sampling the
  guest-CPU holder from a host thread (RECOMP_GUESTCPU_STATS=1, in the
  rescue watchdog: longest hold per second, and a [HOLDSTACK] native stack
  of any hold over 25 ms, ssx3_StackOfThread in thread_dump.c). Two holders
  blocked while holding the guest CPU: (1) recomp_unimpl -- the GPU ISR's
  `in al, dx` (0x0023B50D) runs on every interrupt and each call read the
  environment / printed (CRT locks; once 2.3 s); now the setting is read
  once and each address reported once; (2) the APU frame thread slept for
  XAudio2 queue room while holding the APU locks (my earlier crackle fix),
  so the guest's DirectSound MMIO writes (fe_method -> voice_lock) waited
  with the guest CPU held -- the queue drifts full over minutes; the pacing
  now happens in throttle() with the lock released. Result: 20-cycle soak at
  59-60 FPS throughout, audio 3 underruns in 20 min. Also from the hunt: the
  Vulkan texture cache is hash-indexed with age-based eviction (was a
  linear search over every texture ever made), the process opts out of
  Windows power/timer-resolution throttling, and the window thread sleeps
  while its swap chain is occluded.
- ONE-FRAME SLABS AND WEDGES (fixed 2026-10-21, user's 60 fps video): (1)
  the floor stretched from the screen edge to the rider (the vanishing
  point) for a frame at the start gate and in pipes: a vertex almost exactly
  on the camera plane (|w| < 5.4e-20). The game's epilogue divides with RCC
  (reciprocal clamped to 1.884467e19); xbox.vert / xbox_vsh.vert rebuilt
  clip space as ndc * w, which for a clamped reciprocal put the vertex near
  the screen centre. Now ndc / rcc(w) (identical for every other vertex).
  Deterministic: Peak 1 Mode 1 Event 2 countdown, frames ~926/978/1022.
  (2) 2D panels sliding in from off-screen (radio box, briefing) were
  stretched from the screen edge: the "keep edge vertices on the edges" rule
  of full-screen passes was per vertex; now per draw (push constant
  keep_edges, set for draws > 0.9 of the width). (3) the briefing panel's
  first frames were laid out as HUD (half at the edge): ingame_menu_note
  runs after the screen state change too. Tools: RECOMP_GLITCH_CATCH=<dir>
  (vkr_backend.c: frames that differ from both neighbours while those agree,
  in game; writes glitch_<n>_{a,b,c}.bmp + _draws.txt, the draws of the
  three frames with screen boxes -- CPU-path draws only, so add
  RECOMP_GPU_VSH=0 for a full list), RECOMP_GLITCH_FROM=<frame> skips the frames before (the race-start
  spotlights fill the 60-capture budget), RECOMP_SPIKE_TRACE=1 (object-space
  outlier triangles), RECOMP_DRAWLOG=<a>-<b> (every CPU-path draw; slows the
  game enough to change what it draws). work\probe\hudrun.ps1 -Extra
  'K=V;K=V' -Ws 0 runs the freestyle event with extra settings.
- Untranslated instructions reached (no-ops): `wbinvd`, `out`/`in`, `ldmxcsr`.
- Upstream candidates: the toolkit fixes in `patches/`; `tools.split`
  ignoring applied names (docs/DECOMP.md says otherwise); template
  `recomp_manual.c` declaring the thread-local registers as plain globals;
  the x87 dispatcher stack imbalance (ctrandisp1/trandisp1 tails); the ABI
  check not flagging esp returning too *high*.

## Gotchas

- Hand-written C must use `g_eax`/`g_esp`/... via `#include "recomp/gen/recomp_types.h"`;
  the bare `eax`/`esp` macros exist only under `RECOMP_GENERATED_CODE`. The
  registers are `__declspec(thread)`: a plain `extern uint32_t g_eax`
  (as in the template's recomp_manual.c) names the wrong storage.
- An override is `void sub_X(void)`: set `g_eax`, then `g_esp += 4` (pop the
  return address; plus 4*n for stdcall args).
- Re-running `tools.disasm` wipes the names; `scripts/regen.sh` re-applies them.
- Never disassemble the DOLBY section (DSP56300 microcode).
- Wrapping a function with many direct callers: declare
  `extern void sub_X_gen(void);` and define `void sub_X(void)` that calls it;
  the recompiler then emits the original body as `sub_X_gen` (regen + full
  rebuild).
- `--exclude-manual` only recognises `void sub_XXXXXXXX(void)` with nothing
  else on that line; a trailing comment silently keeps the generated body.
- Callee-saved register bugs: `tools\build.bat 16 abi` builds `build-abi\`
  with `-DRECOMP_ABI_CHECK`; run it with `run.ps1 -Build build-abi`.
- Field watching: `RECOMP_PEEK=<addrs> RECOMP_PEEK_EVERY_MS=2000` (ours,
  src/thread_dump.c) samples guest memory on a clock; `RECOMP_TRACE_FRAMETIMER=1`
  also logs the update gate (`[GATE]`) with return values and guest regs.
- Sync with `rsync -rc` (no `-t`), or Ninja rebuilds all 77 generated files.
- `tools/split_named.py` / `regen.sh` print the disasm stats; the split was
  verified byte-identical against the XBE (19,096/19,096).
- Ghidra (only needed to redo naming): Linux build in WSL, Jython enabled by
  unzipping `Extensions/Ghidra/*Jython.zip` into `Ghidra/Extensions/`; the
  toolkit's `run_ghidra.sh` is Git-Bash-only, so call `support/analyzeHeadless`
  with the same flags (raw x86:LE:32, base 0x10000, `SetAnalysisOptions.java`,
  `ExportXbeNames.py`).
