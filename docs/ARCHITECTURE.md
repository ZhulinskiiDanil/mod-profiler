# How Mod Profiler works

A map of the code and the reasons behind the parts that aren't obvious.

## The path of a sample

1. **Sampler.cpp**: a thread wakes up about every millisecond (a high resolution waitable timer, no change to the system timer). It reserves a slot in the ring, then `SuspendThread` on the main thread, `GetThreadContext`, walks the stack into the slot, `ResumeThread`.
2. **Ring.hpp**: the slot is committed. The ring is single writer, single reader, made once: no allocations or locks after that.
3. **Profiler.cpp**: once a second the analyzer thread empties the ring. Each sample's addresses go to buckets through the **ModuleMap**. **Attribution** decides whose own time it is, if anyone's, and the **Profile** counts it: inclusive once a sample for every mod on the stack, self by the first frame, own for one mod at most.
4. The second is logged, kept with the last 4 for the table, and merged into the session. `profiler::snapshot` hands a copy to the main thread.
5. **Overlay.cpp** asks for the snapshot twice a second while the table is shown.

## Files

| File | What it does |
| --- | --- |
| `main.cpp` | Start on the main thread, the mods and their hooks, the game's loop functions, the settings, the frame counter |
| `Sampler.cpp` | The sampler thread and the stack walk |
| `Profiler.cpp` | The analyzer thread: module maps, learning generated code, windows of samples, the log line, names |
| `Modules.cpp` | Builds a ModuleMap from the loaded modules and the learned generated code, pins modules, the wait functions |
| `Symbols.cpp` | Function names with DbgHelp |
| `Overlay.cpp` | The table in the game |
| `ModuleMap.cpp` | Address to module and bucket, binary searches, function tables, where a function starts |
| `Attribution.cpp` | Whose own time a sample is, idle, between frames |
| `Unwind.cpp` | How far up the stack unwinding a frame reads, where a split function starts |
| `Code.cpp` | Whether a value on the stack is a return address (a call right before it) |
| `Profile.cpp` | Counting samples into buckets and places, shares, the diagnostics line |
| `Ring.hpp`, `Sample.hpp` | The ring and what goes into it |

`Ring`, `ModuleMap`, `Attribution`, `Unwind`, `Code` and `Profile` include neither Windows nor Geode, so `tests/` builds them alone (CI runs them with GCC too).

## Decisions worth knowing

- **While the main thread is suspended, the sampler only reads.** If it allocated, logged or locked, it could wait for a lock the suspended thread holds, and the game would freeze for good. Everything the walk needs is made beforehand: the slot, the map, the bounds of the stack.
- **The stack is walked with the profiler's own copy of the function tables, not `RtlLookupFunctionEntry`.** Geode's hooks (TulipHook) register their generated code with `RtlInstallFunctionTableCallback`, and that callback can build a `std::stringstream` for its log: a heap lock. The map has every loaded module's `.pdata` and code sections, read outside the suspension. The walk does a binary search and calls `RtlVirtualUnwind`, which only reads the unwind codes and the stack.
- **Generated code is learned.** An address outside every module in a sample (a hook handler) is looked up with `RtlLookupFunctionEntry` later, on the main thread: TulipHook's callback reads a `std::map` that only the main thread writes to, when it makes a hook. The function's entry goes into the next map as a module of its own, in the *Hook handlers* bucket, and the walk unwinds through it exactly from then on.
- **Unwind info is checked before `RtlVirtualUnwind` runs.** It trusts the stack: on a frame that makes no sense it reads wherever the codes point. `unwind::frameExtent` works out how far above the frame the unwinding reads, and the walk stops if that leaves the stack. An access violation is caught too, but only as a last net: it goes through every mod's vectored exception handlers on the sampler thread, and one of them could lock.
- **Code without unwind info is crossed by scanning.** Code that isn't learned yet (or never can be, like another library's trampoline) is passed by looking up the stack for the next value that points right after a `call` in a known module. Such samples count as *scanned*.
- **Modules in a map are pinned** with `GetModuleHandleEx`, so none can be unloaded while the sampler reads its tables. A map that was swapped out is freed only once the sampler has started two more samples, so it can't still be reading it.
- **Own time is the innermost mod doing its own work.** A mod's frame passes the call on when what it calls (past the hook handlers) starts at a function it hooks, or at another mod's detour of one: it's calling the original. Then the work under it is someone else's, and the next mod frame out is looked at. A frame called straight from the game that calls straight into it is a patched call (Geode's start under the game's main function) and passes on too. A mod's function called by a hook handler that isn't a detour is the hook library's bookkeeping. The hooks come from `Mod::getHooks()`, their detours from `getRuntimeInfo()`.
- **Idle and between frames aren't anyone's.** A sample in the system call stub of a wait function is idle. A sample in `CCApplication::run` but not in `CCDirector::mainLoop` or `CCEGLView::pollEvents` (or a detour of them) is between frames. Mega Hack's frame limiter spins on `QueryPerformanceCounter` there: before this it was 90% of every list. `mainLoop` isn't exported, it's read from the director's vtable.
- **Frames are counted from a scheduler callback, not a hook.** A hook on the frame would put this mod on every stack under it.
- **A late wake-up doesn't make the next ones come sooner.** Catching up would sample the same moment several times.
- **DbgHelp gets a session handle of its own.** Geode's crash handler calls `SymInitialize` with the process handle: a session on the same handle would break its stack traces. Modules are loaded from their files with the base they're at, PDBs found where they were built.

## Building and trying

```sh
geode build                     # installs into the game
cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
```

Turn on **Stacks in the log** to see what the walk found, as `module+offset`.
