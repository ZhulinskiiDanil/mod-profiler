#include "Sampler.hpp"

#include "Code.hpp"
#include "Unwind.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <thread>

namespace
{
  HANDLE s_mainThread = nullptr;
  NT_TIB const* s_mainTib = nullptr;

  // Leaked on purpose: a joinable std::thread destroyed at exit would terminate the game
  std::thread* s_thread = nullptr;
  std::atomic<bool> s_running{false};
  std::atomic<bool> s_alive{false};
  std::atomic<uint64_t> s_iteration{0};
  std::atomic<uint64_t> s_dropped{0};
  // Kept up to date by the thread itself every few hundred samples
  std::atomic<uint64_t> s_cpuTime{0};
  // In the waitable timer's units of 100 ns
  std::atomic<int64_t> s_interval{10'000};
  std::atomic<ZoneStack const*> s_zones{nullptr};
  int64_t s_frequency = 1;

  // How far a scan looks for the next return address, and how many scans a sample gets
  constexpr uintptr_t SCAN_BYTES = 4096;
  constexpr int MAX_SCANS = 4;

  // ! --- Walking the stack (all of it runs while the main thread is suspended) --- !

  // The part of the main thread's stack that can be read: from where it is now up to its base
  struct Bounds
  {
    uintptr_t low = 0;
    uintptr_t high = 0;

    bool holds(uintptr_t address, uint64_t size) const noexcept
    {
      return address >= low && address < high && size <= high - address;
    }
  };

  // After code without unwind info (a hook handler, another library's trampoline): the next value up
  // the stack that points right after a call in a known module is taken as the return address
  bool scanForReturn(CONTEXT& context, ModuleMap const& map, Bounds bounds) noexcept
  {
    auto start = (context.Rsp + 7) & ~uintptr_t(7);
    auto end = std::min(bounds.high, start + SCAN_BYTES);
    for (auto slot = start; slot >= bounds.low && slot + 8 <= end; slot += 8)
    {
      auto value = *reinterpret_cast<uintptr_t const*>(slot);
      auto index = map.findCode(value);
      if (index < 0)
        continue;
      // The bytes before it are in the same image, which is pinned and mapped whole
      auto available = std::min<uintptr_t>(8, value - map.module(index).base);
      if (!code::isCallBefore(reinterpret_cast<uint8_t const*>(value), available))
        continue;
      context.Rip = value;
      context.Rsp = slot + 8;
      return true;
    }
    return false;
  }

  // One frame up, with the function table of the module the address is in. False when it can't go on
  // `first`: the frame the thread is in. The others are return addresses: their thread is in a call, never
  // in their epilog, whatever the bytes after the call look like
  bool unwindFrame(CONTEXT& context, ModuleMap const& map, int index, uintptr_t lookup, Bounds bounds, bool first) noexcept
  {
    auto const& module = map.module(index);
    auto sp = context.Rsp;
    auto rva = static_cast<uint32_t>(lookup - module.base);
    auto entry = findFunction(module.functions, module.functionCount, rva);

    if (entry < 0)
    {
      // A leaf function: no frame, the return address is right on top
      if (!bounds.holds(sp, 8))
        return false;
      context.Rip = *reinterpret_cast<uintptr_t const*>(sp);
      context.Rsp = sp + 8;
      return true;
    }

    auto function = module.functions + entry * 3;
    auto begin = function[0];
    // An entry can point at the entry with the real info
    if (function[2] & 1)
    {
      auto target = function[2] & ~1u;
      if (static_cast<uint64_t>(target) + 12 > module.size)
        return false;
      function = reinterpret_cast<uint32_t const*>(module.base + target);
    }

    auto image = reinterpret_cast<uint8_t const*>(module.base);

    // Generated hook code: past its prolog and before its epilog, RBP is the exact frame. Its unwind info
    // leaves out what a calling convention adds to the stack, and the frame above would be read wrong
    if (module.bucket == bucket::HOOKS)
    {
      auto prolog = unwind::framePointerProlog(image, module.size, function[2]);
      auto offset = rva - begin;
      auto pc = reinterpret_cast<uint8_t const*>(context.Rip);
      auto available = std::min<uintptr_t>(8, module.base + module.size - context.Rip);
      if (prolog >= 0 && offset >= static_cast<uint32_t>(prolog) && !(first && unwind::isFramePointerEpilog(pc, available)) &&
          bounds.holds(context.Rbp, 16) && context.Rbp >= sp)
      {
        auto frame = reinterpret_cast<uintptr_t const*>(context.Rbp);
        context.Rip = frame[1];
        context.Rsp = context.Rbp + 16;
        context.Rbp = frame[0];
        return true;
      }
    }

    auto extent = unwind::frameExtent(image, module.size, function[2], rva - begin);
    if (!extent.ok || !bounds.holds(sp, 8))
      return false;
    // In an epilog RtlVirtualUnwind reads from RSP, as far as the code says (its add, its pops, the
    // return address), whatever the unwind info says: that's checked too
    auto pc = reinterpret_cast<uint8_t const*>(context.Rip);
    auto codeLeft = std::min<uintptr_t>(32, module.base + module.size - context.Rip);
    auto reach = unwind::epilogReach(pc, codeLeft);
    if (reach && !bounds.holds(sp, reach))
      return false;

    auto frame = sp;
    auto bytes = extent.bytes;
    // In the pops and ret of the epilog the frame register may be the caller's already: only RSP counts
    if (extent.usesFrameRegister && !unwind::isEpilogTail(pc, codeLeft))
      frame = (&context.Rax)[extent.frameRegister] - extent.frameOffset;
    else if (extent.usesFrameRegister)
      bytes = reach;
    if (!bounds.holds(frame, bytes))
      return false;

    PVOID handlerData = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, module.base, context.Rip, reinterpret_cast<PRUNTIME_FUNCTION>(const_cast<uint32_t*>(function)),
                     &context, &handlerData, &establisher, nullptr);
    return true;
  }

  void walk(CONTEXT& context, ModuleMap const& map, Bounds bounds, Sample& sample) noexcept
  {
    int scans = 0;
    while (context.Rip)
    {
      if (sample.depth == MAX_FRAMES)
      {
        sample.flags |= SAMPLE_TRUNCATED;
        return;
      }
      auto pc = context.Rip;
      auto sp = context.Rsp;
      sample.frames[sample.depth++] = pc;
      if (sp < bounds.low || sp >= bounds.high)
        return;

      // A return address is after its call, which may be the last instruction of a function
      auto lookup = sample.depth == 1 ? pc : pc - 1;
      auto index = map.findCode(lookup);
      if (index < 0)
      {
        if (scans++ >= MAX_SCANS || !scanForReturn(context, map, bounds))
          return;
        sample.flags |= SAMPLE_SCANNED;
        continue;
      }
      if (!unwindFrame(context, map, index, lookup, bounds, sample.depth == 1))
        return;
      // Every frame is above the last one, or the walk is lost
      if (context.Rsp <= sp || context.Rsp > bounds.high)
        return;
    }
  }

  // The checks keep the walk inside the stack and the pinned images, this is only the last net:
  // a fault would go through every mod's vectored exception handlers on this thread
  bool guardedWalk(CONTEXT& context, ModuleMap const& map, Bounds bounds, Sample& sample) noexcept
  {
    __try
    {
      walk(context, map, bounds, sample);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // ! --- The thread --- !

  // Until `next` (in QueryPerformanceCounter ticks), then moves it on by one interval. A wait that
  // was late doesn't make the next ones shorter to catch up: they'd sample one moment many times
  void wait(HANDLE timer, int64_t& next)
  {
    auto interval = s_interval.load(std::memory_order_relaxed) * s_frequency / 10'000'000;
    auto now = sampler::now();
    if (next - now > interval || now - next > interval)
      next = now + interval;
    auto remaining = (next - now) * 10'000'000 / s_frequency;
    next += interval;
    if (remaining <= 0)
      return;
    if (!timer)
    {
      Sleep(1);
      return;
    }
    LARGE_INTEGER due;
    due.QuadPart = -remaining;
    if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
    {
      Sleep(1);
      return;
    }
    WaitForSingleObject(timer, INFINITE);
  }

  void run(Ring<Sample>* ring, std::atomic<ModuleMap const*>* currentMap)
  {
    SetThreadDescription(GetCurrentThread(), L"Mod Profiler sampler");
    // Not taken away from the CPU in the middle of a sample while the main thread waits on it
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    // A 1 ms wait without changing the system timer resolution
    auto timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

    CONTEXT context{};
    int64_t next = sampler::now();
    while (s_running.load(std::memory_order_relaxed))
    {
      wait(timer, next);

      // seq_cst with the analyzer's store and its read of the iteration: a map it took away is never
      // loaded after the iteration it waited for
      auto map = currentMap->load(std::memory_order_seq_cst);
      Sample* slot = map ? ring->reserve() : nullptr;
      if (!slot)
      {
        if (map)
          s_dropped.fetch_add(1, std::memory_order_relaxed);
        s_iteration.fetch_add(1, std::memory_order_seq_cst);
        continue;
      }
      slot->depth = 0;
      slot->flags = 0;
      slot->zone = 0;
      auto zones = s_zones.load(std::memory_order_acquire);
      Bounds bounds{.high = reinterpret_cast<uintptr_t>(s_mainTib->StackBase)};

      LARGE_INTEGER start, stopped, end;
      QueryPerformanceCounter(&start);
      if (SuspendThread(s_mainThread) == static_cast<DWORD>(-1))
      {
        s_iteration.fetch_add(1, std::memory_order_seq_cst);
        continue;
      }

      // ! --- Suspended: no allocations, no logs, no locks until ResumeThread --- !
      // SuspendThread only asks, GetThreadContext waits until the thread is really stopped
      context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
      auto gotContext = GetThreadContext(s_mainThread, &context);
      QueryPerformanceCounter(&stopped);
      // A plain read of this mod's memory: the stack's depth, then one id
      if (zones)
        slot->zone = zones->top();
      if (gotContext)
      {
        // The stack can be read from where the thread is up to its base. Elsewhere (a fiber), only RIP
        bounds.low = context.Rsp;
        auto limit = reinterpret_cast<uintptr_t>(s_mainTib->StackLimit);
        if (context.Rsp < limit || context.Rsp >= bounds.high)
          bounds = {};
        if (!guardedWalk(context, *map, bounds, *slot))
          slot->flags |= SAMPLE_NO_CONTEXT;
      }
      else
        slot->flags |= SAMPLE_NO_CONTEXT;
      ResumeThread(s_mainThread);
      // ! --- Resumed --- !

      QueryPerformanceCounter(&end);
      slot->time = start.QuadPart;
      slot->suspendTicks = static_cast<uint32_t>(std::min<int64_t>(end.QuadPart - start.QuadPart, UINT32_MAX));
      slot->contextTicks = static_cast<uint32_t>(std::min<int64_t>(stopped.QuadPart - start.QuadPart, UINT32_MAX));
      ring->commit();
      if (s_iteration.fetch_add(1, std::memory_order_seq_cst) % 64 == 0)
        s_cpuTime.store(sampler::threadCpuTime(), std::memory_order_relaxed);
    }

    if (timer)
      CloseHandle(timer);
    s_alive.store(false, std::memory_order_release);
  }
} // namespace

bool sampler::captureMainThread()
{
  if (s_mainThread)
    return true;
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  s_frequency = frequency.QuadPart;

  HANDLE handle = nullptr;
  auto access = THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION;
  if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle, access, FALSE, 0))
    return false;
  s_mainThread = handle;
  s_mainTib = reinterpret_cast<NT_TIB const*>(NtCurrentTeb());
  return true;
}

bool sampler::start(Ring<Sample>& ring, std::atomic<ModuleMap const*>& map, double intervalMillis)
{
  if (!s_mainThread || s_running.exchange(true))
    return false;
  setInterval(intervalMillis);
  // A thread from an earlier start that's still finishing
  if (s_thread)
  {
    s_thread->join();
    delete s_thread;
  }
  s_alive.store(true, std::memory_order_release);
  s_thread = new std::thread(run, &ring, &map);
  return true;
}

void sampler::stop()
{
  if (!s_running.exchange(false) || !s_thread)
    return;
  s_thread->join();
  delete s_thread;
  s_thread = nullptr;
}

bool sampler::running() { return s_running.load(); }
bool sampler::alive() { return s_alive.load(std::memory_order_acquire); }

uint64_t sampler::iteration() { return s_iteration.load(std::memory_order_seq_cst); }
uint64_t sampler::dropped() { return s_dropped.load(std::memory_order_relaxed); }
uint64_t sampler::cpuTime() { return s_cpuTime.load(std::memory_order_relaxed); }

uint64_t sampler::threadCpuTime()
{
  FILETIME creation, exit, kernel, user;
  if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user))
    return 0;
  auto value = [](FILETIME time)
  { return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
  return value(kernel) + value(user);
}

void sampler::setInterval(double millis)
{
  s_interval.store(static_cast<int64_t>(std::clamp(millis, 0.1, 100.0) * 10'000), std::memory_order_relaxed);
}

int64_t sampler::ticksPerSecond() { return s_frequency; }

void sampler::setZones(ZoneStack const* zones) { s_zones.store(zones, std::memory_order_release); }

int64_t sampler::now()
{
  LARGE_INTEGER value;
  QueryPerformanceCounter(&value);
  return value.QuadPart;
}
