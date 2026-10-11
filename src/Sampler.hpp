#pragma once

// The thread that suspends the main thread about every millisecond and writes its stack into the ring.
// Windows only. While the main thread is suspended it doesn't allocate, log or lock: it reads the
// thread's registers and stack, the current ModuleMap, and writes into a slot reserved beforehand
#include "ModuleMap.hpp"
#include "Ring.hpp"
#include "Sample.hpp"

#include <atomic>
#include <cstdint>

namespace sampler
{
  // Call on the main thread: duplicates its handle and notes where its stack is
  bool captureMainThread();

  // Starts sampling into `ring`. The map can change at any time, see `iteration`
  bool start(Ring<Sample>& ring, std::atomic<ModuleMap const*>& map, double intervalMillis);
  void stop();
  bool running();
  // The thread is still there: false once it finished its last sample after `stop`
  bool alive();

  // Goes up after every sample attempt. A map swapped out at iteration N isn't read anymore at N + 2
  uint64_t iteration();
  // Samples the ring had no room for
  uint64_t dropped();
  // The sampler thread's CPU time so far, in 100 ns units
  uint64_t cpuTime();
  // The calling thread's CPU time, in 100 ns units
  uint64_t threadCpuTime();
  void setInterval(double millis);

  // QueryPerformanceFrequency, for turning ticks into time
  int64_t ticksPerSecond();
  int64_t now();
} // namespace sampler
