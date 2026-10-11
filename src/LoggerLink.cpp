#include "LoggerLink.hpp"

#include "ImprovedLogger.hpp"
#include "Profiler.hpp"

#include <Geode/Geode.hpp>

using namespace geode::prelude;

namespace
{
  constexpr char const* LOGGER_ID = "zhulis.improved-logger";

  bool s_watching = true;

  // The latest numbers pinned in Improved Logger's console, once a second
  class Watcher : public CCObject
  {
  public:
    void update(float)
    {
      if (!s_watching)
        return;
      auto snapshot = profiler::snapshot();
      auto const& window = snapshot.recent;
      auto const& profile = window.profile;
      if (profile.samples() == 0 || window.frames == 0)
        return;

      auto between = 100.0 * profile.between() / profile.samples();
      auto idle = 100.0 * profile.idle() / profile.samples();
      improved_logger::watch("frame", fmt::format("{:.0f} fps, {:.2f} ms: in frames {:.0f}%, between {:.0f}%, idle {:.0f}%",
                                                  window.frames / window.seconds, window.seconds * 1000 / window.frames,
                                                  std::max(0.0, 100 - between - idle), between, idle));

      std::string own;
      for (auto const& share : topMods(window, 3, Order::Own))
        if (share.ownPercent > 0)
          own += fmt::format("{}{} {:.2f} ms", own.empty() ? "" : ", ", snapshot.buckets[share.bucket].name, share.ownMillisPerFrame);
      improved_logger::watch("own time", own.empty() ? std::string("none") : own);

      if (!snapshot.slowFrames.empty())
      {
        auto const& last = snapshot.slowFrames.back();
        auto who = last.culprit.empty() ? std::string("the game") : fmt::format("{} {:.0f}%", last.culprit, last.culpritPercent);
        improved_logger::watch("slow frames", fmt::format("{}, the last {:.0f} ms at {}: {}", snapshot.slowFrames.size(),
                                                          last.millis, last.when, who));
      }
      improved_logger::watch("profiler cost", fmt::format("stopped {:.1f} ms/s", costOf(snapshot.last).stoppedMillisPerSecond));
    }
  };
} // namespace

void logger_link::setup()
{
  auto logger = Loader::get()->getLoadedMod(LOGGER_ID);
  if (!logger)
    return;

  // A marker from the game: the profile since the one before goes to the log, right under it
  listenForKeybindSettingPresses("marker-key", [](Keybind const&, bool down, bool repeat, double)
                                 {
                                   if (down && !repeat)
                                     profiler::markSpan(""); }, logger);

  CCDirector::get()->getScheduler()->scheduleSelector(schedule_selector(Watcher::update), new Watcher(), 1.f, false);
  log::info("Improved Logger is here: the numbers are pinned in its console, a marker logs the profile since the last one");
}

void logger_link::setWatching(bool watching)
{
  if (s_watching && !watching && Loader::get()->isModLoaded(LOGGER_ID))
    improved_logger::unwatch();
  s_watching = watching;
}
