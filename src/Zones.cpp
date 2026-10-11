#include "Zones.hpp"

// ! --- The stack --- !

void ZoneStack::push(uint16_t id) noexcept
{
  auto current = depth.load(std::memory_order_relaxed);
  if (overflow || current >= MAX_DEPTH)
  {
    overflow++;
    return;
  }
  ids[current] = id;
  depth.store(current + 1, std::memory_order_release);
}

void ZoneStack::pop() noexcept
{
  if (overflow)
  {
    overflow--;
    return;
  }
  auto current = depth.load(std::memory_order_relaxed);
  if (current > 0)
    depth.store(current - 1, std::memory_order_release);
}

uint16_t ZoneStack::top() const noexcept
{
  auto current = depth.load(std::memory_order_acquire);
  if (current == 0 || current > MAX_DEPTH)
    return 0;
  return ids[current - 1];
}

// ! --- Names --- !

uint16_t ZoneRegistry::intern(std::string const& mod, std::string const& name)
{
  auto key = mod + '\n' + name;
  std::lock_guard lock(m_mutex);
  if (auto found = m_ids.find(key); found != m_ids.end())
    return found->second;
  if (m_names.size() >= MAX_ZONES)
    return 0;
  m_names.push_back({mod, name});
  auto id = static_cast<uint16_t>(m_names.size());
  m_ids.emplace(std::move(key), id);
  return id;
}

ZoneName ZoneRegistry::nameOf(uint16_t id) const
{
  std::lock_guard lock(m_mutex);
  if (id == 0 || id > m_names.size())
    return {};
  return m_names[id - 1];
}

size_t ZoneRegistry::size() const
{
  std::lock_guard lock(m_mutex);
  return m_names.size();
}
