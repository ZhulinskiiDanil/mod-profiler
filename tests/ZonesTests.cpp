// Tests for src/Zones.cpp: the main thread's zone stack and the zone names
#include "../src/Zones.hpp"
#include "Check.hpp"

static void testStack()
{
  ZoneStack stack;
  CHECK(stack.top() == 0);
  stack.push(3);
  stack.push(7);
  CHECK(stack.top() == 7);
  stack.pop();
  CHECK(stack.top() == 3);
  stack.pop();
  CHECK(stack.top() == 0);
  // More pops than pushes don't go below nothing
  stack.pop();
  CHECK(stack.top() == 0);
  CHECK(stack.depth.load() == 0);
}

static void testOverflow()
{
  ZoneStack stack;
  for (uint16_t i = 1; i <= ZoneStack::MAX_DEPTH + 5; i++)
    stack.push(i);
  // The innermost kept one stays on top while the deeper ones are counted
  CHECK(stack.top() == ZoneStack::MAX_DEPTH);
  for (int i = 0; i < 5; i++)
    stack.pop();
  CHECK(stack.top() == ZoneStack::MAX_DEPTH);
  stack.pop();
  CHECK(stack.top() == ZoneStack::MAX_DEPTH - 1);
}

static void testRegistry()
{
  ZoneRegistry zones;
  auto hair = zones.intern("zhulis.icon-mayhem", "hair");
  auto wings = zones.intern("zhulis.icon-mayhem", "wings");
  auto otherHair = zones.intern("someone.else", "hair");
  CHECK(hair == 1 && wings == 2 && otherHair == 3);
  CHECK(zones.intern("zhulis.icon-mayhem", "hair") == hair);
  CHECK(zones.nameOf(wings).name == "wings");
  CHECK(zones.nameOf(otherHair).mod == "someone.else");
  CHECK(zones.nameOf(0).name.empty());
  CHECK(zones.nameOf(99).name.empty());
  CHECK(zones.size() == 3);
}

static void testTooMany()
{
  ZoneRegistry zones;
  for (size_t i = 0; i < ZoneRegistry::MAX_ZONES; i++)
    CHECK(zones.intern("m", std::to_string(i)) != 0);
  CHECK(zones.intern("m", "one more") == 0);
  // Known ones still work
  CHECK(zones.intern("m", "5") == 6);
}

int main()
{
  testStack();
  testOverflow();
  testRegistry();
  testTooMany();
  return finish("zones");
}
