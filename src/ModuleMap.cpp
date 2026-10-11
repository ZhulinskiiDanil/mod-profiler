#include "ModuleMap.hpp"

#include <algorithm>

// ! --- Buckets --- !

Buckets::Buckets()
{
  m_list = {
      {"GeometryDash.exe", "Geometry Dash"},
      {"libcocos2d.dll", "Cocos2d"},
      {"other", "Other"},
  };
}

uint32_t Buckets::index(std::string const& id, std::string const& name)
{
  auto found = find(id);
  if (found >= 0)
    return static_cast<uint32_t>(found);
  m_list.push_back({id, name.empty() ? id : name});
  return static_cast<uint32_t>(m_list.size() - 1);
}

int Buckets::find(std::string const& id) const
{
  for (size_t i = 0; i < m_list.size(); i++)
    if (m_list[i].id == id)
      return static_cast<int>(i);
  return -1;
}

void Buckets::rename(uint32_t index, std::string name)
{
  if (index < m_list.size() && !name.empty())
    m_list[index].name = std::move(name);
}

// ! --- Map --- !

ModuleMap::ModuleMap(std::vector<MappedModule> modules, std::vector<CodeRange> code)
    : m_modules(std::move(modules)), m_code(std::move(code))
{
  std::erase_if(m_code, [&](CodeRange const& range)
                { return range.end <= range.begin || range.module >= m_modules.size(); });
  std::sort(m_code.begin(), m_code.end(), [](auto const& a, auto const& b)
            { return a.begin < b.begin; });

  for (uint32_t i = 0; i < m_modules.size(); i++)
    m_byBase.push_back(i);
  std::sort(m_byBase.begin(), m_byBase.end(), [&](uint32_t a, uint32_t b)
            { return m_modules[a].base < m_modules[b].base; });
}

int ModuleMap::findCode(uintptr_t address) const noexcept
{
  // The last range starting at or before the address
  size_t low = 0, high = m_code.size();
  while (low < high)
  {
    auto middle = low + (high - low) / 2;
    if (m_code[middle].begin <= address)
      low = middle + 1;
    else
      high = middle;
  }
  if (low == 0)
    return -1;
  auto const& range = m_code[low - 1];
  return address < range.end ? static_cast<int>(range.module) : -1;
}

int ModuleMap::findImage(uintptr_t address) const noexcept
{
  size_t low = 0, high = m_byBase.size();
  while (low < high)
  {
    auto middle = low + (high - low) / 2;
    if (m_modules[m_byBase[middle]].base <= address)
      low = middle + 1;
    else
      high = middle;
  }
  if (low == 0)
    return -1;
  auto const& module = m_modules[m_byBase[low - 1]];
  return address - module.base < module.size ? static_cast<int>(m_byBase[low - 1]) : -1;
}

uint32_t ModuleMap::bucketOf(uintptr_t address) const noexcept
{
  auto index = findImage(address);
  return index < 0 ? bucket::OTHER : m_modules[index].bucket;
}

// ! --- Function tables --- !

long findFunction(uint32_t const* functions, size_t count, uint32_t rva) noexcept
{
  // The last entry beginning at or before the RVA, then whether it reaches it
  size_t low = 0, high = count;
  while (low < high)
  {
    auto middle = low + (high - low) / 2;
    if (functions[middle * 3] <= rva)
      low = middle + 1;
    else
      high = middle;
  }
  if (low == 0)
    return -1;
  auto entry = low - 1;
  return rva < functions[entry * 3 + 1] ? static_cast<long>(entry) : -1;
}
