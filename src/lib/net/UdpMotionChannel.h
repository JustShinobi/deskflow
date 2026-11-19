/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "arch/IArchNetwork.h"
#include "net/ISocket.h"
#include "net/NetworkAddress.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>

class EventQueueTimer;
class IEventQueue;
class ISocketMultiplexerJob;
class SocketMultiplexer;

//! Authenticated low-latency UDP side-channel for real-time mouse motion.
/*!
Eliminates TCP Head-of-Line blocking over lossy/congested Wi-Fi by carrying
idempotent absolute cursor coordinates and cumulative relative deltas in
28-byte SipHash-2-4 authenticated datagrams tagged for WMM voice/video QoS.
*/
class UdpMotionChannel : public ISocket
{
public:
  static constexpr size_t kPacketSize = 28;
  static constexpr size_t kHeaderSize = 20;
  using SessionKey = std::array<uint32_t, 4>;

  enum class PacketType : uint8_t
  {
    Ping = 1,
    Ack = 2,
    MouseAbs = 3,
    MouseRel = 4,
    MouseWheel = 5
  };

  struct DecodedPacket
  {
    PacketType type = PacketType::Ping;
    uint16_t enterSeq = 0;
    uint32_t clientId = 0;
    uint32_t pktSeq = 0;
    int16_t x = 0;
    int16_t y = 0;
  };

  struct ClientRegistration
  {
    uint32_t clientId = 0;
    SessionKey key{};
  };

  struct ClientMotionBatch
  {
    bool hasAbs = false;
    uint16_t absEnterSeq = 0;
    int32_t absX = 0;
    int32_t absY = 0;

    bool hasRel = false;
    uint16_t relEnterSeq = 0;
    int32_t relDx = 0;
    int32_t relDy = 0;
  };

  UdpMotionChannel(IEventQueue *events, SocketMultiplexer *multiplexer);
  UdpMotionChannel(const UdpMotionChannel &) = delete;
  UdpMotionChannel &operator=(const UdpMotionChannel &) = delete;
  ~UdpMotionChannel() override;

  // ISocket overrides
  void bind(const NetworkAddress &addr) override;
  void close() override;
  void *getEventTarget() const override;

  // Pure packet & MAC helpers (exposed for deterministic unit tests)
  static SessionKey generateSessionKey();
  static uint64_t computeMac(const uint8_t header[kHeaderSize], const SessionKey &key);
  static void encodePacket(
      PacketType type, uint16_t enterSeq, uint32_t clientId, uint32_t pktSeq, int16_t x, int16_t y,
      const SessionKey &key, uint8_t out[kPacketSize]
  );
  static bool verifyAndDecode(const uint8_t *buf, size_t len, const SessionKey &key, DecodedPacket &out);
  static bool peekClientId(const uint8_t *buf, size_t len, uint32_t &clientIdOut);

  // Server role API
  bool openServer(const NetworkAddress &listenAddr);
  uint16_t boundPort() const
  {
    return m_boundPort;
  }
  ClientRegistration registerClient(const void *clientHandle);
  void unregisterClient(const void *clientHandle);
  bool isClientActive(const void *clientHandle) const;
  bool sendMouseAbs(const void *clientHandle, uint16_t enterSeq, int32_t x, int32_t y);
  bool sendMouseRel(const void *clientHandle, uint16_t enterSeq, int32_t dx, int32_t dy);

  // Client role API
  bool openClient(const NetworkAddress &serverAddr, uint16_t udpPort, uint32_t clientId, const SessionKey &key);
  void sendClientPing();
  bool consumeClientMotion(ClientMotionBatch &out);
  bool isServerHandshakeAcked() const
  {
    return m_clientAcked.load(std::memory_order_acquire);
  }

private:
  struct ServerPeer
  {
    uint32_t clientId = 0;
    SessionKey key{};
    ArchNetAddress peerAddr = nullptr;
    double lastHeardTime = 0.0;
    uint32_t lastRxPingSeq = 0;
    uint32_t txSeq = 0;
    uint16_t cumRelX = 0;
    uint16_t cumRelY = 0;
    bool loggedReady = false;
  };

  ISocketMultiplexerJob *serviceSocket(ISocketMultiplexerJob *job, bool read, bool write, bool error);
  ISocketMultiplexerJob *newReadJob();
  void handleServerDatagram(const uint8_t *buf, size_t len, ArchNetAddress fromAddr);
  void handleClientDatagram(const uint8_t *buf, size_t len);
  void armClientPingTimer();
  void disarmClientPingTimer();

  IEventQueue *m_events = nullptr;
  SocketMultiplexer *m_multiplexer = nullptr;
  ArchSocket m_socket = nullptr;
  bool m_isServer = false;
  uint16_t m_boundPort = 0;

  mutable std::mutex m_mutex;

  // Server state
  uint32_t m_nextClientId = 1;
  std::map<const void *, uint32_t> m_handleToId;
  std::map<uint32_t, ServerPeer> m_peersById;

  // Client state
  uint32_t m_clientId = 0;
  SessionKey m_clientKey{};
  ArchNetAddress m_serverPeerAddr = nullptr;
  uint32_t m_clientPingSeq = 0;
  EventQueueTimer *m_pingTimer = nullptr;
  std::atomic<bool> m_clientAcked{false};

  uint32_t m_lastRxAbsSeq = 0;
  uint32_t m_lastRxRelSeq = 0;
  int16_t m_rxCumRelX = 0;
  int16_t m_rxCumRelY = 0;
  bool m_hasRelBaseline = false;

  ClientMotionBatch m_pendingBatch{};
  std::atomic<bool> m_notifyQueued{false};
};
