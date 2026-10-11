# Mod Profiler

Shows how much of each frame every installed mod takes.

About once a millisecond the profiler stops the game's main thread for a few microseconds, reads its call stack and lets it go. Whichever mods have code on that stack get the sample. Once a second the numbers go to Geode's log:

- **Inclusive**: how often a mod's code is anywhere on the stack, the game code its hooks call included
- **Self**: how often the thread is in the mod's own code
- **ms a frame**: the inclusive share of an average frame

Geometry Dash, Cocos2d and the rest (Windows, the graphics driver) are counted apart from the mods.

The profiler checks itself: the log line says how long each stop took (usually 20 to 40 microseconds) and whether any sample was lost.

Windows only.
