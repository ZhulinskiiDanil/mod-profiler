#pragma once

// Reading x86-64 code, without Windows or Geode in it
#include <cstddef>
#include <cstdint>

namespace code
{
  // Whether the bytes right before `returnAddress` end with a call instruction, so a value on the stack
  // pointing there is likely a real return address. `available` is how many bytes before it can be read
  bool isCallBefore(uint8_t const* returnAddress, size_t available) noexcept;
} // namespace code
