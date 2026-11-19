/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/ProtocolUtil.h"
#include "base/Log.h"
#include "deskflow/DeskflowException.h"
#include "deskflow/ProtocolTypes.h"
#include "io/IStream.h"
#include <array>
#include <iterator>

#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

//
// ProtocolUtil
//

namespace {

uint8_t *writeIntToPtr(uint32_t value, uint32_t length, uint8_t *dst)
{
  switch (length) {
  case 1:
    *dst++ = static_cast<uint8_t>(value & 0xffU);
    break;
  case 4:
    *dst++ = static_cast<uint8_t>((value >> 24U) & 0xffU);
    *dst++ = static_cast<uint8_t>((value >> 16U) & 0xffU);
    *dst++ = static_cast<uint8_t>((value >> 8U) & 0xffU);
    *dst++ = static_cast<uint8_t>(value & 0xffU);
    break;
  case 2:
    *dst++ = static_cast<uint8_t>((value >> 8U) & 0xffU);
    *dst++ = static_cast<uint8_t>(value & 0xffU);
    break;
  default:
    assert(0 && "invalid integer format length");
    break;
  }
  return dst;
}

template <typename T> uint8_t *writeVectorIntToPtr(const std::vector<T> *vectorData, uint8_t *dst)
{
  const uint32_t count = (vectorData != nullptr) ? static_cast<uint32_t>(vectorData->size()) : 0;
  dst = writeIntToPtr(count, sizeof(uint32_t), dst);
  if (vectorData != nullptr) {
    const std::vector<T> &vec = *vectorData;
    for (size_t i = 0; i < vec.size(); ++i) {
      dst = writeIntToPtr(vec[i], sizeof(T), dst);
    }
  }
  return dst;
}

uint8_t *writeStringToPtr(const std::string *stringData, uint8_t *dst)
{
  const uint32_t len = (stringData != nullptr) ? static_cast<uint32_t>(stringData->size()) : 0;
  dst = writeIntToPtr(len, sizeof(len), dst);
  if (len != 0) {
    std::memcpy(dst, stringData->data(), len);
    dst += len;
  }
  return dst;
}

uint32_t eatFormatLength(const char **pfmt)
{
  const char *fmt = *pfmt;
  uint32_t n = 0;
  while (*fmt >= '0' && *fmt <= '9') {
    n = 10 * n + static_cast<uint32_t>(*fmt - '0');
    ++fmt;
  }
  *pfmt = fmt;
  return n;
}

void writeFormattedBytes(uint8_t *dst, const char *fmt, va_list args)
{
  while (*fmt) {
    if (*fmt == '%') {
      ++fmt;
      const auto len = eatFormatLength(&fmt);
      switch (*fmt) {
      case 'i': {
        const uint32_t v = va_arg(args, uint32_t);
        dst = writeIntToPtr(v, len, dst);
        break;
      }
      case 'I': {
        switch (len) {
        case 1:
          dst = writeVectorIntToPtr(va_arg(args, const std::vector<uint8_t> *), dst);
          break;
        case 2:
          dst = writeVectorIntToPtr(va_arg(args, const std::vector<uint16_t> *), dst);
          break;
        case 4:
          dst = writeVectorIntToPtr(va_arg(args, const std::vector<uint32_t> *), dst);
          break;
        default:
          assert(0 && "invalid integer vector format length");
          return;
        }
        break;
      }
      case 's': {
        assert(len == 0);
        const std::string *src = va_arg(args, std::string *);
        dst = writeStringToPtr(src, dst);
        break;
      }
      case 'S': {
        assert(len == 0);
        const uint32_t byteLen = va_arg(args, uint32_t);
        const uint8_t *src = va_arg(args, const uint8_t *);
        dst = writeIntToPtr(byteLen, sizeof(byteLen), dst);
        if (byteLen != 0 && src != nullptr) {
          std::memcpy(dst, src, byteLen);
          dst += byteLen;
        } else if (byteLen != 0) {
          std::memset(dst, 0, byteLen);
          dst += byteLen;
        }
        break;
      }
      case '%':
        assert(len == 0);
        *dst++ = '%';
        break;
      default:
        assert(0 && "invalid format specifier");
        break;
      }
      ++fmt;
    } else {
      *dst++ = static_cast<uint8_t>(*fmt++);
    }
  }
}

} // namespace

void ProtocolUtil::writef(deskflow::IStream *stream, const char *fmt, ...)
{
  assert(stream != nullptr);
  assert(fmt != nullptr);
  LOG_VERBOSE("writef(%s)", fmt);

  va_list args;
  va_start(args, fmt);
  uint32_t size = 0;
  try {
    size = getLength(fmt, args);
  } catch (...) {
    va_end(args);
    throw;
  }
  va_end(args);
  va_start(args, fmt);
  try {
    vwritef(stream, fmt, size, args);
  } catch (...) {
    va_end(args);
    throw;
  }
  va_end(args);
}

bool ProtocolUtil::readf(deskflow::IStream *stream, const char *fmt, ...)
{
  bool result = false;

  if (stream && fmt) {
    LOG_VERBOSE("readf(%s)", fmt);
    va_list args;
    va_start(args, fmt);
    try {
      vreadf(stream, fmt, args);
      result = true;
    } catch (IOException &) {
      result = false;
    } catch (const std::bad_alloc &) {
      result = false;
    }
    va_end(args);
  }

  return result;
}

void ProtocolUtil::vwritef(deskflow::IStream *stream, const char *fmt, uint32_t size, va_list args)
{
  assert(stream != nullptr);
  assert(fmt != nullptr);

  // done if nothing to write
  if (size == 0) {
    return;
  }

  uint8_t stackBuffer[128];
  std::vector<uint8_t> heapBuffer;
  uint8_t *out = stackBuffer;
  if (size > sizeof(stackBuffer)) {
    heapBuffer.resize(size);
    out = heapBuffer.data();
  }
  writeFormattedBytes(out, fmt, args);

  try {
    stream->write(out, size);
    LOG_VERBOSE("wrote %d bytes", size);
  } catch (const BaseException &exception) {
    LOG_VERBOSE("exception <%s> during wrote %d bytes into stream", exception.what(), size);
    throw;
  }
}

void ProtocolUtil::vreadf(deskflow::IStream *stream, const char *fmt, va_list args)
{
  assert(stream != nullptr);
  assert(fmt != nullptr);

  // begin scanning
  while (*fmt) {
    if (*fmt == '%') {
      // format specifier.  determine argument size.
      ++fmt;
      auto len = eatLength(&fmt);
      switch (*fmt) {
      case 'i': {
        void *destination = va_arg(args, void *);
        switch (len) {
        case 1:
          // 1 byte integer
          *static_cast<uint8_t *>(destination) = read1ByteInt(stream);
          break;
        case 2:
          // 2 byte integer
          *static_cast<uint16_t *>(destination) = read2BytesInt(stream);
          break;
        case 4:
          // 4 byte integer
          *static_cast<uint32_t *>(destination) = read4BytesInt(stream);
          break;
        default:
          // the length is wrong
          LOG_ERR("read: length to be read is wrong: '%d' should be 1,2, or 4", len);
          assert(false); // assert for debugging
          break;
        }
        break;
      }

      case 'I': {
        void *destination = va_arg(args, void *);
        switch (len) {
        case 1:
          // 1 byte integer
          readVector1ByteInt(stream, *static_cast<std::vector<uint8_t> *>(destination));
          break;
        case 2:
          // 2 byte integer
          readVector2BytesInt(stream, *static_cast<std::vector<uint16_t> *>(destination));
          break;
        case 4:
          // 4 byte integer
          readVector4BytesInt(stream, *static_cast<std::vector<uint32_t> *>(destination));
          break;
        default:
          // the length is wrong
          LOG_ERR("read: length to be read is wrong: '%d' should be 1,2, or 4", len);
          assert(false); // assert for debugging
          break;
        }
        break;
      }

      case 's': {
        std::string *destination = va_arg(args, std::string *);

        if (len > PROTOCOL_MAX_STRING_LENGTH) {
          LOG_ERR("read: string length exceeds maximum allowed size: %u", len);
          throw BadClientException("Too long message received");
        }

        readBytes(stream, len, destination);
        break;
      }

      case '%':
        assert(len == 0);
        break;

      default:
        assert(0 && "invalid format specifier");
      }

      // next format character
      ++fmt;
    } else {
      // read next character
      char buffer[1];
      read(stream, buffer, 1);

      // verify match
      if (buffer[0] != *fmt) {
        LOG_VERBOSE("readf: format mismatch: %c vs %c", *fmt, buffer[0]);
        throw XIOReadMismatch();
      }

      // next format character
      ++fmt;
    }
  }
}

uint32_t ProtocolUtil::getLength(const char *fmt, va_list args)
{
  size_t total = 0;
  while (*fmt) {
    if (*fmt == '%') {
      // format specifier.  determine argument size.
      ++fmt;
      size_t fieldLen = eatLength(&fmt);
      switch (*fmt) {
      case 'i':
        assert(fieldLen == 1 || fieldLen == 2 || fieldLen == 4);
        (void)va_arg(args, uint32_t);
        break;

      case 'I':
        assert(fieldLen == 1 || fieldLen == 2 || fieldLen == 4);
        switch (fieldLen) {
        case 1: {
          const auto *list = va_arg(args, const std::vector<uint8_t> *);
          fieldLen = ((list != nullptr) ? list->size() : 0) + 4;
          break;
        }

        case 2: {
          const auto *list = va_arg(args, const std::vector<uint16_t> *);
          fieldLen = 2 * ((list != nullptr) ? list->size() : 0) + 4;
          break;
        }

        case 4: {
          const auto *list = va_arg(args, const std::vector<uint32_t> *);
          fieldLen = 4 * ((list != nullptr) ? list->size() : 0) + 4;
          break;
        }

        default:
          LOG_ERR("format specifier %%I%zu has invalid length", fieldLen);
          fieldLen = 0;
          break;
        }
        break;

      case 's': {
        assert(fieldLen == 0);
        const auto *src = va_arg(args, const std::string *);
        fieldLen = ((src != nullptr) ? src->size() : 0) + 4;
        break;
      }

      case 'S': {
        assert(fieldLen == 0);
        const uint32_t rawLen = va_arg(args, uint32_t);
        (void)va_arg(args, const uint8_t *);
        fieldLen = static_cast<size_t>(rawLen) + 4;
        break;
      }

      case '%':
        assert(fieldLen == 0);
        fieldLen = 1;
        break;

      default:
        assert(0 && "invalid format specifier");
      }

      static constexpr size_t kMaxFrameSize = (std::numeric_limits<uint32_t>::max)() - 4U;
      if (fieldLen > kMaxFrameSize || total > kMaxFrameSize - fieldLen) {
        throw std::length_error("protocol message length overflow");
      }
      total += fieldLen;
      ++fmt;
    } else {
      static constexpr size_t kMaxFrameSize = (std::numeric_limits<uint32_t>::max)() - 4U;
      if (total >= kMaxFrameSize) {
        throw std::length_error("protocol message length overflow");
      }
      ++total;
      ++fmt;
    }
  }
  return static_cast<uint32_t>(total);
}

void ProtocolUtil::writef(std::vector<uint8_t> &buffer, const char *fmt, va_list args)
{
  va_list copy;
  va_copy(copy, args);
  uint32_t size = 0;
  try {
    size = getLength(fmt, copy);
  } catch (...) {
    va_end(copy);
    throw;
  }
  va_end(copy);
  const size_t offset = buffer.size();
  buffer.resize(offset + size);
  if (size != 0) {
    writeFormattedBytes(buffer.data() + offset, fmt, args);
  }
}

uint32_t ProtocolUtil::eatLength(const char **pfmt)
{
  const char *fmt = *pfmt;
  uint32_t n = 0;
  for (;;) {
    uint32_t d;
    switch (*fmt) {
    case '0':
      d = 0;
      break;
    case '1':
      d = 1;
      break;
    case '2':
      d = 2;
      break;
    case '3':
      d = 3;
      break;
    case '4':
      d = 4;
      break;
    case '5':
      d = 5;
      break;
    case '6':
      d = 6;
      break;
    case '7':
      d = 7;
      break;
    case '8':
      d = 8;
      break;
    case '9':
      d = 9;
      break;
    default:
      *pfmt = fmt;
      return n;
    }
    n = 10 * n + d;
    ++fmt;
  }
}

void ProtocolUtil::read(deskflow::IStream *stream, void *vbuffer, uint32_t count)
{
  assert(stream != nullptr);
  assert(vbuffer != nullptr);

  auto *buffer = static_cast<uint8_t *>(vbuffer);
  while (count > 0) {
    // read more
    uint32_t n = stream->read(buffer, count);

    // bail if stream has hungup
    if (n == 0) {
      LOG_VERBOSE("unexpected disconnect in readf(), %d bytes left", count);
      throw IOEndOfStreamException();
    }

    // prepare for next read
    buffer += n;
    count -= n;
  }
}

uint8_t ProtocolUtil::read1ByteInt(deskflow::IStream *stream)
{
  const uint32_t BufferSize = 1;
  std::array<uint8_t, 1> buffer = {};
  read(stream, buffer.data(), BufferSize);

  uint8_t Result = buffer[0];
  LOG_VERBOSE("readf: read 1 byte integer: %d (0x%x)", Result, Result);

  return Result;
}

uint16_t ProtocolUtil::read2BytesInt(deskflow::IStream *stream)
{
  const uint32_t BufferSize = 2;
  std::array<uint8_t, BufferSize> buffer = {};
  read(stream, buffer.data(), BufferSize);

  auto Result = static_cast<uint16_t>((static_cast<uint16_t>(buffer[0]) << 8) | static_cast<uint16_t>(buffer[1]));
  LOG_VERBOSE("readf: read 2 byte integer: %d (0x%x)", Result, Result);

  return Result;
}

uint32_t ProtocolUtil::read4BytesInt(deskflow::IStream *stream)
{
  const int BufferSize = 4;
  std::array<uint8_t, BufferSize> buffer = {};
  read(stream, buffer.data(), BufferSize);

  uint32_t Result = (static_cast<uint32_t>(buffer[0]) << 24) | (static_cast<uint32_t>(buffer[1]) << 16) |
                    (static_cast<uint32_t>(buffer[2]) << 8) | (static_cast<uint32_t>(buffer[3]));

  LOG_VERBOSE("readf: read 4 byte integer: %d (0x%x)", Result, Result);

  return Result;
}

void ProtocolUtil::readVector1ByteInt(deskflow::IStream *stream, std::vector<uint8_t> &destination)
{
  auto size = readVectorSize(stream);
  destination.reserve(destination.size() + size);
  for (uint32_t i = 0; i < size; ++i) {
    destination.push_back(read1ByteInt(stream));
  }
}

void ProtocolUtil::readVector2BytesInt(deskflow::IStream *stream, std::vector<uint16_t> &destination)
{
  auto size = readVectorSize(stream);
  destination.reserve(destination.size() + size);
  for (uint32_t i = 0; i < size; ++i) {
    destination.push_back(read2BytesInt(stream));
  }
}

void ProtocolUtil::readVector4BytesInt(deskflow::IStream *stream, std::vector<uint32_t> &destination)
{
  auto size = readVectorSize(stream);
  destination.reserve(destination.size() + size);
  for (uint32_t i = 0; i < size; ++i) {
    destination.push_back(read4BytesInt(stream));
  }
}

uint32_t ProtocolUtil::readVectorSize(deskflow::IStream *stream)
{
  auto size = read4BytesInt(stream);

  if (size > PROTOCOL_MAX_LIST_LENGTH) {
    LOG_ERR("readVectorSize: vector length exceeds maximum allowed size: %u", size);
    throw BadClientException("Too long message received");
  }

  return size;
}

void ProtocolUtil::readBytes(deskflow::IStream *stream, uint32_t len, std::string *destination)
{
  // when string length is 0, this implies that the size of the string is
  // variable and will be embedded in the stream.
  if (len == 0) {
    len = read4BytesInt(stream);
  }

  if (len > PROTOCOL_MAX_STRING_LENGTH) {
    LOG_ERR("read: string length exceeds maximum allowed size: %u", len);
    throw BadClientException("Too long message received");
  }

  if (destination != nullptr) {
    try {
      destination->resize(len);
    } catch (std::bad_alloc &exception) {
      LOG_ERR("bad alloc, unable to allocate memory %u bytes", len);
      throw exception;
    }
    if (len != 0) {
      read(stream, destination->data(), len);
    }
    LOG_VERBOSE("readf: read %u byte string", len);
    return;
  }

  uint8_t discard[256];
  uint32_t remaining = len;
  while (remaining > 0) {
    const uint32_t chunk = (remaining < sizeof(discard)) ? remaining : static_cast<uint32_t>(sizeof(discard));
    read(stream, discard, chunk);
    remaining -= chunk;
  }
  LOG_VERBOSE("readf: discarded %u byte string", len);
}

//
// XIOReadMismatch
//

QString XIOReadMismatch::getWhat() const throw()
{
  return format("XIOReadMismatch", "ProtocolUtil::readf() mismatch");
}
