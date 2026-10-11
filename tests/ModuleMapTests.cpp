// Tests for src/ModuleMap.cpp: addresses to modules and buckets, function tables
#include "../src/ModuleMap.hpp"
#include "Check.hpp"

// ! --- Buckets --- !

static void testFixedBuckets()
{
  Buckets buckets;
  CHECK(buckets.size() == bucket::FIRST_MOD);
  CHECK(buckets[bucket::HOOKS].id == "hooks");
  CHECK(buckets[bucket::HOOKS].name == "Hook dispatch");
  CHECK(buckets[bucket::GAME].id == "GeometryDash.exe");
  CHECK(buckets[bucket::COCOS].id == "libcocos2d.dll");
  CHECK(buckets[bucket::OTHER].id == "other");
}

static void testBucketIndex()
{
  Buckets buckets;
  auto a = buckets.index("zhulis.a", "A");
  auto b = buckets.index("zhulis.b", "");
  CHECK(a == bucket::FIRST_MOD && b == bucket::FIRST_MOD + 1);
  // The same ID is the same bucket
  CHECK(buckets.index("zhulis.a", "Other name") == a);
  CHECK(buckets[a].name == "A");
  // No name: the ID shows
  CHECK(buckets[b].name == "zhulis.b");
  CHECK(buckets.find("zhulis.b") == static_cast<int>(b));
  CHECK(buckets.find("nope") == -1);
  buckets.rename(b, "B");
  CHECK(buckets[b].name == "B");
  buckets.rename(b, "");
  CHECK(buckets[b].name == "B");
}

// ! --- Map --- !

static ModuleMap sampleMap()
{
  std::vector<MappedModule> modules(3);
  modules[0] = {.base = 0x1000, .size = 0x1000, .bucket = bucket::GAME, .name = "GeometryDash.exe"};
  modules[1] = {.base = 0x5000, .size = 0x2000, .bucket = bucket::FIRST_MOD, .name = "mod.dll"};
  modules[2] = {.base = 0x3000, .size = 0x1000, .bucket = bucket::COCOS, .name = "libcocos2d.dll"};
  // Out of order on purpose, and two code sections in one module
  std::vector<CodeRange> code = {
      {0x6000, 0x6800, 1},
      {0x1100, 0x1800, 0},
      {0x5100, 0x5200, 1},
      {0x3100, 0x3900, 2},
      // Broken ranges are dropped
      {0x9000, 0x8000, 0},
      {0xA000, 0xB000, 7},
  };
  return ModuleMap(std::move(modules), std::move(code));
}

static void testFindCode()
{
  auto map = sampleMap();
  CHECK(map.findCode(0x1100) == 0);
  CHECK(map.findCode(0x17FF) == 0);
  CHECK(map.findCode(0x1800) == -1);
  // Inside the image but not in code: the headers, the data
  CHECK(map.findCode(0x1000) == -1);
  CHECK(map.findCode(0x5300) == -1);
  CHECK(map.findCode(0x5150) == 1);
  CHECK(map.findCode(0x6000) == 1);
  CHECK(map.findCode(0x3500) == 2);
  CHECK(map.findCode(0) == -1);
  CHECK(map.findCode(0x8500) == -1);
  CHECK(map.findCode(0xA500) == -1);
  CHECK(map.findCode(UINTPTR_MAX) == -1);
}

static void testFindImage()
{
  auto map = sampleMap();
  CHECK(map.findImage(0x1000) == 0);
  CHECK(map.findImage(0x1FFF) == 0);
  CHECK(map.findImage(0x2000) == -1);
  CHECK(map.findImage(0x3000) == 2);
  CHECK(map.findImage(0x6FFF) == 1);
  CHECK(map.findImage(0x7000) == -1);
  CHECK(map.findImage(0x10) == -1);
}

static void testBucketOf()
{
  auto map = sampleMap();
  CHECK(map.bucketOf(0x1200) == bucket::GAME);
  CHECK(map.bucketOf(0x3200) == bucket::COCOS);
  CHECK(map.bucketOf(0x5200) == bucket::FIRST_MOD);
  CHECK(map.bucketOf(0x2500) == bucket::OTHER);
}

static void testEmptyMap()
{
  ModuleMap map({}, {});
  CHECK(map.findCode(0x1000) == -1);
  CHECK(map.findImage(0x1000) == -1);
  CHECK(map.bucketOf(0x1000) == bucket::OTHER);
}

static void testOwnFunctions()
{
  // Generated code: the map points the module at its own copy of the entries
  MappedModule hooks{.base = 0x10000, .size = 0x10000, .bucket = bucket::HOOKS};
  hooks.ownFunctions = {0x100, 0x180, 0x200, 0x300, 0x340, 0x400};
  ModuleMap map({std::move(hooks)}, {{0x10100, 0x10180, 0}, {0x10300, 0x10340, 0}});
  auto const& module = map.module(0);
  CHECK(module.functionCount == 2);
  CHECK(module.functions == module.ownFunctions.data());
  CHECK(findFunction(module.functions, module.functionCount, 0x310) == 1);
  CHECK(map.findCode(0x10310) == 0);
  CHECK(map.bucketOf(0x10310) == bucket::HOOKS);
}

// ! --- Function tables --- !

static void testFindFunction()
{
  // begin, end, unwind: with a gap between the second and the third
  uint32_t const table[] = {
      0x100, 0x180, 0x9000,
      0x180, 0x200, 0x9010,
      0x300, 0x340, 0x9020,
  };
  CHECK(findFunction(table, 3, 0x100) == 0);
  CHECK(findFunction(table, 3, 0x17F) == 0);
  CHECK(findFunction(table, 3, 0x180) == 1);
  CHECK(findFunction(table, 3, 0x1FF) == 1);
  // In the gap: a leaf function without an entry
  CHECK(findFunction(table, 3, 0x200) == -1);
  CHECK(findFunction(table, 3, 0x2FF) == -1);
  CHECK(findFunction(table, 3, 0x33F) == 2);
  CHECK(findFunction(table, 3, 0x340) == -1);
  CHECK(findFunction(table, 3, 0x50) == -1);
  CHECK(findFunction(table, 0, 0x100) == -1);
  CHECK(findFunction(nullptr, 0, 0x100) == -1);
}

int main()
{
  testFixedBuckets();
  testBucketIndex();
  testFindCode();
  testFindImage();
  testBucketOf();
  testEmptyMap();
  testOwnFunctions();
  testFindFunction();
  return finish("modulemap");
}
