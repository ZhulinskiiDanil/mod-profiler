# Mod Profiler

Shows how much of each frame every installed mod takes.

Press <cy>Ctrl+Shift+P</c> in the game for the table:

- **own ms**: what the mod costs a frame, its own work only. A hook that just calls the original function doesn't count, the work under it belongs to whoever does it
- **incl %**: how often the mod's code is anywhere on the stack, everything its hooks call included
- **Click a mod**: the hooks its time is under (like `MenuLayer::init`) and its functions, by name when the mod's PDB is around
- **Slow frames**: every frame over 50 ms, with the mod whose time it was and the function. In the table and in the log, with a stack

The top lines say how much of the time is in frames, between frames (a frame limiter waiting) and idle, and what the profiler itself costs.

## Find out what changed

- <cy>[set baseline]</c> keeps the session's numbers, even after a restart. The table then shows each mod's own time back then, red when it takes more now: before and after a mod update, with a mod turned off or on
- <cy>[reset]</c> starts the session again, <cy>[last 5 s]</c> switches to the whole session

## Look deeper

- <cy>Ctrl+Shift+O</c> saves the last 10 seconds of stacks for <cy>speedscope.app</c>, a flame graph of everything the game's main thread did
- With **Improved Logger**, the numbers are pinned in its console, and its marker key (<cy>Ctrl+Shift+M</c>) logs the profile since the last marker

## How

About once a millisecond the profiler stops the game's main thread for a few microseconds, reads its call stack and lets it go. While the thread is stopped it only reads: no memory is allocated, nothing is logged or locked, so it can't freeze the game. At the default settings a frame takes about 2.6% longer.

It checks itself in Geode's log once a second: how many samples, how long each stop took (usually 20 to 40 microseconds), and the mods taking the most.

Windows only.
