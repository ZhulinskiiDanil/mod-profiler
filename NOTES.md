# NOTES: Mod Profiler, the autonomous run

Start: **Sun Oct 11 03:33:17 2026** (the first `date` of the run). End: **Sun Oct 11 11:35:49 2026**, 8 h 02 min.
The last checks, after 8 hours: `geode build` packaged and installed, 11 of 11 test suites passed, the last game
run (111 s in the main menu, v0.8.1) had 111 diagnostics lines, no gap, 27.2 us a sample, no error, no crash.

## Links

- Repository: https://github.com/ZhulinskiiDanil/mod-profiler
- Final release: https://github.com/ZhulinskiiDanil/mod-profiler/releases/tag/v0.8.1 (`zhulis.mod-profiler.geode`,
  sha256 7a12a2e2...639c7a); all releases, v0.1.0 to v0.8.1: https://github.com/ZhulinskiiDanil/mod-profiler/releases
  (pre-releases, so GitHub's "latest" link skips them)
- Zhulis Mods Manager: the entry in `mods.json`, https://github.com/ZhulinskiiDanil/zhulis-mods-manager/blob/main/mods.json
  (commit c72bc67), and in its built-in list (commit 8711519). The manager shows pre-releases with
  *Include pre-releases* on: its beta channel. Its cache had fetched the profiler's releases at 04:22.

## What's done

Stage 1 and the whole backlog, then more of my own:

| | What | Where it's checked |
| --- | --- | --- |
| Stage 1 | The sampler: SuspendThread, GetThreadContext, the walk with its own `.pdata` copy and RtlVirtualUnwind, ResumeThread, nothing allocated, logged or locked in between. A lock-free ring. Mods' DLLs to buckets (game, cocos, other, each mod), inclusive and self. Diagnostics once a second | every game run below, the sampler test, three reviews |
| Backlog 1 | The table in the game (`Ctrl+Shift+P`): own ms a frame, own %, inclusive %, the picked mod's hooks and functions | screenshots, clicks |
| Backlog 2 | Names from PDBs with DbgHelp (own session handle, copies of build-folder PDBs, unloaded when unused) | names in the table, a rebuild while the game runs links |
| Backlog 3 | Hook attribution: own time (pass-through detours excluded), the hooks a mod's time is under | DevTools 97% under swapBuffers, the cost test |
| Backlog 4 | Slow frames with their culprit, function, hook, stacks; hangs and deadlocks reported while they last | self tests: 121 ms, 4.5 s, 4 s wait |
| Backlog 5 | Export for speedscope.app (last 10 s, the game's start), zones as frames | JSON validated, 10 000 samples, every stack to the thread's start |
| Backlog 6 | Improved Logger (soft): pinned numbers, a profile between markers | real keys, the summary in the log |
| Backlog 7 | Baseline comparison, kept across restarts | real clicks, `baseline.json` |
| Backlog 8 | The profiler's own cost, live and measured at 1, 2, 5 ms | README table |
| Backlog 9 | Settings, about.md, README in Russian (the game's fonts have no Cyrillic) | |
| Backlog 10 | Three code reviews by separate agents, 27 findings (none critical), all fixed or explained | |
| Mine | The game's start (which mods make it load slowly), numbers by scene, the game loop's "between frames", hook dispatch as a row, zones for mod developers, exact frames through TulipHook's code, a session summary, the table's corner and size, a Windows test of the sampler itself | log lines, the sampler test |

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
- **The modules in the map are pinned** (`GetModuleHandleEx`) so none can be unmapped while the sampler
  reads its tables. An old map is let go only after the sampler has moved on from it (and kept on a retired
  list if waiting for that timed out). Every 30 s all pins are let go before the next map, so a DLL some mod
  unloaded, which only the pins kept, goes away.
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

- 06:38 **hang reports** (own idea): a slow frame is only seen when it ends, a frozen game never ends it. A
  frame going on for 2 s is attributed and logged while it lasts. Then **deadlocks**: a frame waiting on a
  lock or another thread (`NtWaitForSingleObject`, `NtWaitForAlertByThreadId`, keyed events) 80% of it.
  False alarms found and fixed with real checks: a minimized game (20 s, waiting for time and messages) was
  one 20 s "slow frame", and closing the game (Cocos destroying textures) was a "hang". Neither is reported
  now. The session is summed up in the log when the game closes.
- 06:57 v0.5.0 released (pre-release) after a check in a level.
- 07:00 how exact the numbers are, with a known cost (`MOD_PROFILER_SELFTEST=cost`, 0.500 ms of every frame
  in the profiler's own code): measured 0.514 ms a frame on average over 30 s.

- 07:05 **zones** (own idea, for mod developers): `mod_profiler::Zone zone("hair simulation")` in a mod's
  code counts the samples taken in it for that zone, shown under the mod in ms a frame. A soft dependency
  like Improved Logger's API, but without an event per call: the header asks for a table of two function
  pointers once. The sampler reads the main thread's zone stack while it's suspended (the profiler's own
  memory, written id first, depth second). The cost self test runs in a zone: 0.50 ms in the table.
- 07:06 a second review by a separate reviewer agent, of everything since v0.3.0. Nothing critical again (the
  suspended window clean). Fixed all nine findings: pins let go after a timed-out wait (now kept on a retired
  list), epilogs whose RSP reads weren't checked (a decoder of what RtlVirtualUnwind takes for an epilog),
  hooks made after the game loaded (now looked at every 600 frames), the zone API cached as missing forever
  if a mod asked before the profiler loaded, the zone name cache keyed by pointer only, the start counted
  wrong with sampling turned on late, Geode's bucket possibly missing, the old snapshot freed under the
  lock, the start's memory estimate. Checked again after: sampler test, a level, the cost test (0.512 ms).

- 07:23 v0.6.0 released (pre-release): zones, and the second review's fixes.
- 07:25 a test for the hook handler rule (review gap): the sampler test builds a handler the way TulipHook
  does (`push rbp; mov rbp, rsp; sub rsp, 0xc0`, unwind info for that, then `sub rsp, 0x40` and a call),
  registers it with `RtlAddFunctionTable`, learns it and walks through it. It found a real bug: 0 of 233
  samples. For return addresses the epilog check looked at the bytes after the call (`add rsp, 0x40`) and
  skipped the frame-pointer rule. Fixed: the epilog check is for the first frame only; 292-294 of 292-294.
- 07:28 `geode build` failed while the game ran: `LNK1201`, the PDB locked. The profiler's DbgHelp had it
  open, it names the profiler's own functions. A mod developer rebuilding their mod with the game open
  would hit the same. First fix: close those PDBs every second; that cost 2% of a core (15-18% in some
  seconds) reopening a 56 MB PDB. Final fix: a PDB from a build folder is copied once to %TEMP% and the copy
  is loaded. Checked: `geode build` while the game ran links, names still come, the analyzer at 0.40%.
- 07:30 `functionStart` cached in the map (only the analyzer calls it).
- 08:04 a 40 minute soak in a level, table shown (v0.6.0): 2466 diagnostics lines, no gap, no crash, 999
  samples a second, 29.6 us a sample, analyzer 0.40%, 1.4 scanned a second. The game's memory 1.388 GB after
  a minute, 1.413 after 27, then one step of 49 MB between 07:50 and 07:58, flat after (1.462). A soak with
  sampling off is the comparison, see below.
- 08:17 v0.7.0 released (pre-release): rebuilding a mod while the game runs.
- 08:38 the soak with sampling off (stopped after 20 min): the game at 1.145 GB (1.388 with sampling and the
  table), +7 MB in 16 min. The difference was DbgHelp keeping the PDBs it read for names (Geode.pdb is 90 MB
  on disk, a mod's 56 MB), and the 49 MB step in the long soak was one more mod's PDB loaded. Fixed: PDBs are
  unloaded 30 s after the last name lookup. Checked: the table shown 1.485 GB, hidden 50 s later 1.255 GB.
- 08:40 zones are frames of their own in exported profiles: `[zone] self test cost` right above its
  function, 1267 ms of 10 s (0.53 ms a frame for 0.5 ms of work). The start's samples kept their zone too.
- 08:58 a 17 minute soak, sampling on, table hidden: 1035 lines, no gap, the game's memory 1.1076 to 1.1086
  GB (flat), analyzer 0.28%. It was cut short by my own mistake: the soak with sampling off that I stopped
  with TaskStop left its script running, and that script's last `taskkill` closed the next game. Checked by
  building while the game ran (the build doesn't close it) and by that script's output file.
- 09:00 a third review (changes since v0.6.0): no critical issue, all fixed: the build-folder PDB copied when
  the module is first seen (a copy made later could be of a rebuilt, mismatching PDB), the zone frame above
  the mod's innermost frame (the outermost would pull a whole frame into the zone of a mod hooking it), PDBs
  kept while the table uses names, RBP trusted in hook code until its `ret`, a failed copy not loaded in
  place, the clean-up not following links or deleting a running game's copies, the test failing cleanly.
- 09:14 v0.8.0 released. 09:55 the final 40 minute soak on it, table shown: 2466 lines, no gap, no crash.
- 10:02 the regression suite on HEAD: hang 2.7 s, deadlock 2.5 s, minimized 0 reports, cost 0.507 ms, the
  sampler test 489/489, 292/292, 295/295.
- 10:32 the last hour: only the version, the changelog, the checks. 10:35 the stage 1 check on v0.8.1: 130 s
  in the main menu, 131 diagnostics lines, no gap, 27.3 us a sample, no crash. 10:40 v0.8.1 released, the
  final one, `.geode` attached by CI.
- 10:41 a last 40 minute soak on v0.8.1 in a level: 2466 lines, no gap, no crash, memory flat after the
  PDBs of the start's names were let go.

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
| hang self test (4.5 s frame) | 1000 | - | - | - | reported at 2.7 s: Mod Profiler 100% in selfTestHang, then "Slow frame: 4502 ms" |
| deadlock self test (4 s wait) | 1000 | - | - | - | reported at 2.5 s: waiting on a lock 100%, in a call of Mod Profiler |
| minimized 20 s, then closed | - | - | - | - | no hang, no slow frame reported |
| cost self test (0.500 ms a frame) | 1000 | - | - | - | measured 0.514 ms a frame (30 s mean, 0.49-0.57 a second) |
| v0.5.0, start + 20 s menu + 100 s level | 999 | 29.3 us | 40.4 us avg in the worst second | 20 | no gap, no crash |
| soak, 40 min in a level, table shown (v0.6.0) | 999 | 29.6 us | 52.7 us avg in the worst second | 1.4 | 2466 lines, no gap, no crash; memory +74 MB (PDBs) |
| soak, 20 min, sampling off (comparison) | 0 | - | - | - | memory 1.145 GB, +7 MB in 16 min: the game's own |
| table shown, then hidden 50 s (v0.8.0 code) | 1000 | - | - | - | 1.485 GB, then 1.255 GB: PDBs let go |
| soak, 17 min, sampling on, table hidden | 999 | 29.1 us | 41.2 us avg in the worst second | 2.7 | 1035 lines, no gap; memory flat (+1 MB); analyzer 0.28% |
| soak, 40 min in a level, table shown (v0.8.0) | 999 | 29.1 us | 43.2 us avg in the worst second | 1.4 | 2466 lines, no gap, no crash; memory +72 MB: one PDB (49 MB) and the game's ~0.5 MB a minute |
| rebuild while the game runs | - | - | - | - | `geode build` links (was LNK1201) |
| v0.8.1, 130 s in the main menu (stage 1 check) | 999 | 27.3 us | 33.5 us avg in the worst second | 14 | 131 lines, no gap, no crash |
| soak, 40 min in a level, v0.8.1, table hidden | 999 | 29.3 us | 44.2 us avg in the worst second | 1.1 | 2466 lines, no gap, no crash; memory 1.375 GB at the start (the start's names), 1.155 after the PDBs were let go, 1.169 at the end |
| sampler test (Windows, MSVC and clang) | 1000 | 22-26 us | - | - | 490-499 of 490-499 samples walk the chain, 292-297 of 292-297 deep ones cut at 64 |

## What didn't work, or only partly

- **Russian in the game.** GD's bitmap fonts have no Cyrillic; the table and notifications stay English, the
  README is in both languages.
- **Clicking the game's own buttons in a check.** Mouse input sent with `SendInput` reached the table's buttons
  but never pressed the game's Play button. The level checks open the level through the game's API instead
  (`MOD_PROFILER_SELFTEST=play`).
- **Hook handlers learned before the game loads.** Addresses in TulipHook's code are looked up only after
  `GameEvent::Loaded` (the lookup's callback reads a map the main thread writes while mods make hooks), so
  the start's stacks still go through some handlers by scanning. About 20-30 scanned samples a second in a
  level come from code that has no unwind info at all (Mega Hack's own trampolines); they're crossed by
  scanning and marked.
- **The worst single stop.** Up to 10-20 ms, all of it Windows waiting for the thread to leave a long system
  call (files read while the game loads). Nothing to do from user mode; the README explains it.
- **Mega Hack's frame limiter.** It spins on the clock between frames; that's told apart as "between frames",
  not hidden. Its own time inside frames is still shown.
- **The game's memory in a long soak** grew by 74 MB over 40 minutes: DbgHelp's PDBs, now let go when unused.

## What I'd do next

- **Geode Index.** The profiler depends on Improved Logger softly, but `geode build` warns it isn't on the
  Index; publishing Improved Logger there first (or dropping the declared dependency, the header copy is
  enough) would let the profiler go to the Index.
- **Learn hook handlers earlier.** Hook creation could be watched (Geode's hook events) so handlers made
  during loading are learned before the start's samples are counted.
- **Zones with their own time.** A zone shows all its time now; its own time (what isn't in a deeper zone
  or another mod) would be the next step, and zones on Geode's worker threads.
- **Other threads.** Geode's async runtime and mods' worker threads could be sampled the same way, with their
  own table, for mods that stall the main thread by waiting on them.
- **A frame-time graph** in the table, with the slow frames marked, and clicking one to see its stacks.
- **A "which mod to remove" view**: inclusive ms a frame per mod, which is what removing it would save.
- **Mods on Android/macOS**: the sampler is Windows-only (SuspendThread, RtlVirtualUnwind). Android would need
  a signal-based sampler and libunwind; possible, but its own project.
