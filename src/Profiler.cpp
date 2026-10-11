#include "Profiler.hpp"

#include "Ring.hpp"
#include "Sampler.hpp"

#include <Geode/Geode.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_set>

using namespace geode::prelude;

namespace
{
  // About 8 seconds of samples at 1 ms: the analyzer empties it every second
  Ring<Sample> s_ring(8192);
  std::atomic<ModuleMap const*> s_map{nullptr};
  std::atomic<uint64_t> s_frames{0};
  std::atomic<bool> s_logDiagnostics{true};
  std::atomic<bool> s_logStacks{false};
  // Geode's hooks are in place: generated code can be looked up from then on
  std::atomic<bool> s_hooksReady{false};

  // What the main thread hands to the analyzer, and what it gives back
  std::mutex s_mutex;
  std::vector<modules::ModBinary> s_mods;
  uint64_t s_modsVersion = 0;
  // Generated code the main thread looked up, for the analyzer to take
  std::vector<modules::LearnedFunction> s_learnedInbox;
  std::vector<profiler::HookInfo> s_hooks;
  uint64_t s_hooksVersion = 0;
  uintptr_t s_loop = 0;
  std::vector<uintptr_t> s_frameFunctions;
  // At most this many unknown addresses are looked up, in case some code is generated all the time
  constexpr size_t MAX_TRIED = 20000;
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
    // Generated code: what's known, which addresses were already asked about, which to ask about next
    std::vector<modules::LearnedFunction> learned;
    std::unordered_set<uintptr_t> tried;
    std::vector<uintptr_t> unknown;
    Attribution attribution;
    uint64_t hooksVersion = 0;
  };

  // The hooks and the wait functions, again when the hooks list changed
  void refreshAttribution(Analyzer& analyzer)
  {
    std::vector<profiler::HookInfo> hooks;
    Attribution attribution;
    {
      std::lock_guard lock(s_mutex);
      if (analyzer.hooksVersion == s_hooksVersion && analyzer.hooksVersion != 0)
        return;
      analyzer.hooksVersion = s_hooksVersion;
      hooks = s_hooks;
      if (s_loop && !s_frameFunctions.empty())
      {
        attribution.loop = {s_loop};
        attribution.frame = s_frameFunctions;
      }
    }
    for (auto const& hook : hooks)
      attribution.hooks.add(analyzer.buckets.index(hook.mod, ""), hook.target, hook.detour);
    attribution.waits = modules::waitFunctions();
    attribution.finish();
    analyzer.attribution = std::move(attribution);
  }

  // A new map when a module was loaded or unloaded, the mods list came, or generated code was learned
  void refreshMap(Analyzer& analyzer)
  {
    auto handles = modules::loaded();
    std::vector<modules::ModBinary> mods;
    std::vector<modules::LearnedFunction> learned;
    uint64_t version;
    {
      std::lock_guard lock(s_mutex);
      version = s_modsVersion;
      mods = s_mods;
      learned.swap(s_learnedInbox);
    }
    analyzer.learned.insert(analyzer.learned.end(), learned.begin(), learned.end());
    if (analyzer.map && handles == analyzer.handles && version == analyzer.modsVersion && learned.empty())
      return;

    auto map = modules::build(handles, mods, analyzer.learned, analyzer.buckets);
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

  // Addresses outside every module, to look up on the main thread once
  void collectUnknown(Analyzer& analyzer, Sample const& sample)
  {
    for (int i = 0; i < sample.depth && i < MAX_FRAMES; i++)
    {
      // A return address is after its call: the call itself may end a function
      auto address = i == 0 ? sample.frames[i] : sample.frames[i] - 1;
      if (analyzer.map->findCode(address) >= 0 || analyzer.tried.size() >= MAX_TRIED)
        continue;
      if (analyzer.tried.insert(address).second)
        analyzer.unknown.push_back(address);
    }
  }

  void learnUnknown(Analyzer& analyzer)
  {
    if (analyzer.unknown.empty() || !s_hooksReady.load())
      return;
    queueInMainThread([addresses = std::move(analyzer.unknown)]
                      {
                        std::vector<modules::LearnedFunction> found;
                        for (auto address : addresses)
                          if (auto function = modules::learn(address))
                            found.push_back(*function);
                        if (found.empty())
                          return;
                        std::lock_guard lock(s_mutex);
                        s_learnedInbox.insert(s_learnedInbox.end(), found.begin(), found.end()); });
    analyzer.unknown.clear();
  }

  void analyze(Analyzer& analyzer, Window& window)
  {
    auto toMicros = 1e6 / sampler::ticksPerSecond();
    while (auto sample = s_ring.peek())
    {
      window.profile.add(*sample, *analyzer.map, &analyzer.attribution);
      collectUnknown(analyzer, *sample);
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
      refreshAttribution(analyzer);
      if (analyzer.map)
        analyze(analyzer, window);
      learnUnknown(analyzer);
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

void profiler::setHooks(std::vector<HookInfo> hooks)
{
  std::lock_guard lock(s_mutex);
  s_hooks = std::move(hooks);
  s_hooksVersion++;
}

void profiler::setLoop(uintptr_t loop, std::vector<uintptr_t> frame)
{
  std::lock_guard lock(s_mutex);
  s_loop = loop;
  s_frameFunctions = std::move(frame);
  s_hooksVersion++;
}

void profiler::frame() { s_frames.fetch_add(1, std::memory_order_relaxed); }

void profiler::hooksReady() { s_hooksReady = true; }

profiler::Snapshot profiler::snapshot()
{
  std::lock_guard lock(s_mutex);
  return s_snapshot;
}
