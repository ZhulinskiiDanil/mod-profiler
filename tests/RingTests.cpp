// Tests for src/Ring.hpp: the sampler's ring between two threads
#include "../src/Ring.hpp"
#include "Check.hpp"

#include <cstdint>
#include <thread>

// ! --- One thread --- !

static void testCapacity()
{
  CHECK(Ring<int>(1).capacity() == 1);
  CHECK(Ring<int>(5).capacity() == 8);
  CHECK(Ring<int>(4096).capacity() == 4096);
}

static void testEmpty()
{
  Ring<int> ring(4);
  CHECK(ring.peek() == nullptr);
  CHECK(ring.size() == 0);
}

static void testOrder()
{
  Ring<int> ring(4);
  for (int i = 0; i < 3; i++)
  {
    auto slot = ring.reserve();
    CHECK(slot != nullptr);
    *slot = i;
    ring.commit();
  }
  CHECK(ring.size() == 3);
  for (int i = 0; i < 3; i++)
  {
    auto slot = ring.peek();
    CHECK(slot != nullptr && *slot == i);
    ring.pop();
  }
  CHECK(ring.peek() == nullptr);
}

static void testFull()
{
  Ring<int> ring(4);
  for (int i = 0; i < 4; i++)
  {
    *ring.reserve() = i;
    ring.commit();
  }
  // No room: the writer drops instead of overwriting what the reader hasn't seen
  CHECK(ring.reserve() == nullptr);
  ring.pop();
  CHECK(ring.reserve() != nullptr);
}

static void testReserveWithoutCommit()
{
  // A reserved slot the writer gave up on (the thread couldn't be suspended) isn't seen by the reader
  Ring<int> ring(4);
  *ring.reserve() = 7;
  CHECK(ring.peek() == nullptr);
  *ring.reserve() = 8;
  ring.commit();
  CHECK(ring.peek() && *ring.peek() == 8);
}

static void testWrapAround()
{
  Ring<int> ring(4);
  for (int i = 0; i < 1000; i++)
  {
    *ring.reserve() = i;
    ring.commit();
    CHECK(*ring.peek() == i);
    ring.pop();
  }
  CHECK(ring.size() == 0);
}

// ! --- Two threads --- !

struct Big
{
  uint64_t values[16];
};

static void testThreads()
{
  // Every value arrives once, in order, and whole: a torn slot would have values that differ
  constexpr uint64_t COUNT = 200000;
  Ring<Big> ring(64);
  uint64_t dropped = 0;

  std::thread writer([&]
                     {
                       for (uint64_t i = 0; i < COUNT;)
                       {
                         auto slot = ring.reserve();
                         if (!slot)
                         {
                           dropped++;
                           std::this_thread::yield();
                           continue;
                         }
                         for (auto& value : slot->values)
                           value = i;
                         ring.commit();
                         i++;
                       } });

  uint64_t expected = 0;
  bool torn = false, outOfOrder = false;
  while (expected < COUNT)
  {
    auto slot = ring.peek();
    if (!slot)
      continue;
    for (auto value : slot->values)
      torn |= value != slot->values[0];
    outOfOrder |= slot->values[0] != expected;
    ring.pop();
    expected++;
  }
  writer.join();

  CHECK(!torn);
  CHECK(!outOfOrder);
  CHECK(ring.size() == 0);
}

int main()
{
  testCapacity();
  testEmpty();
  testOrder();
  testFull();
  testReserveWithoutCommit();
  testWrapAround();
  testThreads();
  return finish("ring");
}
