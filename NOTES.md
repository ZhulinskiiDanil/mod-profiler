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

## Log
