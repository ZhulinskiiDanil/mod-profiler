#pragma once

// Which mod a sample's time belongs to, without Windows or Geode in it.
//
// Inclusive time counts a mod whenever its code is on the stack. That's what a mod costs if it's
// removed, but a mod hooking something that wraps the whole frame is on every stack and gets 100%.
// Own time gives each sample to one mod at most: the innermost one doing its own work, not passing
// a call on to the original function (or to the next mod's hook of it). Samples where the thread
// waits (a frame limiter's sleep, vsync) are idle, nobody's. So are the samples in the game's loop
// between frames: a frame limiter that spins on the clock instead of sleeping
#include "ModuleMap.hpp"
#include "Sample.hpp"

#include <cstdint>
#include <utility>
#include <vector>

class HookIndex
{
public:
  // `owner` hooks the function at `target` with `detour`
  void add(uint32_t owner, uintptr_t target, uintptr_t detour);
  // Sorts what was added, before the lookups
  void finish();
  size_t size() const { return m_targets.size(); }

  // Whether a frame of `owner` calling the function starting at `callee` only passes the call on:
  // `callee` is a function it hooks (the original), or another mod's detour of one
  bool passesOn(uint32_t owner, uintptr_t callee) const;
  // The detours of every hook of the function at `target`
  std::vector<uintptr_t> detoursOf(uintptr_t target) const;
  // The function a detour hooks, or 0
  uintptr_t targetOf(uintptr_t detour) const;

private:
  // (target, owner) and (detour, target), sorted
  std::vector<std::pair<uintptr_t, uint32_t>> m_targets;
  std::vector<std::pair<uintptr_t, uintptr_t>> m_detours;
};

struct Attribution
{
  HookIndex hooks;
  // Where the thread waits in the kernel: the system call stubs of the wait functions, sorted
  std::vector<std::pair<uintptr_t, uintptr_t>> waits;
  // The game's loop (CCApplication::run), and what it calls that is part of a frame
  // (CCDirector::mainLoop, CCEGLView::pollEvents). `finish` adds the detours of their hooks
  std::vector<uintptr_t> loop;
  std::vector<uintptr_t> frame;
  // Geode's own bucket: its functions that aren't detours stand between a mod's detour and the
  // original when the mod calls the original through it, and are the hooks' cost, not Geode's
  int loader = -1;

  void finish();
  bool isWait(uintptr_t address) const;
};

struct Verdict
{
  bool idle = false;
  // In the game's loop but not in a frame
  bool between = false;
  // The mod the sample is the own time of, or -1
  int owner = -1;
  // Where in the mod: the start of the function its frame is in (the address itself without an entry),
  // and the function hooked by the mod's detour the frame is under, or 0
  uintptr_t function = 0;
  uintptr_t hook = 0;
};

Verdict attribute(Sample const& sample, ModuleMap const& map, Attribution const& attribution);
