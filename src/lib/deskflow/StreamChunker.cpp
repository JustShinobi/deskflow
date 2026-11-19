/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2013 - 2016 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/StreamChunker.h"

#include "base/Event.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "deskflow/ClipboardChunk.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "io/IStream.h"

#include <algorithm>

namespace {
constexpr size_t kChunkSize = 32 * 1024;
constexpr uint32_t kMaxInFlightSocketBytes = 32 * 1024;
constexpr double kPaceIntervalSeconds = 0.003;
} // namespace

ClipboardPacer::ClipboardPacer(IEventQueue *events, deskflow::IStream *stream) : m_events(events), m_stream(stream)
{
}

ClipboardPacer::~ClipboardPacer()
{
  cancelAll();
}

void ClipboardPacer::cancelAll()
{
  disarmTimer();
  m_queue.clear();
}

void ClipboardPacer::enqueue(ClipboardID id, uint32_t sequence, std::string data)
{
  if (m_stream == nullptr || m_events == nullptr) {
    return;
  }

  // Drop any queued transfer for the same clipboard ID that has not yet emitted DataStart.
  for (auto it = m_queue.begin(); it != m_queue.end();) {
    if (!it->started && it->id == id) {
      it = m_queue.erase(it);
    } else {
      ++it;
    }
  }

  Transfer transfer;
  transfer.id = id;
  transfer.sequence = sequence;
  transfer.payload = std::move(data);
  m_queue.push_back(std::move(transfer));

  pump();
}

void ClipboardPacer::armTimer()
{
  if (m_timer != nullptr || m_events == nullptr) {
    return;
  }
  m_timer = m_events->newOneShotTimer(kPaceIntervalSeconds, nullptr);
  m_events->addHandler(EventTypes::Timer, m_timer, [this](const auto &) {
    disarmTimer();
    pump();
  });
}

void ClipboardPacer::disarmTimer()
{
  if (m_timer != nullptr && m_events != nullptr) {
    m_events->removeHandler(EventTypes::Timer, m_timer);
    m_events->deleteTimer(m_timer);
    m_timer = nullptr;
  }
}

void ClipboardPacer::pump()
{
  if (m_stream == nullptr) {
    return;
  }

  while (!m_queue.empty()) {
    if (m_stream->getOutputSize() >= kMaxInFlightSocketBytes) {
      armTimer();
      return;
    }

    Transfer &cur = m_queue.front();
    if (!cur.started) {
      cur.started = true;
      const std::string sizeStr = std::to_string(cur.payload.size());
      ProtocolUtil::writef(
          m_stream, "DCLP%1i%4i%1i%S", cur.id, cur.sequence, static_cast<uint8_t>(ChunkType::DataStart),
          static_cast<uint32_t>(sizeStr.size()), reinterpret_cast<const uint8_t *>(sizeStr.data())
      );
      if (cur.payload.empty()) {
        // Emit one empty DataChunk before DataEnd to match StreamChunker's wire sequence.
        ProtocolUtil::writef(
            m_stream, "DCLP%1i%4i%1i%S", cur.id, cur.sequence, static_cast<uint8_t>(ChunkType::DataChunk), uint32_t{0},
            nullptr
        );
      }
      continue;
    }

    if (cur.offset < cur.payload.size()) {
      const size_t remaining = cur.payload.size() - cur.offset;
      const size_t slice = std::min(remaining, kChunkSize);
      ProtocolUtil::writef(
          m_stream, "DCLP%1i%4i%1i%S", cur.id, cur.sequence, static_cast<uint8_t>(ChunkType::DataChunk),
          static_cast<uint32_t>(slice), reinterpret_cast<const uint8_t *>(cur.payload.data() + cur.offset)
      );
      cur.offset += slice;
      continue;
    }

    if (!cur.finished) {
      cur.finished = true;
      ProtocolUtil::writef(
          m_stream, "DCLP%1i%4i%1i%S", cur.id, cur.sequence, static_cast<uint8_t>(ChunkType::DataEnd), uint32_t{0},
          nullptr
      );
      LOG_DEBUG("sent clipboard size=%zu", cur.payload.size());
    }

    m_queue.pop_front();
  }
}

void StreamChunker::sendClipboard(
    const std::string_view &data, size_t size, ClipboardID id, uint32_t sequence, IEventQueue *events, void *eventTarget
)
{
  // send first message (data size)
  std::string dataSize = std::to_string(size);
  ClipboardChunk *sizeMessage = ClipboardChunk::start(id, sequence, dataSize);

  events->addEvent(Event(EventTypes::ClipboardSending, eventTarget, sizeMessage));

  // send clipboard chunk with a 32KB slice size
  size_t sentLength = 0;
  size_t chunkSize = kChunkSize;

  while (true) {
    if (sentLength + chunkSize > size) {
      chunkSize = size - sentLength;
    }

    std::string chunk(data.substr(sentLength, chunkSize).data(), chunkSize);
    ClipboardChunk *dataChunk = ClipboardChunk::data(id, sequence, chunk);

    events->addEvent(Event(EventTypes::ClipboardSending, eventTarget, dataChunk));

    sentLength += chunkSize;
    if (sentLength == size) {
      break;
    }
  }

  // send last message
  ClipboardChunk *end = ClipboardChunk::end(id, sequence);

  events->addEvent(Event(EventTypes::ClipboardSending, eventTarget, end));

  LOG_DEBUG("sent clipboard size=%zu", sentLength);
}
