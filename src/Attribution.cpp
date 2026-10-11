#include "Attribution.hpp"

#include <algorithm>

// ! --- Hooks --- !

void HookIndex::add(uint32_t owner, uintptr_t target, uintptr_t detour)
{
  m_targets.push_back({target, owner});
  if (detour)
    m_detours.push_back({detour, target});
}

void HookIndex::finish()
{
  std::sort(m_targets.begin(), m_targets.end());
  m_targets.erase(std::unique(m_targets.begin(), m_targets.end()), m_targets.end());
  std::sort(m_detours.begin(), m_detours.end());
  m_detours.erase(std::unique(m_detours.begin(), m_detours.end()), m_detours.end());
}

bool HookIndex::passesOn(uint32_t owner, uintptr_t callee) const
{
  auto hooks = [&](uintptr_t target)
  { return std::binary_search(m_targets.begin(), m_targets.end(), std::pair{target, owner}); };
  if (hooks(callee))
    return true;
  // The next detour in the chain of a function this mod hooks too
  auto detour = std::lower_bound(m_detours.begin(), m_detours.end(), std::pair<uintptr_t, uintptr_t>{callee, 0});
  for (; detour != m_detours.end() && detour->first == callee; detour++)
    if (hooks(detour->second))
      return true;
  return false;
}

std::vector<uintptr_t> HookIndex::detoursOf(uintptr_t target) const
{
  std::vector<uintptr_t> detours;
  for (auto const& [detour, hooked] : m_detours)
    if (hooked == target)
      detours.push_back(detour);
  return detours;
}

uintptr_t HookIndex::targetOf(uintptr_t detour) const
{
  auto found = std::lower_bound(m_detours.begin(), m_detours.end(), std::pair<uintptr_t, uintptr_t>{detour, 0});
  return found != m_detours.end() && found->first == detour ? found->second : 0;
}

// ! --- Waits --- !

void Attribution::finish()
{
  hooks.finish();
  std::sort(waits.begin(), waits.end());
  // A mod's detour of a function counts as the function
  for (auto* functions : {&loop, &frame})
  {
    auto originals = *functions;
    for (auto original : originals)
      for (auto detour : hooks.detoursOf(original))
        functions->push_back(detour);
    std::erase(*functions, uintptr_t(0));
    std::sort(functions->begin(), functions->end());
  }
}

bool Attribution::isWait(uintptr_t address) const
{
  auto after = std::upper_bound(waits.begin(), waits.end(), std::pair<uintptr_t, uintptr_t>{address, UINTPTR_MAX});
  return after != waits.begin() && address < std::prev(after)->second;
}

// ! --- One sample --- !

namespace
{
  // The address to look a frame's function up by: a return address is after its call, which may
  // be the last instruction of the function
  uintptr_t lookupAddress(Sample const& sample, int frame)
  {
    return frame == 0 ? sample.frames[0] : sample.frames[frame] - 1;
  }

  // Called by a hook handler, but not one of the hooks' detours: the hook library's own bookkeeping
  // (TulipHook is in Geode.dll), which is the cost of the hooks, not of the mod it's in
  bool isHookPlumbing(Sample const& sample, ModuleMap const& map, Attribution const& attribution, int frame)
  {
    if (frame + 1 >= sample.depth || map.bucketOf(sample.frames[frame + 1]) != bucket::HOOKS)
      return false;
    auto start = map.functionStart(lookupAddress(sample, frame));
    return start && !attribution.hooks.targetOf(start);
  }

  // A frame between a call and where it goes: a hook handler, or Geode's code that isn't a detour
  bool isBetween(Sample const& sample, ModuleMap const& map, Attribution const& attribution, int frame)
  {
    auto bucket = map.bucketOf(sample.frames[frame]);
    if (bucket == bucket::HOOKS)
      return true;
    if (attribution.loader < 0 || bucket != static_cast<uint32_t>(attribution.loader))
      return false;
    auto start = map.functionStart(lookupAddress(sample, frame));
    return start && !attribution.hooks.targetOf(start);
  }

  bool passesOn(Sample const& sample, ModuleMap const& map, Attribution const& attribution, int frame, uint32_t owner)
  {
    // What the frame calls, past the hook handlers (and Geode's helpers) in between
    int callee = frame - 1;
    while (callee >= 0 && isBetween(sample, map, attribution, callee))
      callee--;
    if (callee < 0)
      return false;

    auto start = map.functionStart(lookupAddress(sample, callee));
    if (start && attribution.hooks.passesOn(owner, start))
      return true;

    // Called straight from the game's code and calling straight into it: a patched call, like
    // Geode's own start under the game's main function. A mod's own code called by the game is
    // called by Cocos (a node's draw) or through a hook
    if (frame + 1 < sample.depth && callee == frame - 1 && map.bucketOf(sample.frames[frame + 1]) == bucket::GAME &&
        map.bucketOf(sample.frames[callee]) == bucket::GAME)
      return true;
    return false;
  }
} // namespace

Verdict attribute(Sample const& sample, ModuleMap const& map, Attribution const& attribution)
{
  Verdict verdict;
  if (sample.depth == 0)
    return verdict;
  if (attribution.isWait(sample.frames[0]))
  {
    verdict.idle = true;
    return verdict;
  }

  // Between two frames: in the loop, in nothing of a frame
  if (!attribution.loop.empty())
  {
    bool inLoop = false, inFrame = false;
    for (int i = 0; i < sample.depth && i < MAX_FRAMES && !inFrame; i++)
    {
      auto start = map.functionStart(lookupAddress(sample, i));
      if (!start)
        continue;
      inFrame = std::binary_search(attribution.frame.begin(), attribution.frame.end(), start);
      inLoop = inLoop || std::binary_search(attribution.loop.begin(), attribution.loop.end(), start);
    }
    if (inLoop && !inFrame)
    {
      verdict.between = true;
      return verdict;
    }
  }

  for (int i = 0; i < sample.depth && i < MAX_FRAMES; i++)
  {
    auto owner = map.bucketOf(sample.frames[i]);
    // The thread in a hook handler's own code: the cost of the hooks
    if (i == 0 && owner == bucket::HOOKS)
    {
      verdict.owner = static_cast<int>(bucket::HOOKS);
      verdict.function = sample.frames[0];
      break;
    }
    if (owner < bucket::FIRST_MOD || passesOn(sample, map, attribution, i, owner))
      continue;
    if (isHookPlumbing(sample, map, attribution, i))
    {
      verdict.owner = static_cast<int>(bucket::HOOKS);
      auto start = map.functionStart(lookupAddress(sample, i));
      verdict.function = start ? start : sample.frames[i];
      break;
    }
    // Geode's helper that a mod's detour calls the original through: the mod passes on, so does it
    if (attribution.loader >= 0 && owner == static_cast<uint32_t>(attribution.loader) && isBetween(sample, map, attribution, i))
    {
      int caller = i + 1;
      while (caller < sample.depth && caller < MAX_FRAMES && map.bucketOf(sample.frames[caller]) == bucket::HOOKS)
        caller++;
      if (caller < sample.depth && caller < MAX_FRAMES)
      {
        auto callerOwner = map.bucketOf(sample.frames[caller]);
        if (callerOwner >= bucket::FIRST_MOD && callerOwner != owner && passesOn(sample, map, attribution, caller, callerOwner))
          continue;
      }
    }
    verdict.owner = static_cast<int>(owner);
    auto start = map.functionStart(lookupAddress(sample, i));
    verdict.function = start ? start : sample.frames[i];

    // The hook it's working for: the nearest of the mod's detours from here out
    for (int frame = i; frame < sample.depth && frame < MAX_FRAMES && !verdict.hook; frame++)
    {
      if (map.bucketOf(sample.frames[frame]) != owner)
        continue;
      if (auto function = map.functionStart(lookupAddress(sample, frame)))
        verdict.hook = attribution.hooks.targetOf(function);
    }
    break;
  }
  return verdict;
}
