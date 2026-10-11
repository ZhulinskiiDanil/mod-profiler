# How Mod Profiler works

A map of the code and the reasons behind the parts that aren't obvious.

## The path of a sample

1. **Sampler.cpp**: a thread wakes up about every millisecond (a high resolution waitable timer, no change to the system timer). It reserves a slot in the ring, then `SuspendThread` on the main thread, `GetThreadContext`, walks the stack into the slot, `ResumeThread`.
2. **Ring.hpp**: the slot is committed. The ring is single writer, single reader, made once: no allocations or locks after that.
3. **Profiler.cpp**: once a second the analyzer thread empties the ring. Each sample's addresses go to buckets through the **ModuleMap**, the **Profile** counts them (inclusive once a sample, self by the first frame), and the diagnostics line goes to the log.
4. The second is merged into the session, and a copy of both is kept for whoever asks (`profiler::snapshot`).

## Files

| File | What it does |
| --- | --- |
| `main.cpp` | Start on the main thread, the mods list for the buckets, the settings, the frame counter |
| `Sampler.cpp` | The sampler thread and the stack walk |
| `Profiler.cpp` | The analyzer thread: module maps, the per-second windows, the log line |
| `Modules.cpp` | Builds a ModuleMap from the loaded modules: code sections, function tables, buckets, pinning |
| `ModuleMap.cpp` | Address to module and bucket, binary searches, the function table lookup |
| `Unwind.cpp` | How far up the stack unwinding a frame reads, from its unwind codes |
| `Code.cpp` | Whether a value on the stack is a return address (a call right before it) |
| `Profile.cpp` | Counting samples into buckets, shares, the diagnostics line |
| `Ring.hpp`, `Sample.hpp` | The ring and what goes into it |

`Ring`, `ModuleMap`, `Unwind`, `Code` and `Profile` include neither Windows nor Geode, so `tests/` builds them alone (CI runs them with GCC too).

## Decisions worth knowing

- **While the main thread is suspended, the sampler only reads.** If it allocated, logged or locked, it could wait for a lock the suspended thread holds, and the game would freeze for good. Everything the walk needs is made beforehand: the slot, the map, the bounds of the stack.
- **The stack is walked with the profiler's own copy of the function tables, not `RtlLookupFunctionEntry`.** Geode's hooks (TulipHook) register their generated code with `RtlInstallFunctionTableCallback`, and that callback can build a `std::stringstream` for its log: a heap lock. The map has every loaded module's `.pdata` and code sections, read outside the suspension. The walk does a binary search and calls `RtlVirtualUnwind`, which only reads the unwind codes and the stack.
- **Unwind info is checked before `RtlVirtualUnwind` runs.** It trusts the stack: on a frame that makes no sense it reads wherever the codes point. `unwind::frameExtent` works out how far above the frame the unwinding reads, and the walk stops if that leaves the stack. An access violation is caught too, but only as a last net: it goes through every mod's vectored exception handlers on the sampler thread, and one of them could lock.
- **Code without unwind info is crossed by scanning.** A hook handler or a trampoline isn't in any module. The walk looks up the stack for the next value that points right after a `call` in a known module, and goes on from there. Such samples count as *scanned*.
- **Modules in a map are pinned** with `GetModuleHandleEx`, so none can be unloaded while the sampler reads its tables. A map that was swapped out is freed only once the sampler has started two more samples, so it can't still be reading it.
- **Frames are counted from a scheduler callback, not a hook.** A hook on the frame would put this mod on every stack under it.
- **A late wake-up doesn't make the next ones come sooner.** Catching up would sample the same moment several times.

## Building and trying

```sh
geode build                     # installs into the game
cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
```

Turn on **Stacks in the log** to see what the walk found, as `module+offset`.
