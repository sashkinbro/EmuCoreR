#pragma once
#include "types.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <utility>

#ifdef _WIN32
#include <malloc.h> // _aligned_malloc
#endif

template<typename T, uint32_t CAPACITY>
class FIFOQueue
{
public:
  const T* GetReadPointer() const { return &m_ptr[m_head]; }
  T* GetReadPointer() { return &m_ptr[m_head]; }
  T* GetWritePointer() { return &m_ptr[m_tail]; }
  uint32_t GetSize() const { return m_size; }
  uint32_t GetSpace() const { return CAPACITY - m_size; }
  uint32_t GetContiguousSpace() const
  {
    if (m_tail == m_head && m_size > 0)
      return 0;
    else if (m_tail >= m_head)
      return (CAPACITY - m_tail);
    return (m_head - m_tail);
  }
  uint32_t GetContiguousSize() const { return std::min<uint32_t>(CAPACITY - m_head, m_size); }
  bool IsEmpty() const { return m_size == 0; }
  bool IsFull() const { return m_size == CAPACITY; }

  void Clear()
  {
    m_head = 0;
    m_tail = 0;
    m_size = 0;
  }

  template<class Y = T, std::enable_if_t<std::is_pod_v<Y>, int> = 0>
  T& Push(const T& value)
  {
    T& ref = PushAndGetReference();
    std::memcpy(&ref, &value, sizeof(T));
    return ref;
  }

  template<class Y = T, std::enable_if_t<!std::is_pod_v<Y>, int> = 0>
  T& Push(const T& value)
  {
    T& ref = PushAndGetReference();
    new (&ref) T(value);
    return ref;
  }

  // faster version of push_back_range for POD types which can be memcpy()ed
  template<class Y = T, std::enable_if_t<std::is_pod_v<Y>, int> = 0>
  void PushRange(const T* data, uint32_t size)
  {
    const uint32_t space_before_end = CAPACITY - m_tail;
    const uint32_t size_before_end = (size > space_before_end) ? space_before_end : size;
    const uint32_t size_after_end = size - size_before_end;

    std::memcpy(&m_ptr[m_tail], data, sizeof(T) * size_before_end);
    m_tail = (m_tail + size_before_end) % CAPACITY;

    if (size_after_end > 0)
    {
      std::memcpy(&m_ptr[m_tail], data + size_before_end, sizeof(T) * size_after_end);
      m_tail = (m_tail + size_after_end) % CAPACITY;
    }

    m_size += size;
  }

  template<class Y = T, std::enable_if_t<!std::is_pod_v<Y>, int> = 0>
  void PushRange(const T* data, uint32_t size)
  {
    while (size > 0)
    {
      T& ref = PushAndGetReference();
      new (&ref) T(*data);
      data++;
      size--;
    }
  }

  const T& Peek() const { return m_ptr[m_head]; }
  const T& Peek(uint32_t offset) { return m_ptr[(m_head + offset) % CAPACITY]; }

  void Remove(uint32_t count)
  {
    for (uint32_t i = 0; i < count; i++)
    {
      m_ptr[m_head].~T();
      m_head = (m_head + 1) % CAPACITY;
      m_size--;
    }
  }

  void RemoveOne()
  {
    m_ptr[m_head].~T();
    m_head = (m_head + 1) % CAPACITY;
    m_size--;
  }

  // removes and returns moved value
  T Pop()
  {
    T val = std::move(m_ptr[m_head]);
    m_ptr[m_head].~T();
    m_head = (m_head + 1) % CAPACITY;
    m_size--;
    return val;
  }

  void PopRange(T* out_data, uint32_t count)
  {

    for (uint32_t i = 0; i < count; i++)
    {
      out_data[i] = std::move(m_ptr[m_head]);
      m_ptr[m_head].~T();
      m_head = (m_head + 1) % CAPACITY;
      m_size--;
    }
  }

  template<uint32_t QUEUE_CAPACITY>
  void PushFromQueue(FIFOQueue<T, QUEUE_CAPACITY>* other_queue)
  {
    while (!other_queue->IsEmpty() && !IsFull())
    {
      T& dest = PushAndGetReference();
      dest = std::move(other_queue->Pop());
    }
  }

  void AdvanceTail(uint32_t count)
  {
    m_tail = (m_tail + count) % CAPACITY;
    m_size += count;
  }

protected:
  FIFOQueue() = default;

  T& PushAndGetReference()
  {
    T& ref = m_ptr[m_tail];
    m_tail = (m_tail + 1) % CAPACITY;
    m_size++;
    return ref;
  }

  T* m_ptr = nullptr;
  uint32_t m_head = 0;
  uint32_t m_tail = 0;
  uint32_t m_size = 0;
};

template<typename T, uint32_t CAPACITY>
class InlineFIFOQueue : public FIFOQueue<T, CAPACITY>
{
public:
  InlineFIFOQueue() : FIFOQueue<T, CAPACITY>() { this->m_ptr = m_inline_data; }

private:
  T m_inline_data[CAPACITY] = {};
};

template<typename T, uint32_t CAPACITY, uint32_t ALIGNMENT = 0>
class HeapFIFOQueue : public FIFOQueue<T, CAPACITY>
{
public:
  HeapFIFOQueue() : FIFOQueue<T, CAPACITY>()
  {
    if constexpr (ALIGNMENT > 0)
    {
#ifdef _WIN32
      this->m_ptr = static_cast<T*>(_aligned_malloc(sizeof(T) * CAPACITY, ALIGNMENT));
#else
      if (posix_memalign(reinterpret_cast<void**>(&this->m_ptr), ALIGNMENT, sizeof(T) * CAPACITY) != 0)
        this->m_ptr = nullptr;
#endif
    }
    else
      this->m_ptr = static_cast<T*>(std::malloc(sizeof(T) * CAPACITY));

    if (this->m_ptr == nullptr)
      std::abort();

    std::memset(this->m_ptr, 0, sizeof(T) * CAPACITY);
  }

  ~HeapFIFOQueue()
  {
    if constexpr (ALIGNMENT > 0)
    {
#ifdef _WIN32
      _aligned_free(this->m_ptr);
#else
      free(this->m_ptr);
#endif
    }
    else
    {
      free(this->m_ptr);
    }
  }
};
