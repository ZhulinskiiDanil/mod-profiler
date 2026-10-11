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

uint32_t unwind::functionBegin(uint8_t const* image, size_t imageSize, uint32_t begin, uint32_t unwindRva) noexcept
{
  for (int chain = 0; chain < MAX_CHAIN; chain++)
  {
    if (static_cast<uint64_t>(unwindRva) + 4 > imageSize)
      return begin;
    auto info = image + unwindRva;
    if (!((info[0] >> 3) & FLAG_CHAININFO))
      return begin;
    auto codesEnd = static_cast<uint64_t>(unwindRva) + 4 + ((info[2] + 1) & ~1) * 2;
    if (codesEnd + 12 > imageSize)
      return begin;
    begin = read32(image + codesEnd);
    unwindRva = read32(image + codesEnd + 8);
  }
  return begin;
}

int unwind::framePointerProlog(uint8_t const* image, size_t imageSize, uint32_t unwindRva) noexcept
{
  if (static_cast<uint64_t>(unwindRva) + 4 > imageSize)
    return -1;
  auto info = image + unwindRva;
  auto version = info[0] & 7;
  auto count = info[2];
  if ((version != 1 && version != 2) || count == 0 || static_cast<uint64_t>(unwindRva) + 4 + count * 2 > imageSize)
    return -1;
  // The codes are listed last first: the prolog's first instruction is the last code
  auto first = info + 4 + (count - 1) * 2;
  constexpr uint8_t RBP = 5;
  if ((first[1] & 0xF) != PUSH_NONVOL || (first[1] >> 4) != RBP)
    return -1;
  return info[1];
}

bool unwind::isFramePointerEpilog(uint8_t const* pc, size_t available) noexcept
{
  if (available == 0)
    return false;
  // ret, pop rbp
  if (pc[0] == 0xC3 || pc[0] == 0x5D)
    return true;
  // add rsp, imm32 / imm8
  return available >= 3 && pc[0] == 0x48 && (pc[1] == 0x81 || pc[1] == 0x83) && pc[2] == 0xC4;
}

uint64_t unwind::epilogReach(uint8_t const* pc, size_t available) noexcept
{
  size_t at = 0;
  uint64_t reach = 0;
  // add rsp, imm8 / imm32
  if (available >= 4 && pc[0] == 0x48 && pc[1] == 0x83 && pc[2] == 0xC4)
  {
    reach = pc[3];
    at = 4;
  }
  else if (available >= 7 && pc[0] == 0x48 && pc[1] == 0x81 && pc[2] == 0xC4)
  {
    reach = read32(pc + 3);
    at = 7;
  }

  // Pops: a pop in the middle of a function (no ret or jmp after it) isn't an epilog, and is
  // unwound with the unwind info, which must be checked then
  for (int pops = 0; pops <= 8 && at < available; pops++)
  {
    auto byte = pc[at];
    bool ret = byte == 0xC3 || (byte == 0xF3 && at + 1 < available && pc[at + 1] == 0xC3);
    // jmp rel32, jmp [rip + disp32], with REX.W too: a tail call
    bool jmp = byte == 0xE9 || (byte == 0xFF && at + 1 < available && pc[at + 1] == 0x25) ||
               (byte == 0x48 && at + 2 < available && pc[at + 1] == 0xFF && pc[at + 2] == 0x25);
    if (ret || jmp)
      return reach + 8;
    if (byte >= 0x58 && byte <= 0x5F)
      at += 1;
    else if (byte == 0x41 && at + 1 < available && pc[at + 1] >= 0x58 && pc[at + 1] <= 0x5F)
      at += 2;
    else
      return 0;
    reach += 8;
  }
  return 0;
}

bool unwind::isEpilogTail(uint8_t const* pc, size_t available) noexcept
{
  bool add = available >= 3 && pc[0] == 0x48 && (pc[1] == 0x83 || pc[1] == 0x81) && pc[2] == 0xC4;
  return !add && epilogReach(pc, available) > 0;
}

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
