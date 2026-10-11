#pragma once

// One look at the main thread's stack, as the sampler writes it into the ring
#include <cstdint>

// A main thread in the graphics driver is 40 or more frames deep, and the mods are near the bottom
inline constexpr int MAX_FRAMES = 64;

enum SampleFlags : uint16_t
{
  // A frame was found by scanning the stack, after code without unwind info (a hook handler)
  SAMPLE_SCANNED = 1 << 0,
  // The walk stopped at MAX_FRAMES, the stack goes on further
  SAMPLE_TRUNCATED = 1 << 1,
  // GetThreadContext failed: no frames
  SAMPLE_NO_CONTEXT = 1 << 2,
};

struct Sample
{
  // QueryPerformanceCounter when the thread was suspended
  int64_t time = 0;
  // Ticks between SuspendThread and the end of ResumeThread
  uint32_t suspendTicks = 0;
  // The part of it spent waiting in GetThreadContext for the thread to really stop
  uint32_t contextTicks = 0;
  uint16_t depth = 0;
  uint16_t flags = 0;
  // frames[0] is where the thread was, the rest are return addresses, callers last
  uintptr_t frames[MAX_FRAMES] = {};
};
