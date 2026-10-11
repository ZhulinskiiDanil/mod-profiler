#include "Code.hpp"

namespace
{
  // The length of `FF /r` from its ModRM byte on: ModRM, SIB and displacement
  size_t operandLength(uint8_t const* modrm) noexcept
  {
    auto mod = *modrm >> 6;
    auto rm = *modrm & 7;
    if (mod == 3)
      return 1;
    size_t length = 1;
    if (rm == 4)
    {
      length++;
      auto base = modrm[1] & 7;
      if (mod == 0 && base == 5)
        length += 4;
    }
    else if (mod == 0 && rm == 5)
      // [rip + disp32]
      length += 4;
    if (mod == 1)
      length += 1;
    else if (mod == 2)
      length += 4;
    return length;
  }

  bool isRex(uint8_t byte) noexcept { return (byte & 0xF0) == 0x40; }
} // namespace

bool code::isCallBefore(uint8_t const* returnAddress, size_t available) noexcept
{
  // call rel32
  if (available >= 5 && returnAddress[-5] == 0xE8)
    return true;

  // call r/m64 (FF /2), maybe after a REX prefix: 2 to 8 bytes long
  for (size_t length = 2; length <= 8 && length <= available; length++)
  {
    auto start = returnAddress - length;
    auto opcode = start;
    if (isRex(*opcode))
      opcode++;
    if (*opcode != 0xFF || opcode + 1 >= returnAddress)
      continue;
    auto modrm = opcode + 1;
    if (((*modrm >> 3) & 7) != 2)
      continue;
    // The SIB byte has to be inside the instruction too
    if ((*modrm >> 6) != 3 && (*modrm & 7) == 4 && modrm + 1 >= returnAddress)
      continue;
    if (modrm + operandLength(modrm) == returnAddress)
      return true;
  }
  return false;
}
