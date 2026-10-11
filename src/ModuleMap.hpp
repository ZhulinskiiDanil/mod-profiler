#pragma once

// Which loaded module an address belongs to, and its function table, without Windows or Geode in it.
// The sampler reads a map while the main thread is suspended, so the lookups never allocate or lock.
// A map is built once and not changed afterwards: a new module list makes a new map
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// The buckets the time is split into: the game, cocos, everything else, the code Geode's hooks
// generate (handlers and trampolines), then one for each mod
namespace bucket
{
  inline constexpr uint32_t GAME = 0;
  inline constexpr uint32_t COCOS = 1;
  inline constexpr uint32_t OTHER = 2;
  inline constexpr uint32_t HOOKS = 3;
  // Geode itself, a mod like the others but always there: some rules need to know it
  inline constexpr uint32_t LOADER = 4;
  inline constexpr uint32_t FIRST_MOD = 4;
} // namespace bucket

struct Bucket
{
  // The mod's ID, or the file name for the fixed ones
  std::string id;
  std::string name;
};

// Every bucket seen so far. Only grows, so a bucket's index stays the same for the whole session
class Buckets
{
public:
  Buckets();
  // The index of the bucket with this ID, added if it's new
  uint32_t index(std::string const& id, std::string const& name);
  // The index of the bucket with this ID, or -1
  int find(std::string const& id) const;
  void rename(uint32_t index, std::string name);
  Bucket const& operator[](uint32_t index) const { return m_list[index]; }
  size_t size() const { return m_list.size(); }

private:
  std::vector<Bucket> m_list;
};

struct MappedModule
{
  uintptr_t base = 0;
  uintptr_t size = 0;
  // The image's RUNTIME_FUNCTION table: begin, end and unwind info RVAs, sorted by begin
  uint32_t const* functions = nullptr;
  size_t functionCount = 0;
  // For generated code, which has no table of its own: the entries learned so far, sorted.
  // The map points `functions` at it
  std::vector<uint32_t> ownFunctions;
  uint32_t bucket = bucket::OTHER;
  // The file name, for the reports
  std::string name;
  // Whatever the builder needs to let go of the module later (a pinned HMODULE)
  void* handle = nullptr;
};

struct CodeRange
{
  uintptr_t begin = 0;
  uintptr_t end = 0;
  uint32_t module = 0;
};

class ModuleMap
{
public:
  // Ranges may come in any order, they get sorted
  ModuleMap(std::vector<MappedModule> modules, std::vector<CodeRange> code);

  // The module with executable code at this address, or -1
  int findCode(uintptr_t address) const noexcept;
  // The module whose image holds this address, or -1
  int findImage(uintptr_t address) const noexcept;
  // The bucket the address counts for: OTHER outside every module
  uint32_t bucketOf(uintptr_t address) const noexcept;
  // Where the function holding the address starts, from the function table: 0 for a leaf function
  // (no entry) or outside every module. Reads the image's unwind info, so not while sampling. Cached:
  // only one thread may call it (the analyzer; the sampler never does)
  uintptr_t functionStart(uintptr_t address) const;

  MappedModule const& module(size_t index) const { return m_modules[index]; }
  size_t moduleCount() const { return m_modules.size(); }
  std::vector<MappedModule> const& modules() const { return m_modules; }

private:
  std::vector<MappedModule> m_modules;
  std::vector<CodeRange> m_code;
  // Module indices sorted by base
  std::vector<uint32_t> m_byBase;
  // functionStart's answers: the same return addresses come back in every sample
  mutable std::unordered_map<uintptr_t, uintptr_t> m_starts;
};

// The index of the entry in a RUNTIME_FUNCTION table (3 RVAs an entry) whose range holds `rva`, or -1
long findFunction(uint32_t const* functions, size_t count, uint32_t rva) noexcept;
