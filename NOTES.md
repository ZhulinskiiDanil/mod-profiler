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

## Numbers

| Check | Samples/s | Suspended avg | Max | Scanned/s | Result |
| --- | --- | --- | --- | --- | --- |
| v0.1.0, 140 s menu | ~1000 | 26.8 us | 66.6 us avg in the worst second | ~90 | no hang, no crash |
| learning + 64 frames, 100 s | ~1000 | 27-28 us | 100-450 us single | 0-2 | no hang, no crash |
