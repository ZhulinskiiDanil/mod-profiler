#include "Profiler.hpp"

#include "Ring.hpp"
#include "Sampler.hpp"
#include "Speedscope.hpp"
#include "Spikes.hpp"
#include "Symbols.hpp"

#include <Geode/Geode.hpp>

#include <atomic>
#include <chrono>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>

using namespace geode::prelude;

namespace
{
  // About 4 seconds of samples at 1 ms: the analyzer empties it every second
  Ring<Sample> s_ring(4096);
  std::atomic<ModuleMap const*> s_map{nullptr};
  std::atomic<uint64_t> s_frames{0};
  std::atomic<bool> s_logDiagnostics{true};
  std::atomic<bool> s_logStacks{false};
  std::atomic<double> s_spikeMillis{50};
  std::atomic<bool> s_logSpikes{true};
  std::atomic<bool> s_namesWanted{false};
  // When each frame ended, from the main thread
  Ring<int64_t> s_frameEnds(4096);
  std::atomic<uint64_t> s_frameEndsLost{0};
  // An export asked for, under s_mutex
  std::function<void(std::string const&, std::string const&)> s_exportDone;
  bool s_exportStartup = false;
  // A span asked for, under s_mutex: its label
  std::optional<std::string> s_spanLabel;
  std::atomic<bool> s_resetSession{false};
  std::atomic<bool> s_exiting{false};
  std::string s_scene;
  Baseline s_baseline;
  // Geode's hooks are in place: generated code can be looked up from then on
  std::atomic<bool> s_hooksReady{false};
  // When: in sample ticks, and on the steady clock
  std::atomic<int64_t> s_hooksReadyTicks{0};
  std::atomic<std::chrono::steady_clock::time_point> s_hooksReadyTime{};

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
  std::shared_ptr<profiler::Snapshot const> s_snapshot = std::make_shared<profiler::Snapshot>();

  // ! --- The analyzer thread's own state --- !

  struct Analyzer
  {
    Buckets buckets;
    std::unique_ptr<ModuleMap> map;
    std::vector<void*> handles;
    uint64_t modsVersion = 0;
    // When the map was made: it's made again every few seconds, see refreshMap
    std::chrono::steady_clock::time_point mapTime;
    // Maps the sampler may still have been reading when they were taken away
    struct Retired
    {
      std::unique_ptr<ModuleMap> map;
      uint64_t iteration = 0;
    };
    std::vector<Retired> retired;
    Window session;
    // Generated code: what's known, which addresses were already asked about, which to ask about next
    std::vector<modules::LearnedFunction> learned;
    std::unordered_set<uintptr_t> tried;
    std::vector<uintptr_t> unknown;
    Attribution attribution;
    uint64_t hooksVersion = 0;
    std::unordered_map<uintptr_t, std::string> hookNames;
    std::deque<Window> recent;
    // The last seconds of samples, for cutting the slow frames out of
    std::deque<Sample> held;
    FrameClock clock;
    uint64_t frameEndsLost = 0;
    // When the last frame ended, and when a frame still going on was last told about
    int64_t lastFrameEnd = 0;
    int64_t lastHangReport = 0;
    std::deque<profiler::SlowFrame> slowFrames;
    // Since the last marker
    Window span;
    size_t spanSlowFrames = 0;

    // The game's start: its samples are kept until Geode's hooks are known (the game is loaded), and
    // counted a few seconds after that, once the hook handlers in them are learned too
    struct StartupSample
    {
      int64_t time = 0;
      uint16_t depth = 0;
      // Into startupFrames
      size_t first = 0;
    };
    std::vector<StartupSample> startupSamples;
    std::vector<uintptr_t> startupFrames;
    std::chrono::steady_clock::time_point startupBegin = std::chrono::steady_clock::now();
    bool startupDone = false;
    Window startup;

    // The scene the game is in, and the numbers since it went there
    std::string scene;
    Window sceneWindow;
  };

  // A scene shorter than this isn't summed up when the game leaves it
  constexpr double MIN_SCENE_SECONDS = 3;

  // At most this many frames of the start's stacks are kept: 16 MB, a minute or more of loading
  constexpr size_t MAX_STARTUP_FRAMES = 2'000'000;
  constexpr auto STARTUP_LEARNING = std::chrono::seconds(3);

  // The samples taken before the game was loaded
  void keepForStartup(Analyzer& analyzer, Sample const& sample)
  {
    auto loaded = s_hooksReadyTicks.load();
    if (analyzer.startupDone || sample.depth == 0 || (loaded && sample.time >= loaded) ||
        analyzer.startupFrames.size() + sample.depth > MAX_STARTUP_FRAMES)
      return;
    analyzer.startupSamples.push_back({sample.time, sample.depth, analyzer.startupFrames.size()});
    analyzer.startupFrames.insert(analyzer.startupFrames.end(), sample.frames, sample.frames + std::min<int>(sample.depth, MAX_FRAMES));
  }

  // At most this many samples are held for the slow frames and the export: about 10 seconds.
  // A frame longer than that has only its end looked into
  constexpr size_t HELD_SAMPLES = 10000;

  // A function's name, or module+offset
  std::string nameOf(Analyzer& analyzer, uintptr_t address)
  {
    if (auto hook = analyzer.hookNames.find(address); hook != analyzer.hookNames.end())
      return hook->second;
    auto index = analyzer.map->findImage(address);
    auto name = index >= 0 ? symbols::name(address, analyzer.map->module(index)) : std::string();
    if (name.empty())
      return describeAddress(address, *analyzer.map);
    // The module too, for the stacks: the name alone doesn't say whose it is
    return analyzer.map->module(index).name + "!" + name;
  }

  std::string localTime()
  {
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
    localtime_s(&local, &now);
    char text[16];
    std::strftime(text, sizeof(text), "%H:%M:%S", &local);
    return text;
  }

  // Samples as a speedscope profile: frames merged by function, hook handlers as one. `sampleAt(i)`
  // gives the i-th of `count` samples, oldest first
  template <class SampleAt>
  speedscope::Profile exportProfile(Analyzer& analyzer, std::string name, size_t count, SampleAt&& sampleAt)
  {
    speedscope::Profile profile;
    profile.name = std::move(name);
    std::unordered_map<uintptr_t, uint32_t> frameOf;
    uint32_t handlerFrame = UINT32_MAX;

    auto frameFor = [&](uintptr_t address, bool first) -> uint32_t
    {
      auto lookup = first ? address : address - 1;
      if (analyzer.map->bucketOf(lookup) == bucket::HOOKS)
      {
        if (handlerFrame == UINT32_MAX)
        {
          handlerFrame = static_cast<uint32_t>(profile.frames.size());
          profile.frames.push_back({"(hook handler)", ""});
        }
        return handlerFrame;
      }
      auto start = analyzer.map->functionStart(lookup);
      auto key = start ? start : address;
      if (auto known = frameOf.find(key); known != frameOf.end())
        return known->second;
      auto index = static_cast<uint32_t>(profile.frames.size());
      auto image = analyzer.map->findImage(key);
      profile.frames.push_back({shortName(nameOf(analyzer, key)), image >= 0 ? analyzer.map->module(image).name : ""});
      frameOf[key] = index;
      return index;
    };

    auto ticksPerMilli = sampler::ticksPerSecond() / 1000.0;
    for (size_t i = 0; i < count; i++)
    {
      Sample const& sample = sampleAt(i);
      if (sample.depth == 0)
        continue;
      std::vector<uint32_t> stack;
      for (int frame = std::min<int>(sample.depth, MAX_FRAMES) - 1; frame >= 0; frame--)
        stack.push_back(frameFor(sample.frames[frame], frame == 0));
      auto time = sample.time;
      // A sample stands for the time until the next one, a few intervals at most (the sampler can be late)
      double weight = 1;
      if (i + 1 < count)
        weight = std::clamp((sampleAt(i + 1).time - time) / ticksPerMilli, 0.0, 10.0);
      profile.stacks.push_back(std::move(stack));
      profile.weights.push_back(weight);
    }
    return profile;
  }

  // A kept sample of the start, whole again
  Sample const& startupSample(Analyzer& analyzer, size_t index, Sample& into)
  {
    auto const& kept = analyzer.startupSamples[index];
    into.time = kept.time;
    into.depth = kept.depth;
    std::copy_n(analyzer.startupFrames.begin() + kept.first, kept.depth, into.frames);
    return into;
  }

  void exportIfAsked(Analyzer& analyzer)
  {
    std::function<void(std::string const&, std::string const&)> done;
    bool startup;
    {
      std::lock_guard lock(s_mutex);
      done.swap(s_exportDone);
      startup = s_exportStartup;
    }
    if (!done)
      return;

    std::string file, error;
    if (!analyzer.map)
      error = "Sampling is off";
    else if (startup ? !analyzer.startupDone || analyzer.startupSamples.empty() : analyzer.held.empty())
      error = startup ? "The start isn't counted yet" : "No samples yet";
    else
    {
      auto folder = Mod::get()->getSaveDir() / "profiles";
      std::error_code code;
      std::filesystem::create_directories(folder, code);
      auto time = localTime();
      std::erase(time, ':');
      auto path = folder / fmt::format("{}-{}.speedscope.json", startup ? "start" : "profile", time);
      std::ofstream out(path, std::ios::binary);
      if (startup)
      {
        Sample scratch;
        out << speedscope::json(exportProfile(analyzer, "Geometry Dash main thread, the game's start", analyzer.startupSamples.size(),
                                              [&](size_t i) -> Sample const& { return startupSample(analyzer, i, scratch); }));
      }
      else
        out << speedscope::json(exportProfile(analyzer, fmt::format("Geometry Dash main thread, {}", localTime()), analyzer.held.size(),
                                              [&](size_t i) -> Sample const& { return analyzer.held[i]; }));
      if (out)
        file = utils::string::pathToString(path);
      else
        error = "Couldn't write the file";
    }
    queueInMainThread([done = std::move(done), file, error]
                      { done(file, error); });
  }

  // The start counted with what's known now: the hooks, the learned handlers
  void countStartup(Analyzer& analyzer)
  {
    if (analyzer.startupDone || !analyzer.map || !s_hooksReady.load())
      return;
    auto loadedAt = s_hooksReadyTime.load();
    if (std::chrono::steady_clock::now() - loadedAt < STARTUP_LEARNING)
      return;
    // Sampling was off while the game loaded: there's no start to tell about
    if (analyzer.startupSamples.empty())
    {
      analyzer.startupDone = true;
      return;
    }

    Window startup;
    startup.seconds = std::chrono::duration<double>(loadedAt - analyzer.startupBegin).count();
    Sample sample;
    for (size_t i = 0; i < analyzer.startupSamples.size(); i++)
    {
      startup.profile.add(startupSample(analyzer, i, sample), *analyzer.map, &analyzer.attribution);
      startup.stats.samples++;
    }
    analyzer.startup = std::move(startup);
    analyzer.startupDone = true;
    // Kept for [save profile] in the table's start view: a few MB
    auto samples = analyzer.startupSamples.size();

    log::info("The game's start, from Mod Profiler loaded to the game loaded: {}", startupLine(analyzer.startup, analyzer.buckets, 8));
    // Where the mod with the most of it spent it
    auto top = topMods(analyzer.startup, 1, Order::Own);
    if (!top.empty() && top.front().ownPercent > 0)
    {
      auto bucket = top.front().bucket;
      std::string places;
      for (auto const& place : topPlaces(analyzer.startup.profile.functions(), bucket, 3))
        places += fmt::format("{}{} {:.0f}%", places.empty() ? "" : ", ", shortName(nameOf(analyzer, place.address)),
                              100.0 * place.samples / analyzer.startup.profile.at(bucket).own);
      for (auto const& place : topPlaces(analyzer.startup.profile.hooks(), bucket, 2))
        places += fmt::format(", under its hook of {}", nameOf(analyzer, place.address));
      log::info("The start's busiest mod, {}: {} ({} samples kept)", analyzer.buckets[bucket].name, places, samples);
    }
  }

  // A frame going on for this long is told about before it ends, and again this often while it lasts: if
  // the game hangs, the log says where
  constexpr double HANG_SECONDS = 2;
  constexpr double HANG_REPEAT_SECONDS = 5;
  // A frame that's mostly waiting is the game minimized or a dialog, not a hang. Unless it waits on a lock
  // or another thread nearly all the time: then it may be deadlocked
  constexpr double HANG_MIN_WORK = 0.5;
  constexpr double DEADLOCK_SHARE = 0.8;

  void watchForHang(Analyzer& analyzer)
  {
    if (!analyzer.lastFrameEnd || !sampler::running() || s_exiting.load())
      return;
    auto ticksPerSecond = static_cast<double>(sampler::ticksPerSecond());
    auto now = sampler::now();
    if ((now - analyzer.lastFrameEnd) / ticksPerSecond < HANG_SECONDS)
      return;
    if (analyzer.lastHangReport > analyzer.lastFrameEnd && (now - analyzer.lastHangReport) / ticksPerSecond < HANG_REPEAT_SECONDS)
      return;

    std::vector<Sample const*> samples;
    std::vector<Verdict> verdicts;
    for (auto const& sample : analyzer.held)
      if (sample.depth > 0 && sample.time >= analyzer.lastFrameEnd)
      {
        samples.push_back(&sample);
        verdicts.push_back(attribute(sample, *analyzer.map, analyzer.attribution));
      }
    if (samples.empty())
      return;
    auto spike = makeSpike({analyzer.lastFrameEnd, now}, ticksPerSecond / 1000, samples, verdicts, 1);
    auto const& last = *samples.back();
    auto stackOf = [&](Sample const& sample)
    {
      std::string stack;
      for (int i = 0; i < sample.depth && i < MAX_FRAMES; i++)
        stack += (i ? " < " : "") + nameOf(analyzer, sample.frames[i]);
      return stack;
    };

    if (spike.idlePercent > 100 * (1 - HANG_MIN_WORK))
    {
      auto locked = lockWaitShare(samples, analyzer.attribution);
      if (locked < DEADLOCK_SHARE)
        return;
      analyzer.lastHangReport = now;
      auto mod = waitingMod(samples, *analyzer.map, analyzer.attribution);
      log::warn("One frame has taken {:.1f} s so far, waiting on a lock or another thread {:.0f}% of it (the game may be deadlocked){}",
                spike.millis / 1000, locked * 100,
                mod < 0 ? std::string() : ", in a call of " + analyzer.buckets[static_cast<uint32_t>(mod)].name);
      log::warn("Where it waits: {}", stackOf(last));
      return;
    }
    analyzer.lastHangReport = now;

    auto who = spike.culprit < 0 ? fmt::format("no mod (the game {:.0f}%)", spike.gamePercent)
                                 : fmt::format("{} {:.0f}%", analyzer.buckets[static_cast<uint32_t>(spike.culprit)].name, spike.culpritPercent);
    if (!spike.functions.empty())
      who += fmt::format(" in {}", nameOf(analyzer, spike.functions.front().address));
    if (!spike.hooks.empty())
      who += fmt::format(" under its hook of {}", nameOf(analyzer, spike.hooks.front().address));
    log::warn("One frame has taken {:.1f} s so far (the game may hang): {}", spike.millis / 1000, who);
    // The newest sample says where the thread is right now
    log::warn("Where it is now: {}", stackOf(last));
  }

  void findSlowFrames(Analyzer& analyzer)
  {
    // The game closing is one long frame of cleaning up, not a slow one
    if (s_exiting.load())
      return;
    auto ticksPerMilli = sampler::ticksPerSecond() / 1000.0;
    auto threshold = static_cast<int64_t>(s_spikeMillis.load() * ticksPerMilli);
    for (auto const& frame : analyzer.clock.slowFrames(threshold))
    {
      std::vector<Sample const*> samples;
      std::vector<Verdict> verdicts;
      for (auto const& sample : analyzer.held)
        // A sample without a stack (the thread couldn't be read) isn't anyone's time
        if (sample.depth > 0 && sample.time >= frame.start && sample.time < frame.end)
        {
          samples.push_back(&sample);
          verdicts.push_back(attribute(sample, *analyzer.map, analyzer.attribution));
        }
      // Sampling was off, or the frame is older than the held samples: nothing to say about it
      if (samples.empty())
        continue;
      auto spike = makeSpike(frame, ticksPerMilli, samples, verdicts, 3);
      // Nearly all waiting, and not on a lock or another thread: the game minimized or paused, not slow
      if (spike.idlePercent >= 90 && lockWaitShare(samples, analyzer.attribution) < 0.5)
        continue;

      profiler::SlowFrame slow{
          .when = localTime(),
          .millis = spike.millis,
          .culpritPercent = spike.culpritPercent,
          .gamePercent = spike.gamePercent,
          .idlePercent = spike.idlePercent,
      };
      if (spike.culprit >= 0)
        slow.culprit = analyzer.buckets[static_cast<uint32_t>(spike.culprit)].name;
      if (!spike.functions.empty())
        slow.function = nameOf(analyzer, spike.functions.front().address);
      if (!spike.hooks.empty())
        slow.hook = nameOf(analyzer, spike.hooks.front().address);
      auto stacks = spike.stacks;
      // Mostly waiting: whose call it waited in, and where
      if (spike.culprit < 0 && spike.idlePercent >= 50)
      {
        if (auto mod = waitingMod(samples, *analyzer.map, analyzer.attribution); mod >= 0)
          slow.waitingIn = analyzer.buckets[static_cast<uint32_t>(mod)].name;
        // A waiting sample from the middle of the frame: where it waited
        stacks.push_back(*samples[samples.size() / 2]);
      }
      for (auto const& stack : stacks)
      {
        std::string text;
        for (int i = 0; i < stack.depth && i < MAX_FRAMES; i++)
          text += (i ? " < " : "") + nameOf(analyzer, stack.frames[i]);
        slow.stacks.push_back(std::move(text));
      }

      if (s_logSpikes.load(std::memory_order_relaxed))
      {
        auto who = slow.culprit.empty() ? std::string("no mod") : fmt::format("{} {:.0f}%", slow.culprit, slow.culpritPercent);
        if (!slow.function.empty())
          who += fmt::format(" in {}", slow.function);
        if (!slow.hook.empty())
          who += fmt::format(" under its hook of {}", slow.hook);
        log::info("Slow frame: {:.0f} ms, {} samples: {}, the game {:.0f}%, waiting {:.0f}%{}", slow.millis, spike.samples, who,
                  slow.gamePercent, slow.idlePercent, slow.waitingIn.empty() ? "" : " in a call of " + slow.waitingIn);
        if (!slow.stacks.empty())
          log::info("Slow frame stack: {}", slow.stacks.front());
      }

      analyzer.spanSlowFrames++;
      analyzer.slowFrames.push_back(std::move(slow));
      while (analyzer.slowFrames.size() > profiler::KEPT_SLOW_FRAMES)
        analyzer.slowFrames.pop_front();
    }

  }

  // The busiest places of every mod in a window, named: hooks by Geode, functions as module+offset
  std::unordered_map<uintptr_t, std::string> nameTopPlaces(Analyzer& analyzer, Window const& window)
  {
    constexpr size_t PER_MOD = 8;
    std::unordered_map<uintptr_t, std::string> names;
    // Sampling is off: no map to name anything with. Or the table is hidden: nobody to name it for
    if (!analyzer.map || !s_namesWanted.load(std::memory_order_relaxed))
      return names;
    auto const& buckets = window.profile.buckets();
    for (uint32_t bucket = bucket::FIRST_MOD; bucket < buckets.size(); bucket++)
    {
      if (buckets[bucket].own == 0)
        continue;
      for (auto const& place : topPlaces(window.profile.hooks(), bucket, PER_MOD))
      {
        auto name = analyzer.hookNames.find(place.address);
        names[place.address] = name != analyzer.hookNames.end() ? name->second : describeAddress(place.address, *analyzer.map);
      }
      for (auto const& place : topPlaces(window.profile.functions(), bucket, PER_MOD))
      {
        if (names.contains(place.address))
          continue;
        auto index = analyzer.map->findImage(place.address);
        auto name = index >= 0 ? symbols::name(place.address, analyzer.map->module(index)) : std::string();
        names[place.address] = name.empty() ? describeAddress(place.address, *analyzer.map) : name;
      }
    }
    return names;
  }

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
    analyzer.hookNames.clear();
    for (auto const& hook : hooks)
    {
      attribution.hooks.add(analyzer.buckets.index(hook.mod, ""), hook.target, hook.detour);
      if (!hook.name.empty())
        analyzer.hookNames[hook.target] = hook.name;
    }
    attribution.waits = modules::waitFunctions();
    attribution.lockWaits = modules::lockWaitFunctions();
    attribution.loader = static_cast<int>(bucket::LOADER);
    attribution.finish();
    analyzer.attribution = std::move(attribution);
  }

  // A map pins its modules. This often the pins are all let go before the next map, so a module some
  // mod unloaded, which only the pins kept, goes away
  constexpr auto UNPIN_EVERY = std::chrono::seconds(30);

  // Until the sampler has started two samples since: it can't be reading a map it saw before anymore
  // False when the sampler didn't get there in time (a loaded system, a debugger): the map it may still
  // read is kept then, see retire
  bool waitForSampler(uint64_t start)
  {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (sampler::alive() && sampler::iteration() < start + 2)
    {
      if (std::chrono::steady_clock::now() >= deadline)
        return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }

  // A map taken from the sampler at `iteration` goes once the sampler can't be reading it: now, or at a
  // later second if the wait timed out. Its modules stay pinned until then
  void letGo(Analyzer& analyzer, std::unique_ptr<ModuleMap> map, uint64_t iteration)
  {
    if (waitForSampler(iteration))
      modules::release(*map);
    else
      analyzer.retired.push_back({std::move(map), iteration});
  }

  void freeRetired(Analyzer& analyzer)
  {
    auto now = sampler::iteration();
    std::erase_if(analyzer.retired, [&](auto& retired)
                  {
                    if (sampler::alive() && now < retired.iteration + 2)
                      return false;
                    modules::release(*retired.map);
                    return true; });
  }

  // The map goes: the sampler is told first, then the modules are unpinned. A module that only the pin
  // kept loaded unloads right here, while nothing reads it
  void dropMap(Analyzer& analyzer)
  {
    if (!analyzer.map)
      return;
    s_map.store(nullptr, std::memory_order_seq_cst);
    letGo(analyzer, std::move(analyzer.map), sampler::iteration());
    analyzer.handles.clear();
  }

  // A new map when a module was loaded or unloaded, the mods list came, generated code was learned,
  // or the map is a few seconds old. None while sampling is off: no module stays pinned then
  void refreshMap(Analyzer& analyzer)
  {
    if (!sampler::running())
    {
      dropMap(analyzer);
      return;
    }
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
    auto now = std::chrono::steady_clock::now();
    bool unpin = analyzer.map && now - analyzer.mapTime >= UNPIN_EVERY;
    if (analyzer.map && !unpin && handles == analyzer.handles && version == analyzer.modsVersion && learned.empty())
      return;

    if (unpin)
    {
      // The sampler pauses until the new map: without the pins, a module may be gone
      dropMap(analyzer);
      handles = modules::loaded();
      analyzer.mapTime = now;
    }
    else if (!analyzer.map)
      analyzer.mapTime = now;

    // The new map in, then the old one out once the sampler can't be reading it: no pause
    auto map = modules::build(handles, mods, analyzer.learned, analyzer.buckets);
    s_map.store(map.get(), std::memory_order_seq_cst);
    if (analyzer.map)
      letGo(analyzer, std::move(analyzer.map), sampler::iteration());
    analyzer.map = std::move(map);
    analyzer.handles = std::move(handles);
    analyzer.modsVersion = version;
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
      keepForStartup(analyzer, *sample);
      window.stats.add(*sample, toMicros);
      analyzer.held.push_back(*sample);
      if (analyzer.held.size() > HELD_SAMPLES)
        analyzer.held.pop_front();
      // A few stacks to look at, spread over the second
      if (s_logStacks.load(std::memory_order_relaxed) && window.stats.samples % 300 == 1)
        log::info("Stack: {}", describeStack(*sample, *analyzer.map));
      s_ring.pop();
    }
  }

  void run()
  {
    utils::thread::setName("Mod Profiler");
    symbols::cleanUp();
    Analyzer analyzer;

    auto last = std::chrono::steady_clock::now();
    auto next = last + std::chrono::seconds(1);
    auto frames = s_frames.load();
    auto dropped = sampler::dropped();
    auto samplerCpu = sampler::cpuTime();
    auto analyzerCpu = sampler::threadCpuTime();
    // The sampler waits for the first map
    refreshMap(analyzer);

    while (true)
    {
      std::this_thread::sleep_until(next);
      next += std::chrono::seconds(1);

      // Analyzing with the map the samples were taken with, then catching up with new modules
      Window window;
      refreshAttribution(analyzer);
      // The frames first: their samples are in the ring by the time they end
      while (auto end = s_frameEnds.peek())
      {
        analyzer.clock.add(*end);
        analyzer.lastFrameEnd = *end;
        s_frameEnds.pop();
      }
      // Frame ends the ring had no room for: two ends with a lost one between would look like one long
      // frame, so the clock starts again from the last end
      if (auto lost = s_frameEndsLost.load(); lost != analyzer.frameEndsLost)
      {
        analyzer.frameEndsLost = lost;
        analyzer.clock.restart();
      }
      if (analyzer.map)
      {
        analyze(analyzer, window);
        findSlowFrames(analyzer);
        watchForHang(analyzer);
        countStartup(analyzer);
      }
      exportIfAsked(analyzer);
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
      // A restarted sampler starts its count again
      auto samplerCpuNow = sampler::cpuTime();
      window.stats.samplerCpuMillis = samplerCpuNow >= samplerCpu ? (samplerCpuNow - samplerCpu) / 10'000.0 : 0;
      samplerCpu = samplerCpuNow;
      auto analyzerCpuNow = sampler::threadCpuTime();
      window.stats.analyzerCpuMillis = (analyzerCpuNow - analyzerCpu) / 10'000.0;
      analyzerCpu = analyzerCpuNow;

      if (s_logDiagnostics.load(std::memory_order_relaxed) && (sampler::running() || window.stats.samples > 0))
        log::info("{}", diagnosticsLine(window, analyzer.buckets, 5));

      auto merge = [](Window& into, Window const& from)
      {
        into.profile.merge(from.profile);
        into.stats.merge(from.stats);
        into.seconds += from.seconds;
        into.frames += from.frames;
      };
      if (s_resetSession.exchange(false))
      {
        analyzer.session = {};
        analyzer.slowFrames.clear();
      }
      merge(analyzer.session, window);
      merge(analyzer.span, window);

      // A new scene: the one left is summed up in the log
      std::string scene;
      {
        std::lock_guard lock(s_mutex);
        scene = s_scene;
      }
      if (scene != analyzer.scene)
      {
        if (!analyzer.scene.empty() && analyzer.sceneWindow.seconds >= MIN_SCENE_SECONDS && analyzer.sceneWindow.profile.samples() > 0)
          log::info("Left {}: {}", analyzer.scene, summaryLine(analyzer.sceneWindow, analyzer.buckets, 5));
        analyzer.scene = scene;
        analyzer.sceneWindow = {};
      }
      merge(analyzer.sceneWindow, window);
      std::optional<std::string> spanLabel;
      {
        std::lock_guard lock(s_mutex);
        spanLabel.swap(s_spanLabel);
      }
      if (spanLabel)
      {
        log::info("Since the last marker{}: {}, {} slow frames", spanLabel->empty() ? "" : " (" + *spanLabel + ")",
                  summaryLine(analyzer.span, analyzer.buckets, 5), analyzer.spanSlowFrames);
        Baseline baseline;
        {
          std::lock_guard lock(s_mutex);
          baseline = s_baseline;
        }
        if (!baseline.empty())
          log::info("Since the last marker, {}", comparisonLine(analyzer.span, analyzer.buckets, baseline, 5));
        analyzer.span = {};
        analyzer.spanSlowFrames = 0;
      }
      analyzer.recent.push_back(window);
      while (analyzer.recent.size() > profiler::RECENT_SECONDS)
        analyzer.recent.pop_front();
      Window recent;
      for (auto const& second : analyzer.recent)
        merge(recent, second);
      auto names = nameTopPlaces(analyzer, recent);
      // Made outside the lock, swapped in under it
      auto snapshot = std::make_shared<profiler::Snapshot>();
      snapshot->last = std::move(window);
      snapshot->recent = std::move(recent);
      snapshot->session = analyzer.session;
      snapshot->buckets = analyzer.buckets;
      snapshot->names = std::move(names);
      snapshot->slowFrames.assign(analyzer.slowFrames.begin(), analyzer.slowFrames.end());
      snapshot->scene = analyzer.sceneWindow;
      snapshot->sceneName = analyzer.scene;
      if (analyzer.startupDone)
      {
        snapshot->startup = analyzer.startup;
        snapshot->startupReady = true;
      }
      // The old one is freed after the lock: its windows' maps take a while
      std::shared_ptr<profiler::Snapshot const> old = std::move(snapshot);
      {
        std::lock_guard lock(s_mutex);
        std::swap(s_snapshot, old);
      }
      old.reset();
      freeRetired(analyzer);

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
  s_spikeMillis = options.spikeMillis;
  s_logSpikes = options.logSpikes;
  symbols::setEnabled(options.symbols);
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

void profiler::exiting() { s_exiting = true; }

void profiler::setScene(std::string name)
{
  std::lock_guard lock(s_mutex);
  s_scene = std::move(name);
}

void profiler::frame()
{
  s_frames.fetch_add(1, std::memory_order_relaxed);
  if (auto slot = s_frameEnds.reserve())
  {
    *slot = sampler::now();
    s_frameEnds.commit();
  }
  else
    s_frameEndsLost.fetch_add(1, std::memory_order_relaxed);
}

void profiler::hooksReady()
{
  s_hooksReadyTicks = sampler::now();
  s_hooksReadyTime = std::chrono::steady_clock::now();
  s_hooksReady = true;
}

void profiler::markSpan(std::string label)
{
  std::lock_guard lock(s_mutex);
  s_spanLabel = std::move(label);
}

void profiler::setNamesWanted(bool wanted) { s_namesWanted = wanted; }

void profiler::resetSession() { s_resetSession = true; }

void profiler::setBaseline(Baseline baseline)
{
  std::lock_guard lock(s_mutex);
  s_baseline = std::move(baseline);
}

void profiler::requestExport(bool startup, std::function<void(std::string const& file, std::string const& error)> done)
{
  std::lock_guard lock(s_mutex);
  s_exportDone = std::move(done);
  s_exportStartup = startup;
}

std::shared_ptr<profiler::Snapshot const> profiler::snapshot()
{
  std::lock_guard lock(s_mutex);
  return s_snapshot;
}
