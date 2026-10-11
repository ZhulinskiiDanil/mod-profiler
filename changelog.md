# v0.3.0

- Slow frames: every frame over 50 ms (a setting) is looked into, with the mod whose time it was, its function, its hook and a stack. At the bottom of the table and in the log
- `Ctrl+Shift+O` saves the last 10 seconds of stacks for speedscope.app
- A baseline: `[set baseline]` keeps the session's numbers through restarts, the table shows each mod's own time then next to now, red when it got worse. `[reset]` and a view of the whole session
- Improved Logger, optional: the numbers pinned in its console, its marker key logs the profile since the last marker, compared with the baseline
- What the profiler costs, in the table and the log: how long the main thread stood still a second, the CPU of its threads
- The table's buttons and a column for the baseline. README in Russian too (the game's fonts have no Cyrillic, so the table stays English)

# v0.2.0

- A table in the game (`Ctrl+Shift+P`): every mod's own time a frame, its own and inclusive share of the last 5 seconds
- Own time: each sample goes to the innermost mod doing its own work, not to a hook passing the call on to the original. Geode and mod menus are no longer near 100% of everything
- Click a mod for the hooks its time is under and its functions, named from PDBs, `Geode.pdb` and export tables
- Time between frames (a frame limiter spinning on the clock) and waiting in Windows are told apart from the frame
- Stacks go through Geode's hook handlers with their unwind info instead of being scanned past them, and up to 64 frames deep
- A setting for the function names

# v0.1.0

- A sampler stops the main thread about once a millisecond and walks its stack, without allocating, logging or locking while it's stopped
- Every mod's share of the time, inclusive and self, and of a frame in ms, with Geometry Dash, Cocos2d and the rest apart
- A self-diagnostics line in the log once a second: samples, how long the thread stood still, the mods taking the most
- Settings: sampling on or off, how often, the log line, sampled stacks in the log
