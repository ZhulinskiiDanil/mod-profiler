// Tests for src/Code.cpp: telling a return address from any other number on the stack
#include "../src/Code.hpp"
#include "Check.hpp"

#include <cstring>
#include <initializer_list>

// The bytes are put at the end of a buffer of filler, and the "return address" is right after them
static bool callEnds(std::initializer_list<uint8_t> bytes, uint8_t filler = 0x90)
{
  uint8_t buffer[32];
  std::memset(buffer, filler, sizeof(buffer));
  auto end = buffer + sizeof(buffer);
  auto start = end - bytes.size();
  std::memcpy(start, bytes.begin(), bytes.size());
  return code::isCallBefore(end, sizeof(buffer));
}

// ! --- Calls --- !

static void testRelativeCall()
{
  // call rel32
  CHECK(callEnds({0xE8, 0x10, 0x20, 0x30, 0x40}));
}

static void testRegisterCalls()
{
  // call rax, call rbx
  CHECK(callEnds({0xFF, 0xD0}));
  CHECK(callEnds({0xFF, 0xD3}));
  // call r8
  CHECK(callEnds({0x41, 0xFF, 0xD0}));
}

static void testMemoryCalls()
{
  // call [rip + disp32], the usual import call
  CHECK(callEnds({0xFF, 0x15, 0x01, 0x02, 0x03, 0x04}));
  // call [rax]
  CHECK(callEnds({0xFF, 0x10}));
  // call [rax + disp8], a virtual call
  CHECK(callEnds({0xFF, 0x50, 0x18}));
  // call [rax + disp32]
  CHECK(callEnds({0xFF, 0x90, 0x00, 0x01, 0x00, 0x00}));
  // call [rsp + disp8] needs a SIB byte
  CHECK(callEnds({0xFF, 0x54, 0x24, 0x20}));
  // call [r10 + disp8]
  CHECK(callEnds({0x41, 0xFF, 0x52, 0x08}));
  // call qword ptr [rax*8 + disp32] (SIB with no base)
  CHECK(callEnds({0xFF, 0x14, 0xC5, 0x00, 0x10, 0x00, 0x00}));
  // with REX.W
  CHECK(callEnds({0x48, 0xFF, 0x14, 0xC5, 0x00, 0x10, 0x00, 0x00}));
}

// ! --- Not calls --- !

static void testNotCalls()
{
  // nops
  CHECK(!callEnds({0x90, 0x90}));
  // jmp rax (FF /4)
  CHECK(!callEnds({0xFF, 0xE0}, 0xCC));
  // jmp [rip + disp32] (FF /4)
  CHECK(!callEnds({0xFF, 0x25, 0x01, 0x02, 0x03, 0x04}, 0xCC));
  // push [rax] (FF /6)
  CHECK(!callEnds({0xFF, 0x30}, 0xCC));
  // mov eax, imm32 ends where a call rel32 would, but starts with B8
  CHECK(!callEnds({0xB8, 0x01, 0x02, 0x03, 0x04}, 0xCC));
  // A call whose length doesn't reach the end: call [rax + disp8] followed by one more byte
  CHECK(!callEnds({0xFF, 0x50, 0x18, 0xC3}, 0xCC));
}

static void testNotEnoughBytes()
{
  uint8_t bytes[] = {0xE8, 0, 0, 0, 0};
  // Only 4 bytes are known to be readable: the E8 five bytes back isn't looked at
  CHECK(!code::isCallBefore(bytes + 5, 4));
  CHECK(code::isCallBefore(bytes + 5, 5));
  uint8_t reg[] = {0xFF, 0xD0};
  CHECK(!code::isCallBefore(reg + 2, 1));
  CHECK(code::isCallBefore(reg + 2, 2));
  CHECK(!code::isCallBefore(reg + 2, 0));
}

int main()
{
  testRelativeCall();
  testRegisterCalls();
  testMemoryCalls();
  testNotCalls();
  testNotEnoughBytes();
  return finish("code");
}
