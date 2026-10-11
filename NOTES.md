# NOTES: Mod Profiler, the autonomous run

Start: **Sun Oct 11 03:33:17 2026** (the first `date` of the run).

## Decisions

- **The stack is walked with the mod's own copy of the function tables, not `RtlLookupFunctionEntry`.**
  Geode's hooks (TulipHook) register their generated code with `RtlInstallFunctionTableCallback`, and
  that callback can build a `std::stringstream` for its log. Calling the lookup while the main thread is
  suspended could then wait for a heap lock the main thread holds: the game would freeze for good. So the
  `.pdata` of every loaded module is read ahead of time, outside the suspension, and the walk only does a
  binary search over it and calls `RtlVirtualUnwind` (a pure function that reads the unwind codes and the
  stack).
- **Frames in code without unwind info (hook handlers, other mods' trampolines) are crossed by scanning
  the stack** for the next value that points into a known module's code right after a `call`. Those frames
  are marked, and the diagnostics count how many samples needed it.
- **The modules in the snapshot are pinned** (`GetModuleHandleEx`) so none can be unmapped while the
  sampler reads its tables. An old snapshot is freed only after the sampler has moved on from it.

- **Inclusive time alone is misleading, so there is an own time too.** In the first runs Geode was at 96%
  inclusive and Mega Hack at 90%: Geode calls the game's main function from its own start (a patched call),
  and Mega Hack's frame limiter spins in `CCApplication::run` between frames. Both are on the stack all the
  time, so inclusive time (what the spec asks for, and what a mod costs if it's removed) stays, and own time
  is added: each sample goes to the innermost mod doing its own work, not passing a call on to the original
  function or the next mod's detour of it (from `Mod::getHooks()`). Waiting in a kernel wait is *idle*, and
  being in the game's loop outside `CCDirector::mainLoop`/`CCEGLView::pollEvents` is *between frames*.
- **The walk goes up to 64 frames, not 32.** With the graphics driver on the stack it's 40 frames deep, and at
  32 about 45 samples a second were cut off before the mods near the bottom.
- **Hook handlers are learned, not only scanned.** TulipHook registers unwind info for its handlers.
  Unknown addresses from the samples are looked up with `RtlLookupFunctionEntry` on the main thread (its
  callback reads a `std::map` that only the main thread writes, when hooking), and the next maps have them.
  Scanned samples went from about 90 a second to 0-2.
- **Releases are GitHub pre-releases while the version is 0.x.** Zhulis Mods Manager shows them only with
  *Include pre-releases* on: that is its beta channel. The registry entry says "Beta" too.

## Log

- 03:33 start. Read improved-geode-logger (style, CMake, tests, CI) and zhulis-mods-manager (registry, pre-releases).
- 03:55 first build and run. 640 samples a second instead of 1000 (the timer waited 1 ms after each sample, not
  until the next millisecond): fixed with a fixed schedule.
- 04:03 check for v0.1.0: 140 s in the main menu with 60 mods, 138 diagnostics lines, no gap over 2 s, suspension
  26.8 us on average (24.0 to 66.6 a second), no crash. Tests 5/5, CI green (Windows build, GCC tests).
- 04:10 v0.1.0 released (pre-release), added to the manager's `mods.json` (commit c72bc67 there).
- 04:16 own time: in the menu at 240 fps, between frames 90%, the frame itself about 9%, own time Geode 0.4%,
  DevTools 0.4%, Mega Hack 0.2%. Suspension 25-28 us on average.

- 04:25 the table in the game (backlog 1). Picking a mod shows the hooks its own time is under (backlog 3):
  DevTools 97% under `CCEGLView::swapBuffers`, where it draws ImGui.
- 04:30 names with DbgHelp (backlog 2): Geode's functions from `Geode.pdb`, Cocos' from exports. No mod in the
  game folder ships a PDB, but a mod's DLL has the path of its PDB where it was built: a developer profiling
  their own mod gets names. TulipHook's bookkeeping in Geode.dll stopped counting as Geode's own time.
- 04:39 v0.2.0 released (pre-release) after a 110 s check: 108 lines, 27.5 us on average, no gap, no crash.
  The manager fetched the profiler's releases at 04:22 (its cache has `ZhulinskiiDanil_mod-profiler.json`).
- 04:47 what the profiler costs (backlog 8), measured at 1, 2 and 5 ms, in the README.
- 04:55 slow frames (backlog 4). Checked with `MOD_PROFILER_SELFTEST=1`, which makes one 120 ms frame of the
  profiler's own: "Slow frame: 121 ms, 121 samples: Mod Profiler 99% in selfTestSlowFrame".
- 05:01 export for speedscope.app (backlog 5): 10 000 samples, 654 functions, every stack down to
  `RtlUserThreadStart`, the self test's 120 ms in it. Speedscope, not Chrome Trace: speedscope opens Chrome's
  format too, and its sampled format is exactly what a sampler has.

## Numbers

| Check | Samples/s | Suspended avg | Max | Scanned/s | Result |
| --- | --- | --- | --- | --- | --- |
| v0.1.0, 140 s menu | ~1000 | 26.8 us | 66.6 us avg in the worst second | ~90 | no hang, no crash |
| learning + 64 frames, 100 s | ~1000 | 27-28 us | 100-450 us single | 0-2 | no hang, no crash |
| v0.2.0, 110 s menu | 1000 | 27.5 us | 47.6 us avg in the worst second | 0-2 | no hang, no crash |
| interval 1 ms, 80 s | 1000 | 26.1 us | 844 us single | 27 | stopped 26.1 ms/s, sampler 1.9%, analyzer 0.3% of a core |
| interval 2 ms, 80 s | 500 | 28.4 us | 649 us single | 8 | stopped 14.2 ms/s, sampler 0.9%, analyzer 0.2% |
| interval 5 ms, 80 s | 200 | 33.9 us | 335 us single | 3 | stopped 6.8 ms/s, analyzer 0.2% |
| self test, 100 s | 1000 | 26-27 us | - | - | slow frame of 121 ms found, 98-99% blamed on the profiler's test function |
