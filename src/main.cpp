#include "../include/ModProfiler.hpp"
#include "BaselineFile.hpp"
#include "ZonesApi.hpp"
#include "LoggerLink.hpp"
#include "Overlay.hpp"
#include "Profiler.hpp"
#include "Sampler.hpp"

#include <Geode/Geode.hpp>

using namespace geode::prelude;

namespace
{
  profiler::Options readOptions()
  {
    auto mod = Mod::get();
    return {
        .enabled = mod->getSettingValue<bool>("enabled"),
        .intervalMillis = mod->getSettingValue<double>("interval"),
        .logDiagnostics = mod->getSettingValue<bool>("log-diagnostics"),
        .logStacks = mod->getSettingValue<bool>("log-stacks"),
        .symbols = mod->getSettingValue<bool>("symbols"),
        .spikeMillis = mod->getSettingValue<double>("spike-threshold"),
        .logSpikes = mod->getSettingValue<bool>("log-spikes"),
    };
  }

  std::vector<modules::ModBinary> loadedMods()
  {
    std::vector<modules::ModBinary> mods;
    for (auto mod : Loader::get()->getAllMods())
    {
      if (!mod->isLoaded())
        continue;
      mods.push_back({
          .id = std::string(mod->getID()),
          .name = std::string(mod->getName()),
          .path = mod->getBinaryPath(),
      });
    }
    return mods;
  }

  std::vector<profiler::HookInfo> loadedHooks()
  {
    std::vector<profiler::HookInfo> hooks;
    for (auto mod : Loader::get()->getAllMods())
    {
      if (!mod->isLoaded())
        continue;
      for (auto hook : mod->getHooks())
      {
        // The detour is only in the runtime info, as a decimal string
        auto detour = hook->getRuntimeInfo()["detour"].asString().unwrapOr("0");
        hooks.push_back({
            .mod = std::string(mod->getID()),
            .target = hook->getAddress(),
            .detour = static_cast<uintptr_t>(std::strtoull(detour.c_str(), nullptr, 10)),
            .name = std::string(hook->getDisplayName()),
        });
      }
    }
    return hooks;
  }

  // CCApplication::run is the game's loop: a frame is CCDirector::mainLoop and CCEGLView::pollEvents,
  // the rest of the loop waits for the next frame
  void findLoop()
  {
    auto cocos = GetModuleHandleW(L"libcocos2d.dll");
    if (!cocos)
      return;
    auto exported = [&](char const* name)
    { return reinterpret_cast<uintptr_t>(GetProcAddress(cocos, name)); };
    auto run = exported("?run@CCApplication@cocos2d@@UEAAHXZ");
    auto pollEvents = exported("?pollEvents@CCEGLView@cocos2d@@QEAAXXZ");

    // mainLoop isn't exported: it's in the director's vtable
    uintptr_t mainLoop = 0;
    if (auto director = CCDirector::get())
    {
      auto self = reinterpret_cast<uintptr_t>(static_cast<CCDisplayLinkDirector*>(director)) +
                  addresser::getThunkOffset(&CCDisplayLinkDirector::mainLoop);
      auto table = *reinterpret_cast<uintptr_t*>(self);
      mainLoop = *reinterpret_cast<uintptr_t*>(table + addresser::getVirtualOffset(&CCDisplayLinkDirector::mainLoop));
      // An incremental-linking thunk: a jmp to the function
      if (mainLoop && *reinterpret_cast<uint8_t*>(mainLoop) == 0xE9)
        mainLoop += 5 + *reinterpret_cast<int32_t*>(mainLoop + 1);
    }
    if (!run || !mainLoop)
    {
      log::warn("Couldn't find the game's loop, the time between frames counts as frame time");
      return;
    }
    auto base = reinterpret_cast<uintptr_t>(cocos);
    log::info("The game's loop: run at libcocos2d.dll+{:#x}, mainLoop at +{:#x}", run - base, mainLoop - base);
    profiler::setLoop(run, {mainLoop, pollEvents});
  }

  // MOD_PROFILER_SELFTEST=1 at launch: one slow frame of this mod's own, 40 seconds after the game
  // loaded, and the profile saved 5 seconds later, so a check can see the slow frame found, blamed on
  // the right mod and function, and in the file. MOD_PROFILER_SELFTEST=play opens the first main level
  // 20 seconds in too: nobody plays it, the player dies and it starts again, the game's real work
  void __declspec(noinline) selfTestSlowFrame()
  {
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(120);
    volatile uint64_t spin = 0;
    while (std::chrono::steady_clock::now() < until)
      spin = spin + 1;
    log::info("Self test: a 120 ms slow frame made");
  }

  // A frame that takes 4.5 seconds: the hang report has to come during it
  void __declspec(noinline) selfTestHang()
  {
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(4500);
    volatile uint64_t spin = 0;
    while (std::chrono::steady_clock::now() < until)
      spin = spin + 1;
    log::info("Self test: a 4.5 s frame made");
  }

  // A frame that waits 4 seconds on an event nobody sets: the deadlock report has to come during it
  void __declspec(noinline) selfTestDeadlock()
  {
    auto event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    WaitForSingleObject(event, 4000);
    CloseHandle(event);
    log::info("Self test: a 4 s wait made");
  }

  // Exactly this long every frame: the table's own ms for this mod has to say about the same
  void __declspec(noinline) selfTestCost()
  {
    // Through the API like any mod: the zone has to come out at the same 0.5 ms
    mod_profiler::Zone zone("self test cost");
    auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(500);
    volatile uint64_t spin = 0;
    while (std::chrono::steady_clock::now() < until)
      spin = spin + 1;
  }

  class SelfTest : public CCObject
  {
  public:
    void run(float) { selfTestSlowFrame(); }
    void hang(float) { selfTestHang(); }
    void deadlock(float) { selfTestDeadlock(); }
    void cost(float) { selfTestCost(); }
    void play(float)
    {
      auto level = GameLevelManager::get()->getMainLevel(1, false);
      log::info("Self test: opening {}", std::string(level->m_levelName));
      CCDirector::get()->replaceScene(CCTransitionFade::create(0.5f, PlayLayer::scene(level, false, false)));
    }
    void save(float) { overlay::saveProfile(); }
  };

  // Counts the frames as a plain scheduler callback: a hook on the frame would put this mod on
  // every stack under it, and its share would look like everything the frame does
  class FrameCounter : public CCObject
  {
  public:
    void update(float) override
    {
      profiler::frame();
      // Hooks made after the game loaded (a mod hooking lazily): sent again when their count changes
      if (++m_frames % 600 == 0)
      {
        size_t hooks = 0;
        for (auto mod : Loader::get()->getAllMods())
          if (mod->isLoaded())
            hooks += mod->getHooks().size();
        if (hooks != m_hooks)
        {
          m_hooks = hooks;
          profiler::setHooks(loadedHooks());
        }
      }
      // The scene's name when it changes, from its layer. A transition is still the old scene's
      auto scene = CCDirector::get()->getRunningScene();
      if (scene == m_scene || !scene || typeinfo_cast<CCTransitionScene*>(scene))
        return;
      m_scene = scene;
      auto children = scene->getChildren();
      CCObject* layer = children && children->count() ? children->objectAtIndex(0) : scene;
      profiler::setScene(std::string(getObjectName(layer)));
    }

  private:
    // Only compared, never used: it may be gone
    CCScene* m_scene = nullptr;
    uint64_t m_frames = 0;
    size_t m_hooks = 0;
  };
} // namespace

// ! --- Start --- !

$on_mod(Loaded)
{
  // Before the other mods load: their first zone asks for the API, once
  zones::setup();
  sampler::setZones(&zones::stack());

  // Loaded on the main thread: the one to sample
  if (!profiler::start(readOptions()))
    return;
  profiler::setMods(loadedMods());

  listenForAllSettingChanges([](std::string_view, std::shared_ptr<SettingV3>)
                             {
                               profiler::setOptions(readOptions());
                               overlay::applySettings();
                               logger_link::setWatching(Mod::get()->getSettingValue<bool>("logger-watch")); });

  listenForKeybindSettingPresses("overlay-key", [](Keybind const&, bool down, bool repeat, double)
                                 {
                                   if (down && !repeat)
                                     overlay::toggle(); });

  listenForKeybindSettingPresses("export-key", [](Keybind const&, bool down, bool repeat, double)
                                 {
                                   if (down && !repeat)
                                     overlay::saveProfile(); });
}

$on_game(Loaded)
{
  // Every mod is loaded by now, with its name
  profiler::setMods(loadedMods());
  profiler::setHooks(loadedHooks());
  findLoop();
  profiler::hooksReady();
  CCDirector::get()->getScheduler()->scheduleUpdateForTarget(new FrameCounter(), 0, false);

  char value[32] = {};
  auto selfTest = GetEnvironmentVariableA("MOD_PROFILER_SELFTEST", value, sizeof(value)) ? std::string(value) : std::string();
  if (selfTest == "hang")
    CCDirector::get()->getScheduler()->scheduleSelector(schedule_selector(SelfTest::hang), new SelfTest(), 0, 0, 30, false);
  // Every frame, from 20 seconds in
  if (selfTest == "cost")
    CCDirector::get()->getScheduler()->scheduleSelector(schedule_selector(SelfTest::cost), new SelfTest(), 0, kCCRepeatForever, 20, false);
  if (selfTest == "deadlock")
    CCDirector::get()->getScheduler()->scheduleSelector(schedule_selector(SelfTest::deadlock), new SelfTest(), 0, 0, 30, false);
  if (selfTest == "1" || selfTest == "play")
  {
    // Then the profile with it in, saved
    auto test = new SelfTest();
    if (selfTest == "play")
      CCDirector::get()->getScheduler()->scheduleSelector(schedule_selector(SelfTest::play), test, 0, 0, 20, false);
    CCDirector::get()->getScheduler()->scheduleSelector(schedule_selector(SelfTest::run), test, 0, 0, 40, false);
    CCDirector::get()->getScheduler()->scheduleSelector(schedule_selector(SelfTest::save), test, 0, 0, 45, false);
  }

  logger_link::setWatching(Mod::get()->getSettingValue<bool>("logger-watch"));
  logger_link::setup();
  baseline_file::load();
  overlay::setup();
  overlay::setVisible(Mod::get()->getSavedValue<bool>("overlay-visible", false));
}

// ! --- End --- !

// The whole session in the log when the game closes, for reading after playing
$on_game(Exiting)
{
  profiler::exiting();
  auto snapshot = profiler::snapshot();
  auto const& session = snapshot->session;
  if (session.profile.samples() == 0)
    return;
  log::info("The session: {}", summaryLine(session, snapshot->buckets, 8));
  // Mods' zones, in ms a frame
  std::string zones;
  for (auto const& [id, samples] : session.profile.zones())
  {
    auto name = zones::registry().nameOf(id);
    auto millis = session.frames ? 1000.0 * samples / session.profile.samples() * session.seconds / session.frames : 0;
    zones += fmt::format("{}{}: {} {:.2f} ms", zones.empty() ? "" : ", ", name.mod, name.name, millis);
  }
  if (!zones.empty())
    log::info("The session's zones: {}", zones);
}
