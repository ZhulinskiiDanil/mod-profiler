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

- **Localization (backlog 9) is only half possible: the table stays English, the README is in Russian too.**
  GD's bitmap fonts (`chatFont`, `bigFont`) have 95 ASCII glyphs, no Cyrillic. A TTF label would render its
  texture again on every update, twice a second for 50 labels: a cost the profiler would show as its own.
- **Improved Logger (backlog 6) is a soft dependency.** `"required": false` in mod.json, its API header copied
  in, as its docs suggest, so the mod builds and runs without it. It has no event for its markers, so the
  profiler listens to its marker keybind (`Ctrl+Shift+M` in the game). A marker set in its console with `b`
  isn't seen. `geode build` warns that the logger isn't on the Geode Index: fine for GitHub releases, but it
  matters if the profiler goes to the Index.
- **The comparison (backlog 7) is against a saved session, kept by mod ID.** Saved to the mod's save folder, so
  it survives a restart and a mod update, which is what a before/after needs.
- **Keys and clicks in the checks are real.** `SendInput` with scan codes (GLFW reads keys by scan code, so
  `SendKeys` did nothing) and absolute mouse clicks on the table's buttons.

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

- 05:05 Improved Logger link (backlog 6), checked with real keys: its marker key logged "Since the last marker:
  47.0 s, 222 fps, ... own: ImagePlus 0.22 ms". 05:20 the baseline (backlog 7), checked with real clicks on
  [set baseline] and [reset]: `baseline.json` written, the table's base column, the comparison at a marker.
- 05:20 README.ru.md, about.md for the index (backlog 9).
- 05:30 code review (backlog 10) by a separate reviewer agent, no file edits by it. Nothing critical: every path
  in the suspended window resumes the thread, nothing in it allocates or locks, all reads are bounded. Fixed:
  pins held for the whole session (a DLL a mod unloaded never went away: now released every 5 s), seq_cst
  for the map/iteration handshake, fake slow frames after lost frame ends, slow frames without samples,
  failed samples counted as the game's, the snapshot deep-copied on the main thread, DbgHelp work while the
  table is hidden. Left as they are, with the reasons:
  - a fault inside the walk would go through vectored exception handlers, one of which could lock. The checks
    before `RtlVirtualUnwind` make a fault very unlikely; copying the stack and unwinding the copy would remove
    it but needs an unwinder of its own (RtlVirtualUnwind reads the real stack through the frame register)
  - in the last bytes of an `rbp` epilog the frame check fails and the sample keeps only its first frame
  - DbgHelp is process-wide: if the game crashes while the analyzer loads a PDB, Geode's crash report may lack
    symbols. Names are now only made for the table when it's shown, for slow frames and for exports
- 05:28 gameplay checks: the mouse can't press the game's own buttons (the table's buttons work), so
  `MOD_PROFILER_SELFTEST=play` opens Stereo Madness through the game's API. Opening it is a 130-150 ms slow
  frame, about half of it in Geode's hook dispatch (`tulip::hook::Thiscall64Convention`'s shared_ptr released
  under the hooks of `CCSprite::create` and `CCSpriteBatchNode::initWithTexture`): worth telling Geode about.
- 05:40 v0.3.0 released (pre-release) after a 100 s check in a level.

- 05:45 exact stacks through TulipHook's handlers and trampolines: they start with `push rbp; mov rbp, rsp`
  and a calling convention may move RSP past what their unwind info says, so past their prolog the frame
  comes from RBP. In a level 234 of 240 sampled stacks go down to `RtlUserThreadStart`.
- 05:49 Geode's helper for calling the original (between a mod's detour and the original) is passed over:
  opening a level went from 49-53% Geode's own time to 25-34%, now in Geode's own hook of
  `CCSpriteFrameCache::spriteFrameByName` (`operator new`).
- 05:53 **the game's start** (own idea, after the backlog): "which mod makes my game load slowly" is the
  question a profiler answers best. The profiler loads early now (`early-load`, priority -2900, right after
  Improved Logger) and keeps the start's samples until the hooks are known. With 60 mods: 8.0 s,
  ImagePlus 2.83-2.90 s (35%, decoding images under its hook of `CCImage::initWithImageFile`), Mega Hack
  1.35-1.39 s, Geode 0.32-0.35 s. The start's flame graph can be saved too.
- 05:58 numbers by scene (own idea): a level's numbers apart from the menu's, summed up when the game leaves.
- 06:00 the analyzer measured itself: building a map was 5-19 ms a second (every module's path compared
  with every mod's, lowered each time). Now 0.7% of a core instead of 1.3%.
- 06:08 the worst single stops (10-20 ms) are all *stopping*: Windows delivers a suspension only when the
  thread leaves a long system call, the thread works meanwhile. They're at the game's start. The README says
  the 2.6% is an upper bound.
- 06:10 v0.4.0 released (pre-release) after a check in a level.

- 06:12 the hook handlers' own time and the hook library's bookkeeping became a row of their own, "Hook
  dispatch" (0.48 s of the start, 0.03 ms a frame in a level). Settings for the table's corner and size.
  Review item 8 (epilog tails of rbp frames) fixed, only where the bytes are pops then ret.
- 06:20 a test of the sampler itself on Windows (`tests/SamplerTests.cpp`): a thread stuck in a known chain
  of functions is sampled by the real sampler; every sample must walk the chain in order down to the
  thread's start, every sample of a 200-deep recursion must be cut at 64 frames and marked. CI runs it with
  MSVC (a leaf without a frame has no `.pdata` there: the test's innermost function calls a leaf now).
- 06:31 a 20 minute soak in a level with the table shown: 1264 diagnostics lines, no gap, no crash, 999
  samples a second, 29.7 us a sample, the analyzer 0.38% of a core. The game's memory 1.370 GB after a
  minute, 1.392 GB after 21: growth flattens (+0.2 MB in the last 4 minutes).

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
| v0.3.0, 100 s in a level | 994 | 29.6 us | 733 us single | 18 | stopped 29.4 ms/s, sampler 2.0%, analyzer 0.7%, no gap, no crash |
| sampling off, 80 s in a level | 0 | - | - | - | no diagnostics, no slow frames, table says "No samples yet" |
| v0.4.0, start + 20 s menu + 75 s level | 997 | 28.9 us | 46.0 us avg in the worst second | 20 | start counted (8.0 s), no gap, no crash, analyzer 0.69% |
| soak, 21 min in a level, table shown | 999 | 29.7 us | 50.4 us avg in the worst second, 1160 us single | 2.2 | 1264 lines, no gap, no crash, memory flat after ~5 min |
| sampler test (Windows, MSVC and clang) | 1000 | 22-26 us | - | - | 490-499 of 490-499 samples walk the chain, 292-297 of 292-297 deep ones cut at 64 |
