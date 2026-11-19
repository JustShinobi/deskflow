/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2013 - 2016 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/ClipboardTypes.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

class EventQueueTimer;
class IEventQueue;

namespace deskflow {
class IStream;
}

//! Streams clipboard payloads in small, socket-backpressure-paced chunks so
//! interactive mouse and keyboard frames never stall behind bulk transfers.
class ClipboardPacer
{
public:
  ClipboardPacer(IEventQueue *events, deskflow::IStream *stream);
  ~ClipboardPacer();

  ClipboardPacer(const ClipboardPacer &) = delete;
  ClipboardPacer &operator=(const ClipboardPacer &) = delete;

  void enqueue(ClipboardID id, uint32_t sequence, std::string data);
  void cancelAll();

private:
  struct Transfer
  {
    ClipboardID id = 0;
    uint32_t sequence = 0;
    std::string payload;
    size_t offset = 0;
    bool started = false;
    bool finished = false;
  };

  void pump();
  void armTimer();
  void disarmTimer();

  IEventQueue *m_events = nullptr;
  deskflow::IStream *m_stream = nullptr;
  EventQueueTimer *m_timer = nullptr;
  std::deque<Transfer> m_queue;
};

class StreamChunker
{
public:
  static void sendClipboard(
      const std::string_view &data, size_t size, ClipboardID id, uint32_t sequence, IEventQueue *events,
      void *eventTarget
  );
};
