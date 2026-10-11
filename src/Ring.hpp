#pragma once

// A ring of samples between two threads, without locks or allocations once it's made.
// One thread writes (the sampler), one thread reads (the analyzer). The writer fills a slot in place:
// `reserve` gives the next free slot or nothing when the ring is full, `commit` hands it to the reader
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

template <class T>
class Ring
{
public:
  // The capacity is rounded up to a power of two
  explicit Ring(size_t capacity)
  {
    size_t size = 1;
    while (size < capacity)
      size <<= 1;
    m_mask = size - 1;
    m_slots = std::make_unique<T[]>(size);
  }

  size_t capacity() const { return m_mask + 1; }

  // ! --- Writer --- !

  // The next free slot, or null when the reader is behind by a whole ring
  T* reserve()
  {
    auto head = m_head.load(std::memory_order_relaxed);
    auto tail = m_tail.load(std::memory_order_acquire);
    if (head - tail > m_mask)
      return nullptr;
    return &m_slots[head & m_mask];
  }

  // The slot from `reserve` is ready for the reader
  void commit() { m_head.store(m_head.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

  // ! --- Reader --- !

  // The oldest committed slot, or null when there's none
  T const* peek() const
  {
    auto tail = m_tail.load(std::memory_order_relaxed);
    auto head = m_head.load(std::memory_order_acquire);
    if (tail == head)
      return nullptr;
    return &m_slots[tail & m_mask];
  }

  // The slot from `peek` can be written again
  void pop() { m_tail.store(m_tail.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

  size_t size() const
  {
    return static_cast<size_t>(m_head.load(std::memory_order_acquire) - m_tail.load(std::memory_order_acquire));
  }

private:
  std::unique_ptr<T[]> m_slots;
  size_t m_mask = 0;
  // Apart, so the two threads don't fight over one cache line
  alignas(64) std::atomic<uint64_t> m_head{0};
  alignas(64) std::atomic<uint64_t> m_tail{0};
};
