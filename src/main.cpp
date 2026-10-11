#include "LoggerLink.hpp"
#include "Overlay.hpp"
#include "Profiler.hpp"

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

  // The last seconds to a file for speedscope.app, with a notification of where
  void exportProfile()
  {
    Notification::create("Saving the last 10 seconds...", NotificationIcon::Loading, 1.5f)->show();
    profiler::requestExport([](std::string const& file, std::string const& error)
                            {
                              if (!error.empty())
                              {
                                Notification::create("Not saved: " + error, NotificationIcon::Error)->show();
                                return;
                              }
                              log::info("Saved the profile to {}, open it at https://www.speedscope.app", file);
                              Notification::create("Saved: open it at speedscope.app (the folder is in the log)", NotificationIcon::Success, 4.f)->show(); });
  }

  // MOD_PROFILER_SELFTEST=1 at launch: one slow frame of this mod's own, 40 seconds after the game
  // loaded, and the profile saved 5 seconds later, so a check can see the slow frame found, blamed on
  // the right mod and function, and in the file
  void __declspec(noinline) selfTestSlowFrame()
  {
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(120);
    volatile uint64_t spin = 0;
    while (std::chrono::steady_clock::now() < until)
      spin = spin + 1;
    log::info("Self test: a 120 ms slow frame made");
  }

  class SelfTest : public CCObject
  {
  public:
    void run(float) { selfTestSlowFrame(); }
    void save(float) { exportProfile(); }
  };

  // Counts the frames as a plain scheduler callback: a hook on the frame would put this mod on
  // every stack under it, and its share would look like everything the frame does
  class FrameCounter : public CCObject
  {
  public:
    void update(float) override { profiler::frame(); }
  };
} // namespace

// ! --- Start --- !

$on_mod(Loaded)
{
  // Loaded on the main thread: the one to sample
  if (!profiler::start(readOptions()))
    return;
  profiler::setMods(loadedMods());

  listenForAllSettingChanges([](std::string_view, std::shared_ptr<SettingV3>)
                             {
                               profiler::setOptions(readOptions());
                               logger_link::setWatching(Mod::get()->getSettingValue<bool>("logger-watch")); });

  listenForKeybindSettingPresses("overlay-key", [](Keybind const&, bool down, bool repeat, double)
                                 {
                                   if (down && !repeat)
                                     overlay::toggle(); });

  listenForKeybindSettingPresses("export-key", [](Keybind const&, bool down, bool repeat, double)
                                 {
                                   if (down && !repeat)
                                     exportProfile(); });
}

$on_game(Loaded)
{
  // Every mod is loaded by now, with its name
  profiler::setMods(loadedMods());
  profiler::setHooks(loadedHooks());
  findLoop();
  profiler::hooksReady();
  CCDirector::get()->getScheduler()->scheduleUpdateForTarget(new FrameCounter(), 0, false);

  char value[8] = {};
  if (GetEnvironmentVariableA("MOD_PROFILER_SELFTEST", value, sizeof(value)) && value[0] == '1')
  {
    // Then the profile with it in, saved
    auto test = new SelfTest();
    CCDirector::get()->getScheduler()->scheduleSelector(schedule_selector(SelfTest::run), test, 0, 0, 40, false);
    CCDirector::get()->getScheduler()->scheduleSelector(schedule_selector(SelfTest::save), test, 0, 0, 45, false);
  }

  logger_link::setWatching(Mod::get()->getSettingValue<bool>("logger-watch"));
  logger_link::setup();
  overlay::setup();
  overlay::setVisible(Mod::get()->getSavedValue<bool>("overlay-visible", false));
}
