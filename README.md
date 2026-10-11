# Mod Profiler

A [Geode](https://geode-sdk.org) mod for Geometry Dash that shows how much of every frame each installed mod takes. It's a sampling profiler: about once a millisecond it stops the game's main thread for a few microseconds, reads its call stack and lets it go. Whichever mods have code on that stack get the sample. Windows only.

<img src="logo.png" width="128" alt="logo">

## What it measures

Once a second the profiler adds up the last second of samples:

- **Inclusive**: a mod counts once for every sample where its code is anywhere on the stack. This includes the game code its hooks call. A mod hooking the whole frame (a menu that draws over the game, an FPS limiter) is close to 100% here.
- **Self**: the samples where the thread was in the mod's own code right then.
- **ms a frame**: the inclusive share times the average frame time, so it's comparable between 60 and 240 fps.

Geometry Dash itself, `libcocos2d.dll` and everything else (Windows, the graphics driver) get their own buckets.

The self-diagnostics line in Geode's log, once a second:

```
992 samples in 0.99 s (258 scanned), suspended avg 26.5 us, max 224.9 us (stopping avg 16.7 us, max 202.3 us), 199 fps | self: Geometry Dash 0.9%, Cocos2d 11.4%, Other 70.5%, mods 17.2% | top: geode.loader 89.4% (4.48 ms), ...
```

- **scanned**: samples where the stack went through code without unwind info (hook handlers) and the rest of it was found by scanning
- **suspended**: how long the main thread stood still for one sample; **stopping** is the part spent waiting for Windows to really stop it

## Settings

| Setting | Default | What it does |
| --- | --- | --- |
| Sampling | on | Off: the profiler does nothing |
| Sample every (ms) | 1 | Less is more precise and costs more |
| Diagnostics in the log | on | The line above, once a second |
| Stacks in the log | off | A few sampled stacks as `module+offset`, for checking the profiler itself |

## Build

```sh
geode build
cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
```

Needs the Geode SDK (`GEODE_SDK`) and its CLI. How the parts fit together, and why some are the way they are: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
