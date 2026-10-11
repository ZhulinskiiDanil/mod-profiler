// Tests for src/Unwind.cpp: how far up the stack unwinding a frame reads
#include "../src/Unwind.hpp"
#include "Check.hpp"

#include <cstring>
#include <initializer_list>
#include <vector>

// An image of zeros with UNWIND_INFO written at `rva`
struct Image
{
  std::vector<uint8_t> bytes = std::vector<uint8_t>(0x1000, 0);

  // `codes` are UNWIND_CODE slots: (offset, op | info << 4) pairs, or raw 16-bit values for the extra slots
  void info(uint32_t rva, uint8_t version, uint8_t flags, uint8_t prolog, uint8_t frameRegister, uint8_t frameOffset,
            std::initializer_list<uint16_t> slots)
  {
    auto at = bytes.data() + rva;
    at[0] = static_cast<uint8_t>(version | flags << 3);
    at[1] = prolog;
    at[2] = static_cast<uint8_t>(slots.size());
    at[3] = static_cast<uint8_t>(frameRegister | frameOffset << 4);
    int i = 0;
    for (auto slot : slots)
      std::memcpy(at + 4 + 2 * i++, &slot, 2);
  }

  // The chained RUNTIME_FUNCTION after `count` codes of the info at `rva`
  void chain(uint32_t rva, uint8_t count, uint32_t parent)
  {
    auto at = bytes.data() + rva + 4 + ((count + 1) & ~1) * 2;
    uint32_t entry[3] = {0, 0, parent};
    std::memcpy(at, entry, sizeof(entry));
  }

  unwind::Extent extent(uint32_t rva, uint32_t offset) const
  {
    return unwind::frameExtent(bytes.data(), bytes.size(), rva, offset);
  }
};

// One UNWIND_CODE slot
static uint16_t code(uint8_t offset, uint8_t op, uint8_t info = 0) { return static_cast<uint16_t>(offset | (op | info << 4) << 8); }

constexpr uint8_t RBX = 3, RBP = 5, RSI = 6, RDI = 7;

// ! --- Frames --- !

static void testLeafLikeFrame()
{
  // No codes at all: just the return address
  Image image;
  image.info(0x100, 1, 0, 0, 0, 0, {});
  auto extent = image.extent(0x100, 0);
  CHECK(extent.ok);
  CHECK(extent.bytes == 8);
  CHECK(!extent.usesFrameRegister);
}

static void testPushesAndAlloc()
{
  // push rbx; push rsi; sub rsp, 0x28 -> listed last first
  Image image;
  image.info(0x100, 1, 0, 6, 0, 0, {code(6, 2, 4), code(2, 0, RSI), code(1, 0, RBX)});
  auto extent = image.extent(0x100, 0x40);
  CHECK(extent.ok);
  // 0x28 + 8 + 8 + the return address
  CHECK(extent.bytes == 0x28 + 16 + 8);
}

static void testInsideProlog()
{
  // Same frame, the thread right after the first push: only it is undone
  Image image;
  image.info(0x100, 1, 0, 6, 0, 0, {code(6, 2, 4), code(2, 0, RSI), code(1, 0, RBX)});
  CHECK(image.extent(0x100, 1).bytes == 8 + 8);
  // Before any of the prolog
  CHECK(image.extent(0x100, 0).bytes == 8);
}

static void testLargeAllocs()
{
  Image image;
  // sub rsp, 0x1000 (scaled by 8 in one extra slot)
  image.info(0x100, 1, 0, 7, 0, 0, {code(7, 1, 0), 0x1000 / 8});
  CHECK(image.extent(0x100, 0x10).bytes == 0x1000 + 8);
  // sub rsp, 0x123458 (unscaled, two extra slots)
  image.info(0x200, 1, 0, 7, 0, 0, {code(7, 1, 1), 0x3458, 0x0012});
  CHECK(image.extent(0x200, 0x10).bytes == 0x123458 + 8);
}

static void testFramePointer()
{
  // push rbp; sub rsp, 0x40; lea rbp, [rsp + 0x20]: RSP comes back from RBP - 0x20
  Image image;
  image.info(0x100, 1, 0, 10, RBP, 2, {code(10, 3), code(5, 2, 7), code(1, 0, RBP)});
  auto extent = image.extent(0x100, 0x20);
  CHECK(extent.ok);
  CHECK(extent.usesFrameRegister);
  CHECK(extent.frameRegister == RBP);
  CHECK(extent.frameOffset == 0x20);
  // From the frame base: the 0x40 and the push, then the return address
  CHECK(extent.bytes == 0x40 + 8 + 8);
  // Before the lea ran, the frame is still RSP-based
  auto early = image.extent(0x100, 6);
  CHECK(!early.usesFrameRegister);
  CHECK(early.bytes == 0x40 + 8 + 8);
}

static void testFramePointerSkipsLaterAllocs()
{
  // mov rbp, rsp right after the push, then a big sub rsp: RSP comes back from RBP, the alloc doesn't count
  Image image;
  image.info(0x100, 1, 0, 11, RBP, 0, {code(11, 1, 0), 0x8000 / 8, code(4, 3), code(1, 0, RBP)});
  auto extent = image.extent(0x100, 0x40);
  CHECK(extent.ok && extent.usesFrameRegister);
  CHECK(extent.bytes == 8 + 8);
}

static void testSaves()
{
  // sub rsp, 0x28; mov [rsp + 0x40], rdi: the save is above the frame
  Image image;
  image.info(0x100, 1, 0, 9, 0, 0, {code(9, 4, RDI), 0x40 / 8, code(4, 2, 4)});
  auto extent = image.extent(0x100, 0x20);
  CHECK(extent.ok);
  CHECK(extent.bytes == 0x48);
  // movaps [rsp + 0x10], xmm6 with a 0x28 frame stays inside it
  image.info(0x200, 1, 0, 9, 0, 0, {code(9, 8, 6), 1, code(4, 2, 4)});
  CHECK(image.extent(0x200, 0x20).bytes == 0x28 + 8);
}

static void testMachineFrame()
{
  Image image;
  image.info(0x100, 1, 0, 0, 0, 0, {code(0, 10, 1)});
  auto extent = image.extent(0x100, 0);
  CHECK(extent.ok);
  CHECK(extent.bytes >= 48);
}

static void testChained()
{
  // The child allocates 0x20, its parent pushed two registers
  Image image;
  image.info(0x100, 1, 4 /* chained */, 4, 0, 0, {code(4, 2, 3)});
  image.chain(0x100, 1, 0x200);
  image.info(0x200, 1, 0, 2, 0, 0, {code(2, 0, RSI), code(1, 0, RBX)});
  auto extent = image.extent(0x100, 0x10);
  CHECK(extent.ok);
  CHECK(extent.bytes == 0x20 + 16 + 8);
  // In the child's prolog the parent still counts whole
  CHECK(image.extent(0x100, 0).bytes == 16 + 8);
}

static void testChainedFramePointer()
{
  // A shrink-wrapped function: the frame pointer is set up in the parent
  Image image;
  image.info(0x100, 1, 4, 0, 0, 0, {});
  image.chain(0x100, 0, 0x200);
  image.info(0x200, 1, 0, 4, RBP, 0, {code(4, 3), code(1, 0, RBP)});
  auto extent = image.extent(0x100, 0x10);
  CHECK(extent.ok && extent.usesFrameRegister);
  CHECK(extent.frameRegister == RBP);
  CHECK(extent.bytes == 16);
}

// ! --- Function starts --- !

static void testFunctionBegin()
{
  // A part at 0x500 chained to its function at 0x100, which isn't chained
  Image image;
  image.info(0x200, 1, 0, 0, 0, 0, {});
  image.info(0x300, 1, 4, 0, 0, 0, {code(0, 2, 1)});
  image.chain(0x300, 1, 0x200);
  // chain() writes begin 0: put the parent's begin in
  uint32_t parentBegin = 0x100;
  std::memcpy(image.bytes.data() + 0x300 + 4 + 2 * 2, &parentBegin, 4);
  CHECK(unwind::functionBegin(image.bytes.data(), image.bytes.size(), 0x500, 0x300) == 0x100);
  // Not chained: its own begin
  CHECK(unwind::functionBegin(image.bytes.data(), image.bytes.size(), 0x100, 0x200) == 0x100);
  // Broken: the begin it was given
  CHECK(unwind::functionBegin(image.bytes.data(), image.bytes.size(), 0x700, 0xFFFF) == 0x700);
}

// ! --- Generated code's frame pointer --- !

static void testFramePointerProlog()
{
  // TulipHook's: push rbp, then sub rsp 0xc0 (listed last first), prolog 8 bytes
  Image image;
  image.info(0x100, 1, 0, 8, 0, 0, {code(8, 1, 0), 0xc0 / 8, code(1, 0, RBP)});
  CHECK(unwind::framePointerProlog(image.bytes.data(), image.bytes.size(), 0x100) == 8);
  // Starts with push rbx: no
  image.info(0x200, 1, 0, 2, 0, 0, {code(2, 0, RBP), code(1, 0, RBX)});
  CHECK(unwind::framePointerProlog(image.bytes.data(), image.bytes.size(), 0x200) == -1);
  // No codes, broken
  image.info(0x300, 1, 0, 0, 0, 0, {});
  CHECK(unwind::framePointerProlog(image.bytes.data(), image.bytes.size(), 0x300) == -1);
  CHECK(unwind::framePointerProlog(image.bytes.data(), image.bytes.size(), 0xFFFF) == -1);
}

static void testFramePointerEpilog()
{
  uint8_t ret[] = {0xC3};
  uint8_t pop[] = {0x5D, 0xC3};
  uint8_t add[] = {0x48, 0x81, 0xC4, 0xC0, 0x00, 0x00, 0x00};
  uint8_t addSmall[] = {0x48, 0x83, 0xC4, 0x28};
  uint8_t call[] = {0xFF, 0xD0};
  CHECK(unwind::isFramePointerEpilog(ret, 1));
  CHECK(unwind::isFramePointerEpilog(pop, 2));
  CHECK(unwind::isFramePointerEpilog(add, 7));
  CHECK(unwind::isFramePointerEpilog(addSmall, 4));
  CHECK(!unwind::isFramePointerEpilog(call, 2));
  CHECK(!unwind::isFramePointerEpilog(add, 2));
  CHECK(!unwind::isFramePointerEpilog(ret, 0));
}

static void testEpilogTail()
{
  uint8_t ret[] = {0xC3};
  uint8_t popRbpRet[] = {0x5D, 0xC3};
  uint8_t pops[] = {0x41, 0x5F, 0x41, 0x5E, 0x5F, 0x5E, 0x5D, 0xC3};
  uint8_t popThenWork[] = {0x58, 0x48, 0x89, 0xC1};
  uint8_t add[] = {0x48, 0x83, 0xC4, 0x28, 0xC3};
  uint8_t rex[] = {0x41, 0x50, 0xC3};
  CHECK(unwind::isEpilogTail(ret, 1));
  CHECK(unwind::isEpilogTail(popRbpRet, 2));
  CHECK(unwind::isEpilogTail(pops, sizeof(pops)));
  // A pop in the middle of a function
  CHECK(!unwind::isEpilogTail(popThenWork, sizeof(popThenWork)));
  // Before the pops: the frame register still holds
  CHECK(!unwind::isEpilogTail(add, sizeof(add)));
  // push r8, not a pop
  CHECK(!unwind::isEpilogTail(rex, sizeof(rex)));
  // The ret isn't in the readable bytes
  CHECK(!unwind::isEpilogTail(popRbpRet, 1));
  CHECK(!unwind::isEpilogTail(ret, 0));
  // Ending with a tail call, or rep ret
  uint8_t popJmp[] = {0x5D, 0xE9, 0, 0, 0, 0};
  uint8_t repRet[] = {0x5D, 0xF3, 0xC3};
  uint8_t jmpRip[] = {0x5D, 0x48, 0xFF, 0x25, 0, 0, 0, 0};
  CHECK(unwind::isEpilogTail(popJmp, sizeof(popJmp)));
  CHECK(unwind::isEpilogTail(repRet, sizeof(repRet)));
  CHECK(unwind::isEpilogTail(jmpRip, sizeof(jmpRip)));
}

static void testEpilogReach()
{
  // What's read from RSP: the add's bytes, 8 a pop, the return address
  uint8_t ret[] = {0xC3};
  uint8_t pops[] = {0x41, 0x5F, 0x5E, 0x5D, 0xC3};
  uint8_t addSmall[] = {0x48, 0x83, 0xC4, 0x28, 0x5F, 0xC3};
  uint8_t addLarge[] = {0x48, 0x81, 0xC4, 0xC0, 0x00, 0x00, 0x00, 0x5D, 0xC3};
  uint8_t notEpilog[] = {0x48, 0x83, 0xC4, 0x28, 0x48, 0x89, 0xC1};
  CHECK(unwind::epilogReach(ret, sizeof(ret)) == 8);
  CHECK(unwind::epilogReach(pops, sizeof(pops)) == 3 * 8 + 8);
  CHECK(unwind::epilogReach(addSmall, sizeof(addSmall)) == 0x28 + 8 + 8);
  CHECK(unwind::epilogReach(addLarge, sizeof(addLarge)) == 0xC0 + 8 + 8);
  CHECK(unwind::epilogReach(notEpilog, sizeof(notEpilog)) == 0);
  // An add before the pops isn't the tail yet
  CHECK(!unwind::isEpilogTail(addSmall, sizeof(addSmall)));
}

// ! --- Broken info --- !

static void testBroken()
{
  Image image;
  // Version 3 doesn't exist
  image.info(0x100, 3, 0, 0, 0, 0, {});
  CHECK(!image.extent(0x100, 0).ok);
  // Past the end of the image
  CHECK(!image.extent(0xFFE, 0).ok);
  CHECK(!image.extent(0x10000, 0).ok);
  // An ALLOC_LARGE without its extra slot
  image.info(0x200, 1, 0, 4, 0, 0, {code(4, 1, 0)});
  CHECK(!image.extent(0x200, 0x10).ok);
  // An unknown op
  image.info(0x300, 1, 0, 4, 0, 0, {code(4, 15, 0)});
  CHECK(!image.extent(0x300, 0x10).ok);
  // SET_FPREG with no frame register
  image.info(0x400, 1, 0, 4, 0, 0, {code(4, 3)});
  CHECK(!image.extent(0x400, 0x10).ok);
  // A chain that points at itself never ends
  image.info(0x500, 1, 4, 0, 0, 0, {});
  image.chain(0x500, 0, 0x500);
  CHECK(!image.extent(0x500, 0).ok);
}

int main()
{
  testLeafLikeFrame();
  testPushesAndAlloc();
  testInsideProlog();
  testLargeAllocs();
  testFramePointer();
  testFramePointerSkipsLaterAllocs();
  testSaves();
  testMachineFrame();
  testChained();
  testChainedFramePointer();
  testFunctionBegin();
  testFramePointerProlog();
  testFramePointerEpilog();
  testEpilogTail();
  testEpilogReach();
  testBroken();
  return finish("unwind");
}
