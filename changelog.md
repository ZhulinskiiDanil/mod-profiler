# v0.7.0

- A mod can be rebuilt while the game runs: the profiler read its PDB and kept it open, so linking failed. A PDB from a build folder is read from a copy now
- Exact frames through hook handlers whose code after a call looks like an epilog
- The analyzer caches where functions start

# v0.6.0

- Zones: `mod_profiler::Zone zone("hair simulation")` (include/ModProfiler.hpp) in a mod's code, and the table shows the zone's ms a frame under the mod. A soft dependency, a pointer check when Mod Profiler isn't there
- Fixes from a second code review: a module isn't unloaded while the sampler may still read its tables even when the wait for it times out, every epilog's reads are checked, hooks made after the game loaded count, the game's start is right with sampling turned on late
- Geode is always its own row, the game's start keeps at most 16 MB of stacks

# v0.5.0

- Hangs: a frame going on for 2 seconds is in the log before it ends, with the mod, its function and the stack the game is stuck in. Deadlocks too: a frame waiting on a lock or another thread, with the mod it waits in a call of. A minimized game, a dialog or the game closing aren't hangs
- Hook dispatch is a row of its own: the time in Geode's hook handlers and their bookkeeping
- Settings for the table's corner and size
- The session is summed up in the log when the game closes
- Stacks in the last bytes of a function aren't cut short anymore
- A test of the sampler itself, run in CI on Windows

# v0.4.0

- The game's start: the profiler loads before the other mods, the log says how long loading took and each mod's own seconds of it, with where the busiest one spent them. A `[startup]` view in the table, and the start's flame graph from `[save profile]` there
- Numbers by scene: a `[this scene]` view, and a summary in the log when the game leaves a scene
- Stacks through Geode's hook handlers and trampolines are exact: past their prolog the frame comes from RBP, their unwind info leaves out what a calling convention adds. In a level 234 of 240 stacks go all the way down
- Geode's helper for calling the original, between a mod's detour and the original, isn't Geode's own time anymore: opening a level was half Geode's, now it's a quarter, in its own hook
- The analyzer takes half the CPU it did (mods looked up by path in a table, no pause for a new map)

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
