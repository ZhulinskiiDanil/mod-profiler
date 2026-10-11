#include "Profiler.hpp"

#include "Ring.hpp"
#include "Sampler.hpp"

#include <Geode/Geode.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

using namespace geode::prelude;

namespace
{
  // About 8 seconds of samples at 1 ms: the analyzer empties it every second
  Ring<Sample> s_ring(8192);
  std::atomic<ModuleMap const*> s_map{nullptr};
  std::atomic<uint64_t> s_frames{0};
  std::atomic<bool> s_logDiagnostics{true};
  std::atomic<bool> s_logStacks{false};

  // What the main thread hands to the analyzer, and what it gives back
  std::mutex s_mutex;
  std::vector<modules::ModBinary> s_mods;
  uint64_t s_modsVersion = 0;
  profiler::Snapshot s_snapshot;

  // ! --- The analyzer thread's own state --- !

  struct Retired
  {
    std::unique_ptr<ModuleMap> map;
    // The sampler's iteration when it was swapped out
    uint64_t iteration = 0;
  };

  struct Analyzer
  {
    Buckets buckets;
    std::unique_ptr<ModuleMap> map;
    std::vector<void*> handles;
    uint64_t modsVersion = 0;
    std::vector<Retired> retired;
    Window session;
  };

  // A new map when a module was loaded or unloaded, or the mods list came
  void refreshMap(Analyzer& analyzer)
  {
    auto handles = modules::loaded();
    std::vector<modules::ModBinary> mods;
    uint64_t version;
    {
      std::lock_guard lock(s_mutex);
      version = s_modsVersion;
      if (version != analyzer.modsVersion)
        mods = s_mods;
    }
    if (analyzer.map && handles == analyzer.handles && version == analyzer.modsVersion)
      return;
    if (version == analyzer.modsVersion)
    {
      std::lock_guard lock(s_mutex);
      mods = s_mods;
    }

    auto map = modules::build(handles, mods, analyzer.buckets);
    s_map.store(map.get(), std::memory_order_release);
    if (analyzer.map)
      analyzer.retired.push_back({std::move(analyzer.map), sampler::iteration()});
    analyzer.map = std::move(map);
    analyzer.handles = std::move(handles);
    analyzer.modsVersion = version;
  }

  // Maps the sampler can't be reading anymore: it has finished the sample it was in when they were swapped
  void freeRetired(Analyzer& analyzer, bool all)
  {
    auto now = sampler::iteration();
    std::erase_if(analyzer.retired, [&](Retired& retired)
                  {
                    if (!all && now < retired.iteration + 2)
                      return false;
                    modules::release(*retired.map);
                    return true; });
  }

  void analyze(Analyzer& analyzer, Window& window)
  {
    auto toMicros = 1e6 / sampler::ticksPerSecond();
    while (auto sample = s_ring.peek())
    {
      window.profile.add(*sample, *analyzer.map);
      window.stats.add(*sample, toMicros);
      // A few stacks to look at, spread over the second
      if (s_logStacks.load(std::memory_order_relaxed) && window.stats.samples % 300 == 1)
        log::info("Stack: {}", describeStack(*sample, *analyzer.map));
      s_ring.pop();
    }
  }

  void run()
  {
    utils::thread::setName("Mod Profiler");
    Analyzer analyzer;

    auto last = std::chrono::steady_clock::now();
    auto next = last + std::chrono::seconds(1);
    auto frames = s_frames.load();
    auto dropped = sampler::dropped();
    // The sampler waits for the first map
    refreshMap(analyzer);

    while (true)
    {
      std::this_thread::sleep_until(next);
      next += std::chrono::seconds(1);

      // Analyzing with the map the samples were taken with, then catching up with new modules
      Window window;
      if (analyzer.map)
        analyze(analyzer, window);
      refreshMap(analyzer);

      auto now = std::chrono::steady_clock::now();
      window.seconds = std::chrono::duration<double>(now - last).count();
      last = now;
      auto framesNow = s_frames.load();
      window.frames = framesNow - frames;
      frames = framesNow;
      auto droppedNow = sampler::dropped();
      window.stats.dropped = droppedNow - dropped;
      dropped = droppedNow;

      if (s_logDiagnostics.load(std::memory_order_relaxed) && (sampler::running() || window.stats.samples > 0))
        log::info("{}", diagnosticsLine(window, analyzer.buckets, 5));

      analyzer.session.profile.merge(window.profile);
      analyzer.session.stats.merge(window.stats);
      analyzer.session.seconds += window.seconds;
      analyzer.session.frames += window.frames;
      {
        std::lock_guard lock(s_mutex);
        s_snapshot.last = std::move(window);
        s_snapshot.session = analyzer.session;
        s_snapshot.buckets = analyzer.buckets;
        s_snapshot.modules = analyzer.map->modules();
      }

      freeRetired(analyzer, !sampler::alive());
    }
  }
} // namespace

bool profiler::start(Options const& options)
{
  if (!sampler::captureMainThread())
  {
    log::error("Couldn't get the main thread: {}", GetLastError());
    return false;
  }
  setOptions(options);
  std::thread(run).detach();
  return true;
}

void profiler::setOptions(Options const& options)
{
  s_logDiagnostics = options.logDiagnostics;
  s_logStacks = options.logStacks;
  sampler::setInterval(options.intervalMillis);
  if (options.enabled && !sampler::running())
    sampler::start(s_ring, s_map, options.intervalMillis);
  else if (!options.enabled)
    sampler::stop();
}

void profiler::setMods(std::vector<modules::ModBinary> mods)
{
  std::lock_guard lock(s_mutex);
  s_mods = std::move(mods);
  s_modsVersion++;
}

void profiler::frame() { s_frames.fetch_add(1, std::memory_order_relaxed); }

profiler::Snapshot profiler::snapshot()
{
  std::lock_guard lock(s_mutex);
  return s_snapshot;
}
