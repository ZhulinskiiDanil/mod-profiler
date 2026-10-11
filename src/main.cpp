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
                             { profiler::setOptions(readOptions()); });
}

$on_game(Loaded)
{
  // Every mod is loaded by now, with its name
  profiler::setMods(loadedMods());
  CCDirector::get()->getScheduler()->scheduleUpdateForTarget(new FrameCounter(), 0, false);
}
