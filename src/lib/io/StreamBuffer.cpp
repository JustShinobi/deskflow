/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "io/StreamBuffer.h"

#include <algorithm>
#include <cassert>
#include <cstring>

//
// StreamBuffer
//

const void *StreamBuffer::peek(uint32_t n)
{
  assert(n <= m_size);

  if (n == 0) {
    return nullptr;
  }

  return static_cast<const void *>(m_data.data() + m_head);
}

void StreamBuffer::pop(uint32_t n)
{
  if (n >= m_size) {
    m_head = 0;
    m_size = 0;
    m_data.clear();
    if (m_data.capacity() > kMaxRetainedCapacity) {
      std::vector<uint8_t> shrunk;
      shrunk.reserve(kInitialCapacity);
      m_data.swap(shrunk);
    }
    return;
  }

  m_head += n;
  m_size -= n;

  if (m_head >= kCompactThreshold && m_head >= m_size) {
    std::memmove(m_data.data(), m_data.data() + m_head, m_size);
    m_data.resize(m_size);
    m_head = 0;
  }
}

void StreamBuffer::write(const void *vdata, uint32_t n)
{
  assert(vdata != nullptr);

  if (n == 0) {
    return;
  }

  if (m_head > 0 && m_data.size() + n > m_data.capacity()) {
    std::memmove(m_data.data(), m_data.data() + m_head, m_size);
    m_data.resize(m_size);
    m_head = 0;
  }

  const size_t required = m_data.size() + static_cast<size_t>(n);
  if (required > m_data.capacity()) {
    const size_t doubled = m_data.capacity() * 2;
    m_data.reserve(std::max({kInitialCapacity, doubled, required}));
  }

  const size_t writeOffset = m_data.size();
  m_data.resize(required);
  std::memcpy(m_data.data() + writeOffset, vdata, n);
  m_size += n;
}

uint32_t StreamBuffer::getSize() const
{
  return m_size;
}
