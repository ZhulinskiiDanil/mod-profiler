# Mod Profiler

Shows how much of each frame every installed mod takes.

Press <cy>Ctrl+Shift+P</c> in the game for the table:

- **own ms**: what the mod costs a frame, its own work only. A hook that just calls the original function doesn't count, the work under it belongs to whoever does it
- **incl %**: how often the mod's code is anywhere on the stack, everything its hooks call included
- **Click a mod**: the hooks its time is under (like `MenuLayer::init`) and its functions, by name when the mod's PDB is around

The top line says how much of the time is in frames, between frames (a frame limiter waiting) and idle.

## How

About once a millisecond the profiler stops the game's main thread for a few microseconds, reads its call stack and lets it go. While the thread is stopped it only reads: no memory is allocated, nothing is logged or locked, so it can't freeze the game.

It checks itself in Geode's log once a second: how many samples, how long each stop took (usually 20 to 40 microseconds), and the mods taking the most.

Windows only.
