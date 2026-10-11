// Tests for src/Attribution.cpp: whose own time a sample is
#include "../src/Attribution.hpp"
#include "Check.hpp"

#include <initializer_list>
#include <memory>

// ! --- A fake process --- !

// Each module is a real buffer, so the unwind info the map reads for function starts is there (zeros:
// no chained parts). Every module has three functions: 0x100, 0x200 and 0x300, 0x100 bytes each
struct Process
{
  static constexpr size_t SIZE = 0x1000;
  std::unique_ptr<uint8_t[]> game{new uint8_t[SIZE]()}, cocos{new uint8_t[SIZE]()}, a{new uint8_t[SIZE]()},
      b{new uint8_t[SIZE]()}, hooks{new uint8_t[SIZE]()}, ntdll{new uint8_t[SIZE]()};
  uint32_t A = bucket::FIRST_MOD;
  uint32_t B = bucket::FIRST_MOD + 1;

  uintptr_t G(uintptr_t offset) const { return reinterpret_cast<uintptr_t>(game.get()) + offset; }
  uintptr_t C(uintptr_t offset) const { return reinterpret_cast<uintptr_t>(cocos.get()) + offset; }
  uintptr_t MA(uintptr_t offset) const { return reinterpret_cast<uintptr_t>(a.get()) + offset; }
  uintptr_t MB(uintptr_t offset) const { return reinterpret_cast<uintptr_t>(b.get()) + offset; }
  uintptr_t H(uintptr_t offset) const { return reinterpret_cast<uintptr_t>(hooks.get()) + offset; }
  uintptr_t N(uintptr_t offset) const { return reinterpret_cast<uintptr_t>(ntdll.get()) + offset; }

  ModuleMap map() const
  {
    auto module = [](uint8_t* image, uint32_t bucket)
    {
      MappedModule mapped{.base = reinterpret_cast<uintptr_t>(image), .size = SIZE, .bucket = bucket};
      // Unwind info at 0x800 for all of them
      mapped.ownFunctions = {0x100, 0x200, 0x800, 0x200, 0x300, 0x800, 0x300, 0x400, 0x800};
      return mapped;
    };
    std::vector<MappedModule> modules;
    modules.push_back(module(game.get(), bucket::GAME));
    modules.push_back(module(cocos.get(), bucket::COCOS));
    modules.push_back(module(a.get(), A));
    modules.push_back(module(b.get(), B));
    modules.push_back(module(hooks.get(), bucket::HOOKS));
    modules.push_back(module(ntdll.get(), bucket::OTHER));
    return ModuleMap(std::move(modules), {});
  }

  // A and B both hook the game's function at 0x100, A with its function at 0x100, B with its own at 0x100.
  // The wait is ntdll's 0x300..0x320
  Attribution attribution() const
  {
    Attribution attribution;
    attribution.hooks.add(A, G(0x100), MA(0x100));
    attribution.hooks.add(B, G(0x100), MB(0x100));
    attribution.waits.push_back({N(0x300), N(0x320)});
    attribution.finish();
    return attribution;
  }
};

static Sample stack(std::initializer_list<uintptr_t> frames)
{
  Sample sample;
  for (auto frame : frames)
    sample.frames[sample.depth++] = frame;
  return sample;
}

static Verdict verdict(Process const& process, Sample const& sample)
{
  return attribute(sample, process.map(), process.attribution());
}

// ! --- Hooks --- !

static void testHookIndex()
{
  HookIndex index;
  index.add(4, 0x1000, 0x5000);
  index.add(5, 0x1000, 0x6000);
  index.add(5, 0x2000, 0x6100);
  index.finish();
  CHECK(index.size() == 3);
  // The original
  CHECK(index.passesOn(4, 0x1000));
  CHECK(!index.passesOn(4, 0x2000));
  // The next mod's detour of a function 4 hooks too
  CHECK(index.passesOn(4, 0x6000));
  // A detour of a function 4 doesn't hook
  CHECK(!index.passesOn(4, 0x6100));
  CHECK(index.passesOn(5, 0x5000));
  CHECK(!index.passesOn(6, 0x1000));
  CHECK(index.targetOf(0x6100) == 0x2000);
  CHECK(index.targetOf(0x7000) == 0);
}

static void testWaits()
{
  Attribution attribution;
  attribution.waits = {{0x300, 0x320}, {0x100, 0x120}};
  attribution.finish();
  CHECK(attribution.isWait(0x100));
  CHECK(attribution.isWait(0x31F));
  CHECK(!attribution.isWait(0x320));
  CHECK(!attribution.isWait(0x200));
  CHECK(!attribution.isWait(0x50));
  attribution.lockWaits = {{0x300, 0x320}};
  attribution.finish();
  CHECK(attribution.isLockWait(0x310));
  CHECK(!attribution.isLockWait(0x110));
}

static void testFunctionStart()
{
  Process process;
  auto map = process.map();
  CHECK(map.functionStart(process.G(0x150)) == process.G(0x100));
  CHECK(map.functionStart(process.G(0x2FF)) == process.G(0x200));
  // Before the first function: no entry
  CHECK(map.functionStart(process.G(0x50)) == 0);
  CHECK(map.functionStart(0x10) == 0);
}

// ! --- Samples --- !

static void testPassingOnToTheOriginal()
{
  // A's detour calling the game's original: the game's time, not A's
  Process process;
  auto result = verdict(process, stack({process.G(0x150), process.H(0x10), process.MA(0x150), process.H(0x10), process.G(0x250)}));
  CHECK(!result.idle);
  CHECK(result.owner == -1);
}

static void testOwnWork()
{
  // A's detour calling one of A's functions, which calls Cocos: A's work
  Process process;
  auto result = verdict(process, stack({process.C(0x150), process.MA(0x250), process.MA(0x150), process.H(0x10), process.G(0x250)}));
  CHECK(result.owner == static_cast<int>(process.A));
}

static void testWhere()
{
  // A's own function at 0x200, called from A's detour of the game's 0x100
  Process process;
  auto result = verdict(process, stack({process.C(0x150), process.MA(0x250), process.MA(0x150), process.H(0x10), process.G(0x250)}));
  CHECK(result.function == process.MA(0x200));
  CHECK(result.hook == process.G(0x100));
  // Not under a detour: no hook
  auto plain = verdict(process, stack({process.MA(0x250), process.C(0x250)}));
  CHECK(plain.function == process.MA(0x200));
  CHECK(plain.hook == 0);
  // Before every function: the address itself
  CHECK(verdict(process, stack({process.MA(0x50), process.C(0x250)})).function == process.MA(0x50));
}

static void testChainOfDetours()
{
  // A's detour calls B's detour of the same function, which calls the original
  Process process;
  auto result = verdict(process, stack({process.G(0x150), process.H(0x10), process.MB(0x150), process.H(0x10), process.MA(0x150),
                                        process.H(0x10), process.G(0x250)}));
  CHECK(result.owner == -1);
}

static void testInnermostWins()
{
  // B's detour works inside the original that A passed the call on to
  Process process;
  auto result = verdict(process, stack({process.C(0x150), process.MB(0x150), process.H(0x10), process.G(0x150), process.H(0x10),
                                        process.MA(0x150), process.H(0x10), process.G(0x250)}));
  CHECK(result.owner == static_cast<int>(process.B));
}

static void testThreadInTheMod()
{
  Process process;
  CHECK(verdict(process, stack({process.MA(0x150), process.H(0x10), process.G(0x250)})).owner == static_cast<int>(process.A));
  // A frame of A under a hook handler it called: A's (the thread is in the handler: the hooks')
  CHECK(verdict(process, stack({process.MA(0x250), process.MA(0x150)})).owner == static_cast<int>(process.A));
}

static void testPatchedCall()
{
  // Called by the game and calling the game, no hook: a patched call (Geode's own start)
  Process process;
  CHECK(verdict(process, stack({process.G(0x350), process.MA(0x250), process.G(0x250)})).owner == -1);
  // Called by Cocos: a node of the mod drawing
  CHECK(verdict(process, stack({process.G(0x350), process.MA(0x250), process.C(0x250)})).owner == static_cast<int>(process.A));
}

static void testIdle()
{
  // A frame limiter in A sleeping
  Process process;
  auto result = verdict(process, stack({process.N(0x310), process.MA(0x250), process.C(0x250)}));
  CHECK(result.idle);
  CHECK(result.owner == -1);
  CHECK(!verdict(process, stack({process.N(0x330), process.MA(0x250)})).idle);
}

static void testBetweenFrames()
{
  // The loop is the game's function at 0x300, a frame is Cocos' 0x200, and A hooks the game's 0x100
  Process process;
  auto attribution = process.attribution();
  attribution.loop = {process.G(0x300)};
  attribution.frame = {process.C(0x200)};
  attribution.finish();
  auto map = process.map();
  // A frame limiter in A spinning in the loop: between frames
  auto spinning = attribute(stack({process.MA(0x250), process.G(0x350)}), map, attribution);
  CHECK(spinning.between);
  CHECK(spinning.owner == -1);
  // A's work in a frame
  auto working = attribute(stack({process.MA(0x250), process.C(0x250), process.G(0x350)}), map, attribution);
  CHECK(!working.between);
  CHECK(working.owner == static_cast<int>(process.A));
  // Outside the loop entirely (the game starting): not between frames
  CHECK(!attribute(stack({process.MA(0x250), process.G(0x150)}), map, attribution).between);
}

static void testDetoursCountAsTheFunction()
{
  // A's detour of the frame function, before it calls the original: still in a frame
  Process process;
  auto attribution = process.attribution();
  attribution.loop = {process.G(0x300)};
  attribution.frame = {process.G(0x100)};
  attribution.finish();
  auto map = process.map();
  auto result = attribute(stack({process.MA(0x150), process.H(0x10), process.G(0x350)}), map, attribution);
  CHECK(!result.between);
  CHECK(result.owner == static_cast<int>(process.A));
}

static void testHookPlumbing()
{
  // A handler calling a function of A that isn't a detour (the hook library's index keeping): the hooks'
  Process process;
  auto plumbing = verdict(process, stack({process.MA(0x250), process.H(0x10), process.G(0x250)}));
  CHECK(plumbing.owner == static_cast<int>(bucket::HOOKS));
  CHECK(plumbing.function == process.MA(0x200));
  // The thread in the handler itself
  auto handler = verdict(process, stack({process.H(0x10), process.MA(0x150), process.G(0x250)}));
  CHECK(handler.owner == static_cast<int>(bucket::HOOKS));
  // A handler calling A's detour: A's
  CHECK(verdict(process, stack({process.MA(0x150), process.H(0x10), process.G(0x250)})).owner == static_cast<int>(process.A));
}

static void testThroughGeodesHelper()
{
  // B is Geode here. A's detour calls the original through one of Geode's functions (not a detour)
  // and a hook handler: A passes on, Geode's function is in between, the game's work is nobody's
  Process process;
  auto attribution = process.attribution();
  attribution.loader = static_cast<int>(process.B);
  auto map = process.map();
  auto passing = attribute(stack({process.G(0x150), process.H(0x10), process.MB(0x250), process.MA(0x150), process.H(0x10),
                                  process.G(0x350)}),
                           map, attribution);
  CHECK(passing.owner == -1);
  // A's own function calling Geode's (an API, its own work): Geode's work for it counts as Geode's
  auto api = attribute(stack({process.C(0x150), process.MB(0x250), process.MA(0x250), process.C(0x350)}), map, attribution);
  CHECK(api.owner == static_cast<int>(process.B));
  // Geode's detour is Geode's own as before
  auto detour = attribute(stack({process.C(0x150), process.MB(0x150), process.H(0x10), process.G(0x350)}), map, attribution);
  CHECK(detour.owner == static_cast<int>(process.B));
}

static void testEmpty()
{
  Process process;
  auto result = verdict(process, Sample{});
  CHECK(!result.idle && result.owner == -1);
  // Only the game
  CHECK(verdict(process, stack({process.G(0x150), process.G(0x250)})).owner == -1);
}

int main()
{
  testHookIndex();
  testWaits();
  testFunctionStart();
  testPassingOnToTheOriginal();
  testOwnWork();
  testWhere();
  testChainOfDetours();
  testInnermostWins();
  testThreadInTheMod();
  testPatchedCall();
  testIdle();
  testHookPlumbing();
  testThroughGeodesHelper();
  testBetweenFrames();
  testDetoursCountAsTheFunction();
  testEmpty();
  return finish("attribution");
}
