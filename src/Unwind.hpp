#pragma once

// Reading x64 unwind info ahead of RtlVirtualUnwind, without Windows or Geode in it.
// RtlVirtualUnwind trusts the stack: on a frame that makes no sense it reads wherever the codes point.
// The sampler runs it while the main thread is suspended, where an access violation would go through
// every mod's exception handlers, so it first checks with `frameExtent` that the reads stay in the stack
#include <cstddef>
#include <cstdint>

namespace unwind
{
  struct Extent
  {
    // False when the info can't be read or has codes this doesn't know: the walk stops there
    bool ok = false;
    // The frame is addressed from a register (UWOP_SET_FPREG already ran): it's that register's
    // value minus `frameOffset`, not RSP
    bool usesFrameRegister = false;
    uint8_t frameRegister = 0;
    uint32_t frameOffset = 0;
    // How far above the frame base the unwinding reads, the return address included
    uint64_t bytes = 0;
  };

  // The image is `imageSize` bytes from `image`. `unwindRva` is the function's UNWIND_INFO,
  // `offset` is how far into the function the address is (a frame in its prolog has done only part of it)
  Extent frameExtent(uint8_t const* image, size_t imageSize, uint32_t unwindRva, uint32_t offset) noexcept;
} // namespace unwind
