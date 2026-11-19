/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

//! FIFO of bytes
/*!
This class maintains a contiguous FIFO buffer of bytes with amortized O(1)
compaction and retained capacity for high-frequency packet I/O.
*/
class StreamBuffer
{
public:
  StreamBuffer() = default;
  ~StreamBuffer() = default;

  //! @name manipulators
  //@{

  //! Read data without removing from buffer
  /*!
  Return a pointer to memory with the next \c n bytes in the buffer
  (which must be <= getSize()).  The caller must not modify the returned
  memory nor delete it.
  */
  const void *peek(uint32_t n);

  //! Discard data
  /*!
  Discards the next \c n bytes.  If \c n >= getSize() then the buffer
  is cleared.
  */
  void pop(uint32_t n);

  //! Write data to buffer
  /*!
  Appends \c n bytes from \c data to the buffer.
  */
  void write(const void *data, uint32_t n);

  //@}
  //! @name accessors
  //@{

  //! Get size of buffer
  /*!
  Returns the number of bytes in the buffer.
  */
  uint32_t getSize() const;

  //@}

private:
  static constexpr size_t kInitialCapacity = 4096;
  static constexpr size_t kCompactThreshold = 4096;
  static constexpr size_t kMaxRetainedCapacity = 65536;

  std::vector<uint8_t> m_data;
  uint32_t m_head = 0;
  uint32_t m_size = 0;
};
