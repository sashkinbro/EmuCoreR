#pragma once
#include <algorithm>
#include <cassert>
#include <cstddef>

template<typename T, std::size_t SIZE>
class HeapArray
{
public:
  using value_type = T;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using reference = T&;
  using const_reference = const T&;
  using pointer = T*;
  using const_pointer = const T*;
  using this_type = HeapArray<T, SIZE>;

  HeapArray() { m_data = new T[SIZE]; }

  HeapArray(const this_type& copy) = delete;
  this_type& operator=(const this_type& rhs) = delete;

  HeapArray(this_type&& move)
  {
    m_data = move.m_data;
    move.m_data = nullptr;
  }

  ~HeapArray() { delete[] m_data; }

  size_type size() const { return SIZE; }

  pointer data() { return m_data; }
  const_pointer data() const { return m_data; }

  const_reference operator[](size_type index) const
  {
    assert(index < SIZE);
    return m_data[index];
  }
  reference operator[](size_type index)
  {
    assert(index < SIZE);
    return m_data[index];
  }

  void fill(const_reference value) { std::fill(m_data, m_data + SIZE, value); }

  this_type& operator=(this_type&& move)
  {
    delete[] m_data;
    m_data = move.m_data;
    move.m_data = nullptr;
    return *this;
  }

private:
  T* m_data;
};
