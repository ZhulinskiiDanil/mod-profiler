#include "Unwind.hpp"

#include <algorithm>
#include <cstring>

namespace
{
  enum Op : uint8_t
  {
    PUSH_NONVOL = 0,
    ALLOC_LARGE = 1,
    ALLOC_SMALL = 2,
    SET_FPREG = 3,
    SAVE_NONVOL = 4,
    SAVE_NONVOL_FAR = 5,
    // SAVE_XMM in version 1, EPILOG in version 2
    OP_6 = 6,
    // SAVE_XMM_FAR in version 1
    OP_7 = 7,
    SAVE_XMM128 = 8,
    SAVE_XMM128_FAR = 9,
    PUSH_MACHFRAME = 10,
  };

  constexpr uint8_t FLAG_CHAININFO = 0x4;
  // A chain longer than this is broken
  constexpr int MAX_CHAIN = 32;

  uint16_t read16(uint8_t const* at)
  {
    uint16_t value;
    std::memcpy(&value, at, sizeof(value));
    return value;
  }

  uint32_t read32(uint8_t const* at)
  {
    uint32_t value;
    std::memcpy(&value, at, sizeof(value));
    return value;
  }
} // namespace

unwind::Extent unwind::frameExtent(uint8_t const* image, size_t imageSize, uint32_t unwindRva, uint32_t offset) noexcept
{
  Extent extent;
  // Bytes the unwinding moves RSP up by, from the frame base, and the farthest saved register read
  uint64_t stack = 0;
  uint64_t saves = 0;
  bool primary = true;

  for (int chain = 0; chain < MAX_CHAIN; chain++)
  {
    if (static_cast<uint64_t>(unwindRva) + 4 > imageSize)
      return {};
    auto info = image + unwindRva;
    auto version = info[0] & 7;
    auto flags = info[0] >> 3;
    auto prologSize = info[1];
    auto count = info[2];
    auto frameRegister = info[3] & 0xF;
    auto frameOffset = (info[3] >> 4) * 16u;
    if (version != 1 && version != 2)
      return {};
    // The codes, rounded up to an even count, then the chained entry
    auto codesEnd = static_cast<uint64_t>(unwindRva) + 4 + ((count + 1) & ~1) * 2;
    if (codesEnd + ((flags & FLAG_CHAININFO) ? 12 : 0) > imageSize)
      return {};
    auto codes = info + 4;

    for (int i = 0; i < count;)
    {
      auto codeOffset = codes[i * 2];
      auto op = codes[i * 2 + 1] & 0xF;
      auto opInfo = codes[i * 2 + 1] >> 4;
      // In the prolog, the codes past where the thread is haven't run yet. Chained parents ran whole
      bool done = !primary || offset >= prologSize || codeOffset <= offset;

      int slots = 1;
      uint64_t moves = 0;
      uint64_t reads = 0;
      switch (op)
      {
      case PUSH_NONVOL:
        moves = 8;
        break;
      case ALLOC_LARGE:
        if (opInfo == 0)
        {
          slots = 2;
          if (i + 1 >= count)
            return {};
          moves = read16(codes + (i + 1) * 2) * 8ull;
        }
        else if (opInfo == 1)
        {
          slots = 3;
          if (i + 2 >= count)
            return {};
          moves = read32(codes + (i + 1) * 2);
        }
        else
          return {};
        break;
      case ALLOC_SMALL:
        moves = opInfo * 8ull + 8;
        break;
      case SET_FPREG:
        if (!done)
          break;
        // Unwinding takes RSP from the frame register here: what came before it in the list doesn't add up
        if (extent.usesFrameRegister || frameRegister == 0)
          return {};
        extent.usesFrameRegister = true;
        extent.frameRegister = static_cast<uint8_t>(frameRegister);
        extent.frameOffset = frameOffset;
        stack = 0;
        break;
      case SAVE_NONVOL:
        slots = 2;
        if (i + 1 >= count)
          return {};
        reads = read16(codes + (i + 1) * 2) * 8ull + 8;
        break;
      case SAVE_NONVOL_FAR:
        slots = 3;
        if (i + 2 >= count)
          return {};
        reads = read32(codes + (i + 1) * 2) + 8ull;
        break;
      case OP_6:
        if (version == 2)
          break;
        slots = 2;
        if (i + 1 >= count)
          return {};
        reads = read16(codes + (i + 1) * 2) * 16ull + 16;
        break;
      case OP_7:
        if (version == 2)
          return {};
        slots = 3;
        if (i + 2 >= count)
          return {};
        reads = read32(codes + (i + 1) * 2) + 16ull;
        break;
      case SAVE_XMM128:
        slots = 2;
        if (i + 1 >= count)
          return {};
        reads = read16(codes + (i + 1) * 2) * 16ull + 16;
        break;
      case SAVE_XMM128_FAR:
        slots = 3;
        if (i + 2 >= count)
          return {};
        reads = read32(codes + (i + 1) * 2) + 16ull;
        break;
      case PUSH_MACHFRAME:
        // The interrupted RIP and RSP come from the machine frame, there's no return address after it
        if (opInfo > 1)
          return {};
        moves = opInfo ? 48 : 40;
        break;
      default:
        return {};
      }

      if (done)
      {
        // A save in a chained parent is relative to its own frame, which is somewhere above this one
        saves = std::max(saves, (primary ? 0 : stack) + reads);
        stack += moves;
      }
      i += slots;
    }

    if (!(flags & FLAG_CHAININFO))
      break;
    // The parent's RUNTIME_FUNCTION: begin, end, unwind info
    unwindRva = read32(image + codesEnd + 8);
    primary = false;
    if (chain == MAX_CHAIN - 1)
      return {};
  }

  extent.ok = true;
  extent.bytes = std::max(saves, stack + 8);
  return extent;
}
