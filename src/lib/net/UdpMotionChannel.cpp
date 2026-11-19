/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "net/UdpMotionChannel.h"

#include "arch/Arch.h"
#include "arch/ArchException.h"
#include "base/Event.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "deskflow/OptionTypes.h"
#include "net/SocketMultiplexer.h"
#include "net/TSocketMultiplexerMethodJob.h"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>

namespace {

constexpr uint8_t kMagic0 = 'D';
constexpr uint8_t kMagic1 = 'F';
constexpr uint8_t kMagic2 = 'U';
constexpr uint8_t kMagic3 = '1';
constexpr double kPeerActiveTimeoutSeconds = 8.0;
constexpr double kClientPingIntervalSeconds = 2.0;
constexpr size_t kMaxDatagramsPerPoll = 64;

uint64_t rotl64(uint64_t v, int bits)
{
  return (v << bits) | (v >> (64 - bits));
}

void sipRound(uint64_t &v0, uint64_t &v1, uint64_t &v2, uint64_t &v3)
{
  v0 += v1;
  v1 = rotl64(v1, 13);
  v1 ^= v0;
  v0 = rotl64(v0, 32);
  v2 += v3;
  v3 = rotl64(v3, 16);
  v3 ^= v2;
  v0 += v3;
  v3 = rotl64(v3, 21);
  v3 ^= v0;
  v2 += v1;
  v1 = rotl64(v1, 17);
  v1 ^= v2;
  v2 = rotl64(v2, 32);
}

uint64_t loadLE64(const uint8_t *p)
{
  return static_cast<uint64_t>(p[0]) | (static_cast<uint64_t>(p[1]) << 8) | (static_cast<uint64_t>(p[2]) << 16) |
         (static_cast<uint64_t>(p[3]) << 24) | (static_cast<uint64_t>(p[4]) << 32) |
         (static_cast<uint64_t>(p[5]) << 40) | (static_cast<uint64_t>(p[6]) << 48) |
         (static_cast<uint64_t>(p[7]) << 56);
}

uint32_t loadLE32(const uint8_t *p)
{
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

void writeBE16(uint8_t *p, uint16_t v)
{
  p[0] = static_cast<uint8_t>((v >> 8) & 0xffU);
  p[1] = static_cast<uint8_t>(v & 0xffU);
}

void writeBE32(uint8_t *p, uint32_t v)
{
  p[0] = static_cast<uint8_t>((v >> 24) & 0xffU);
  p[1] = static_cast<uint8_t>((v >> 16) & 0xffU);
  p[2] = static_cast<uint8_t>((v >> 8) & 0xffU);
  p[3] = static_cast<uint8_t>(v & 0xffU);
}

void writeBE64(uint8_t *p, uint64_t v)
{
  writeBE32(p, static_cast<uint32_t>((v >> 32) & 0xffffffffULL));
  writeBE32(p + 4, static_cast<uint32_t>(v & 0xffffffffULL));
}

uint16_t readBE16(const uint8_t *p)
{
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]));
}

uint32_t readBE32(const uint8_t *p)
{
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint32_t sanitizeOptionWord(uint32_t word)
{
  // Avoid colliding with legacy Client::setOptions OptionIDs if an older client connects.
  while (word == 0 || word == kOptionClipboardSharing || word == kOptionClipboardSharingSize ||
         word == kOptionRelativeMouseMoves) {
    word ^= 0x9e3779b9U;
  }
  return word;
}

uint32_t nextNonZeroSeq(uint32_t &counter)
{
  if (++counter == 0) {
    ++counter;
  }
  return counter;
}

bool isNewerSeq(uint32_t candidate, uint32_t baseline)
{
  if (candidate == 0) {
    return false;
  }
  if (baseline == 0) {
    return true;
  }
  return static_cast<int32_t>(candidate - baseline) > 0;
}

int16_t clampInt16(int32_t v)
{
  return static_cast<int16_t>(std::clamp<int32_t>(v, -32768, 32767));
}

int32_t saturatingAddInt32(int32_t a, int32_t b)
{
  const int64_t sum = static_cast<int64_t>(a) + static_cast<int64_t>(b);
  return static_cast<int32_t>(
      std::clamp<int64_t>(sum, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max())
  );
}

std::string normalizeIpString(std::string ip)
{
  for (char &ch : ip) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  static constexpr std::string_view kMappedPrefix = "::ffff:";
  if (ip.rfind(kMappedPrefix, 0) == 0) {
    ip.erase(0, kMappedPrefix.size());
  }
  return ip;
}

uint64_t
computePayloadKeystream(uint8_t rawType, uint32_t clientId, uint32_t pktSeq, const UdpMotionChannel::SessionKey &key)
{
  // Domain-separated subkey for Encrypt-then-MAC keystream generation over (type, clientId, pktSeq)
  const UdpMotionChannel::SessionKey encKey = {
      key[0] ^ 0x656e6331U, key[1] ^ 0x7374726dU, key[2] ^ 0x6d61736bU, key[3] ^ 0xdf01df01U
  };
  uint8_t nonceBlock[UdpMotionChannel::kHeaderSize]{};
  nonceBlock[0] = kMagic0;
  nonceBlock[1] = kMagic1;
  nonceBlock[2] = kMagic2;
  nonceBlock[3] = kMagic3;
  nonceBlock[4] = rawType;
  writeBE32(nonceBlock + 8, clientId);
  writeBE32(nonceBlock + 12, pktSeq);
  return UdpMotionChannel::computeMac(nonceBlock, encKey);
}

} // namespace

UdpMotionChannel::UdpMotionChannel(IEventQueue *events, SocketMultiplexer *multiplexer)
    : m_events(events),
      m_multiplexer(multiplexer)
{
}

UdpMotionChannel::~UdpMotionChannel()
{
  close();
}

void UdpMotionChannel::bind(const NetworkAddress &addr)
{
  if (m_socket != nullptr && addr.getAddress() != nullptr) {
    ARCH->bindSocket(m_socket, addr.getAddress());
  }
}

void UdpMotionChannel::close()
{
  disarmClientPingTimer();

  if (m_multiplexer != nullptr && m_socket != nullptr) {
    m_multiplexer->removeSocket(this);
  }

  std::scoped_lock lock{m_mutex};
  if (m_socket != nullptr) {
    ArchSocket sock = m_socket;
    m_socket = nullptr;
    try {
      ARCH->closeSocket(sock);
    } catch (const ArchNetworkException &e) {
      LOG_WARN("error closing UDP fast-path socket: %s", e.what());
    }
  }

  for (auto &[id, peer] : m_peersById) {
    if (peer.peerAddr != nullptr) {
      ARCH->closeAddr(peer.peerAddr);
      peer.peerAddr = nullptr;
    }
  }
  m_peersById.clear();
  m_handleToId.clear();

  if (m_serverPeerAddr != nullptr) {
    ARCH->closeAddr(m_serverPeerAddr);
    m_serverPeerAddr = nullptr;
  }

  m_boundPort = 0;
  m_clientAcked.store(false, std::memory_order_release);
  m_notifyQueued.store(false, std::memory_order_release);
  m_pendingBatch = {};
}

void *UdpMotionChannel::getEventTarget() const
{
  return const_cast<UdpMotionChannel *>(this);
}

bool UdpMotionChannel::generateSessionKey(SessionKey &out)
{
  uint8_t raw[16]{};
  if (RAND_bytes(raw, sizeof(raw)) != 1) {
    LOG_ERR("CSPRNG failure generating UDP session key; keeping motion on TCP");
    out = {};
    return false;
  }
  for (size_t i = 0; i < 4; ++i) {
    out[i] = sanitizeOptionWord(readBE32(raw + i * 4));
  }
  return true;
}

uint64_t UdpMotionChannel::computeMac(const uint8_t header[kHeaderSize], const SessionKey &key)
{
  // SipHash-2-4 over exactly 20 bytes (two 8-byte blocks + 4-byte tail with length 20)
  const uint64_t k0 = (static_cast<uint64_t>(key[0]) << 32) | static_cast<uint64_t>(key[1]);
  const uint64_t k1 = (static_cast<uint64_t>(key[2]) << 32) | static_cast<uint64_t>(key[3]);

  uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
  uint64_t v1 = 0x646f72616e646f6dULL ^ k1;
  uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
  uint64_t v3 = 0x7465646279746573ULL ^ k1;

  const uint64_t m0 = loadLE64(header);
  v3 ^= m0;
  sipRound(v0, v1, v2, v3);
  sipRound(v0, v1, v2, v3);
  v0 ^= m0;

  const uint64_t m1 = loadLE64(header + 8);
  v3 ^= m1;
  sipRound(v0, v1, v2, v3);
  sipRound(v0, v1, v2, v3);
  v0 ^= m1;

  const uint64_t m2 = (static_cast<uint64_t>(kHeaderSize) << 56) | static_cast<uint64_t>(loadLE32(header + 16));
  v3 ^= m2;
  sipRound(v0, v1, v2, v3);
  sipRound(v0, v1, v2, v3);
  v0 ^= m2;

  v2 ^= 0xffULL;
  sipRound(v0, v1, v2, v3);
  sipRound(v0, v1, v2, v3);
  sipRound(v0, v1, v2, v3);
  sipRound(v0, v1, v2, v3);

  return v0 ^ v1 ^ v2 ^ v3;
}

void UdpMotionChannel::encodePacket(
    PacketType type, uint16_t enterSeq, uint32_t clientId, uint32_t pktSeq, int16_t x, int16_t y, const SessionKey &key,
    uint8_t out[kPacketSize]
)
{
  const uint8_t rawType = static_cast<uint8_t>(type);
  const uint64_t ks = computePayloadKeystream(rawType, clientId, pktSeq, key);
  const uint16_t maskedEnter = static_cast<uint16_t>(enterSeq ^ static_cast<uint16_t>(ks & 0xffffULL));
  const uint16_t maskedX =
      static_cast<uint16_t>(static_cast<uint16_t>(x) ^ static_cast<uint16_t>((ks >> 16) & 0xffffULL));
  const uint16_t maskedY =
      static_cast<uint16_t>(static_cast<uint16_t>(y) ^ static_cast<uint16_t>((ks >> 32) & 0xffffULL));

  out[0] = kMagic0;
  out[1] = kMagic1;
  out[2] = kMagic2;
  out[3] = kMagic3;
  out[4] = rawType;
  out[5] = 0;
  writeBE16(out + 6, maskedEnter);
  writeBE32(out + 8, clientId);
  writeBE32(out + 12, pktSeq);
  writeBE16(out + 16, maskedX);
  writeBE16(out + 18, maskedY);

  // Encrypt-then-MAC over full 20-byte ciphertext header
  const uint64_t mac = computeMac(out, key);
  writeBE64(out + 20, mac);
}

bool UdpMotionChannel::peekClientId(const uint8_t *buf, size_t len, uint32_t &clientIdOut)
{
  if (buf == nullptr || len != kPacketSize) {
    return false;
  }
  if (buf[0] != kMagic0 || buf[1] != kMagic1 || buf[2] != kMagic2 || buf[3] != kMagic3) {
    return false;
  }
  clientIdOut = readBE32(buf + 8);
  return clientIdOut != 0;
}

bool UdpMotionChannel::verifyAndDecode(const uint8_t *buf, size_t len, const SessionKey &key, DecodedPacket &out)
{
  uint32_t cid = 0;
  if (!peekClientId(buf, len, cid)) {
    return false;
  }

  const uint8_t rawType = buf[4];
  if (rawType < static_cast<uint8_t>(PacketType::Ping) || rawType > static_cast<uint8_t>(PacketType::MouseWheel)) {
    return false;
  }

  const uint32_t pktSeq = readBE32(buf + 12);
  if (pktSeq == 0) {
    return false;
  }

  const uint64_t expectedMac = computeMac(buf, key);
  uint8_t expectedMacBytes[8];
  writeBE64(expectedMacBytes, expectedMac);
  if (CRYPTO_memcmp(expectedMacBytes, buf + 20, sizeof(expectedMacBytes)) != 0) {
    return false;
  }

  const uint64_t ks = computePayloadKeystream(rawType, cid, pktSeq, key);
  out.type = static_cast<PacketType>(rawType);
  out.enterSeq = static_cast<uint16_t>(readBE16(buf + 6) ^ static_cast<uint16_t>(ks & 0xffffULL));
  out.clientId = cid;
  out.pktSeq = pktSeq;
  out.x = static_cast<int16_t>(readBE16(buf + 16) ^ static_cast<uint16_t>((ks >> 16) & 0xffffULL));
  out.y = static_cast<int16_t>(readBE16(buf + 18) ^ static_cast<uint16_t>((ks >> 32) & 0xffffULL));
  return true;
}

bool UdpMotionChannel::openServer(const NetworkAddress &listenAddr)
{
  close();
  if (m_multiplexer == nullptr || listenAddr.getAddress() == nullptr) {
    return false;
  }

  const int port = listenAddr.getPort();
  if (port <= 0 || port > 65535) {
    return false;
  }

  ArchSocket sock = nullptr;
  try {
    const auto family = ARCH->getAddrFamily(listenAddr.getAddress());
    sock = ARCH->newSocket(family, IArchNetwork::SocketType::DataGram);
    ARCH->bindSocket(sock, listenAddr.getAddress());
    ARCH->setLowLatencyOnSocket(sock);

    {
      std::scoped_lock lock{m_mutex};
      m_socket = sock;
      sock = nullptr;
      m_isServer = true;
      m_boundPort = static_cast<uint16_t>(port);
    }
    m_multiplexer->addSocket(this, newReadJob());
    LOG_INFO("UDP motion fast-path listening on port %u", m_boundPort);
    return true;
  } catch (const BaseException &e) {
    if (sock != nullptr) {
      try {
        ARCH->closeSocket(sock);
      } catch (const BaseException &) {
        // ignore close failure on error path
      }
    }
    LOG_WARN("could not open UDP fast-path socket on port %d (falling back to TCP): %s", port, e.what());
    return false;
  }
}

UdpMotionChannel::ClientRegistration
UdpMotionChannel::registerClient(const void *clientHandle, const std::string &expectedPeerIp)
{
  std::scoped_lock lock{m_mutex};

  // Always rotate clientId + key + sequence state on re-registration (e.g. Server::setConfig reload)
  // so the client's fresh UDP socket and reset Ping counter re-handshake immediately without nonce reuse.
  if (const auto existing = m_handleToId.find(clientHandle); existing != m_handleToId.end()) {
    const uint32_t oldId = existing->second;
    m_handleToId.erase(existing);
    if (auto peerIt = m_peersById.find(oldId); peerIt != m_peersById.end()) {
      if (peerIt->second.peerAddr != nullptr) {
        ARCH->closeAddr(peerIt->second.peerAddr);
      }
      m_peersById.erase(peerIt);
    }
  }

  SessionKey key{};
  if (!generateSessionKey(key)) {
    return {0, {}};
  }

  uint32_t cid = sanitizeOptionWord(m_nextClientId++);
  while (m_peersById.contains(cid)) {
    cid = sanitizeOptionWord(m_nextClientId++);
  }

  ServerPeer peer{};
  peer.clientId = cid;
  peer.key = key;
  peer.expectedIp = expectedPeerIp.empty() ? std::string{} : normalizeIpString(expectedPeerIp);
  m_handleToId[clientHandle] = cid;
  m_peersById.emplace(cid, std::move(peer));
  return {cid, key};
}

void UdpMotionChannel::unregisterClient(const void *clientHandle)
{
  std::scoped_lock lock{m_mutex};
  const auto it = m_handleToId.find(clientHandle);
  if (it == m_handleToId.end()) {
    return;
  }
  const uint32_t cid = it->second;
  m_handleToId.erase(it);
  if (auto peerIt = m_peersById.find(cid); peerIt != m_peersById.end()) {
    if (peerIt->second.peerAddr != nullptr) {
      ARCH->closeAddr(peerIt->second.peerAddr);
    }
    m_peersById.erase(peerIt);
  }
}

bool UdpMotionChannel::isClientActive(const void *clientHandle) const
{
  std::scoped_lock lock{m_mutex};
  if (m_socket == nullptr) {
    return false;
  }
  const auto it = m_handleToId.find(clientHandle);
  if (it == m_handleToId.end()) {
    return false;
  }
  const auto peerIt = m_peersById.find(it->second);
  if (peerIt == m_peersById.end() || peerIt->second.peerAddr == nullptr) {
    return false;
  }
  return (Arch::time() - peerIt->second.lastHeardTime) <= kPeerActiveTimeoutSeconds;
}

bool UdpMotionChannel::sendMouseAbs(const void *clientHandle, uint16_t enterSeq, int32_t x, int32_t y)
{
  uint8_t packet[kPacketSize];
  ArchSocket sock = nullptr;
  ArchNetAddress dest = nullptr;
  {
    std::scoped_lock lock{m_mutex};
    if (m_socket == nullptr) {
      return false;
    }
    const auto it = m_handleToId.find(clientHandle);
    if (it == m_handleToId.end()) {
      return false;
    }
    auto peerIt = m_peersById.find(it->second);
    if (peerIt == m_peersById.end() || peerIt->second.peerAddr == nullptr) {
      return false;
    }
    ServerPeer &peer = peerIt->second;
    if ((Arch::time() - peer.lastHeardTime) > kPeerActiveTimeoutSeconds) {
      return false;
    }
    const uint32_t seq = nextNonZeroSeq(peer.txSeq);
    encodePacket(PacketType::MouseAbs, enterSeq, peer.clientId, seq, clampInt16(x), clampInt16(y), peer.key, packet);
    sock = m_socket;
    dest = ARCH->copyAddr(peer.peerAddr);
  }

  bool sent = false;
  try {
    sent = (ARCH->writeDatagram(sock, packet, sizeof(packet), dest) == sizeof(packet));
  } catch (const BaseException &) {
    sent = false;
  }
  ARCH->closeAddr(dest);
  return sent;
}

bool UdpMotionChannel::sendMouseRel(const void *clientHandle, uint16_t enterSeq, int32_t dx, int32_t dy)
{
  uint8_t packet[kPacketSize];
  ArchSocket sock = nullptr;
  ArchNetAddress dest = nullptr;
  {
    std::scoped_lock lock{m_mutex};
    if (m_socket == nullptr) {
      return false;
    }
    const auto it = m_handleToId.find(clientHandle);
    if (it == m_handleToId.end()) {
      return false;
    }
    auto peerIt = m_peersById.find(it->second);
    if (peerIt == m_peersById.end() || peerIt->second.peerAddr == nullptr) {
      return false;
    }
    ServerPeer &peer = peerIt->second;
    if ((Arch::time() - peer.lastHeardTime) > kPeerActiveTimeoutSeconds) {
      return false;
    }
    if (!peer.hasRelEnterSeq || peer.lastRelEnterSeq != enterSeq) {
      peer.hasRelEnterSeq = true;
      peer.lastRelEnterSeq = enterSeq;
      peer.cumRelX = 0;
      peer.cumRelY = 0;
    }
    peer.cumRelX = static_cast<uint16_t>(peer.cumRelX + static_cast<uint16_t>(clampInt16(dx)));
    peer.cumRelY = static_cast<uint16_t>(peer.cumRelY + static_cast<uint16_t>(clampInt16(dy)));
    const uint32_t seq = nextNonZeroSeq(peer.txSeq);
    encodePacket(
        PacketType::MouseRel, enterSeq, peer.clientId, seq, static_cast<int16_t>(peer.cumRelX),
        static_cast<int16_t>(peer.cumRelY), peer.key, packet
    );
    sock = m_socket;
    dest = ARCH->copyAddr(peer.peerAddr);
  }

  bool sent = false;
  try {
    sent = (ARCH->writeDatagram(sock, packet, sizeof(packet), dest) == sizeof(packet));
  } catch (const BaseException &) {
    sent = false;
  }
  ARCH->closeAddr(dest);
  return sent;
}

bool UdpMotionChannel::openClient(
    const NetworkAddress &serverAddr, uint16_t udpPort, uint32_t clientId, const SessionKey &key
)
{
  close();
  if (m_multiplexer == nullptr || serverAddr.getAddress() == nullptr || udpPort == 0 || clientId == 0) {
    return false;
  }

  try {
    const auto family = ARCH->getAddrFamily(serverAddr.getAddress());
    ArchSocket sock = ARCH->newSocket(family, IArchNetwork::SocketType::DataGram);
    ArchNetAddress anyAddr = ARCH->newAnyAddr(family);
    try {
      ARCH->bindSocket(sock, anyAddr);
    } catch (...) {
      ARCH->closeAddr(anyAddr);
      ARCH->closeSocket(sock);
      throw;
    }
    ARCH->closeAddr(anyAddr);

    ArchNetAddress peer = ARCH->copyAddr(serverAddr.getAddress());
    ARCH->setAddrPort(peer, udpPort);
    ARCH->setLowLatencyOnSocket(sock, peer);
    std::string expectedServerIp = normalizeIpString(ARCH->addrToString(peer));

    {
      std::scoped_lock lock{m_mutex};
      m_socket = sock;
      m_isServer = false;
      m_clientId = clientId;
      m_clientKey = key;
      m_serverPeerAddr = peer;
      m_expectedServerIp = std::move(expectedServerIp);
      m_expectedServerPort = udpPort;
      m_clientPingSeq = 0;
      m_lastRxAbsSeq = 0;
      m_lastRxRelSeq = 0;
      m_rxRelEnterSeq = 0;
      m_rxCumRelX = 0;
      m_rxCumRelY = 0;
      m_hasRelBaseline = false;
      m_pendingBatch = {};
    }

    m_multiplexer->addSocket(this, newReadJob());
    sendClientPing();
    armClientPingTimer();
    return true;
  } catch (const BaseException &e) {
    LOG_WARN("could not open client UDP fast-path socket (staying on TCP): %s", e.what());
    return false;
  }
}

void UdpMotionChannel::sendClientPing()
{
  uint8_t packet[kPacketSize];
  ArchSocket sock = nullptr;
  ArchNetAddress peer = nullptr;
  {
    std::scoped_lock lock{m_mutex};
    if (m_socket == nullptr || m_serverPeerAddr == nullptr) {
      return;
    }
    const uint32_t seq = nextNonZeroSeq(m_clientPingSeq);
    encodePacket(PacketType::Ping, 0, m_clientId, seq, 0, 0, m_clientKey, packet);
    sock = m_socket;
    peer = ARCH->copyAddr(m_serverPeerAddr);
  }

  try {
    ARCH->writeDatagram(sock, packet, sizeof(packet), peer);
  } catch (const BaseException &) {
    // non-fatal; periodic timer will retry
  }
  ARCH->closeAddr(peer);
}

void UdpMotionChannel::armClientPingTimer()
{
  if (m_pingTimer != nullptr || m_events == nullptr) {
    return;
  }
  m_pingTimer = m_events->newTimer(kClientPingIntervalSeconds, nullptr);
  m_events->addHandler(EventTypes::Timer, m_pingTimer, [this](const auto &) { sendClientPing(); });
}

void UdpMotionChannel::disarmClientPingTimer()
{
  if (m_pingTimer != nullptr && m_events != nullptr) {
    m_events->removeHandler(EventTypes::Timer, m_pingTimer);
    m_events->deleteTimer(m_pingTimer);
    m_pingTimer = nullptr;
  }
}

bool UdpMotionChannel::consumeClientMotion(ClientMotionBatch &out)
{
  m_notifyQueued.store(false, std::memory_order_release);
  std::scoped_lock lock{m_mutex};
  if (!m_pendingBatch.hasAbs && !m_pendingBatch.hasRel) {
    return false;
  }
  out = m_pendingBatch;
  m_pendingBatch = {};
  return true;
}

ISocketMultiplexerJob *UdpMotionChannel::newReadJob()
{
  if (m_socket == nullptr) {
    return nullptr;
  }
  return new TSocketMultiplexerMethodJob<UdpMotionChannel>(
      this, &UdpMotionChannel::serviceSocket, m_socket, true, false
  );
}

ISocketMultiplexerJob *UdpMotionChannel::serviceSocket(ISocketMultiplexerJob *job, bool read, bool, bool error)
{
  if (error || !read) {
    return job;
  }

  uint8_t buf[64];
  for (size_t pktCount = 0; pktCount < kMaxDatagramsPerPoll; ++pktCount) {
    ArchSocket sock = nullptr;
    bool isServer = false;
    {
      std::scoped_lock lock{m_mutex};
      sock = m_socket;
      isServer = m_isServer;
    }
    if (sock == nullptr) {
      return nullptr;
    }

    ArchNetAddress fromAddr = nullptr;
    size_t n = 0;
    try {
      n = ARCH->readDatagram(sock, buf, sizeof(buf), &fromAddr);
    } catch (const BaseException &) {
      if (fromAddr != nullptr) {
        ARCH->closeAddr(fromAddr);
      }
      break;
    }

    if (n == 0) {
      if (fromAddr != nullptr) {
        ARCH->closeAddr(fromAddr);
      }
      break;
    }

    if (isServer) {
      handleServerDatagram(buf, n, fromAddr);
    } else {
      handleClientDatagram(buf, n, fromAddr);
    }
    if (fromAddr != nullptr) {
      ARCH->closeAddr(fromAddr);
    }
  }

  return job;
}

void UdpMotionChannel::handleServerDatagram(const uint8_t *buf, size_t len, ArchNetAddress fromAddr)
{
  if (fromAddr == nullptr) {
    return;
  }

  uint32_t cid = 0;
  if (!peekClientId(buf, len, cid)) {
    return;
  }

  const std::string senderIp = normalizeIpString(ARCH->addrToString(fromAddr));
  const int senderPort = ARCH->getAddrPort(fromAddr);

  uint8_t ackPacket[kPacketSize];
  ArchSocket sock = nullptr;
  ArchNetAddress replyAddr = nullptr;
  bool configureQos = false;

  {
    std::scoped_lock lock{m_mutex};
    auto it = m_peersById.find(cid);
    if (it == m_peersById.end()) {
      return;
    }
    ServerPeer &peer = it->second;
    if (!peer.expectedIp.empty() && senderIp != peer.expectedIp) {
      return;
    }

    DecodedPacket pkt{};
    if (!verifyAndDecode(buf, len, peer.key, pkt) || pkt.type != PacketType::Ping) {
      return;
    }
    if (!isNewerSeq(pkt.pktSeq, peer.lastRxPingSeq)) {
      return;
    }
    peer.lastRxPingSeq = pkt.pktSeq;
    peer.lastHeardTime = Arch::time();

    if (peer.peerAddr == nullptr || !ARCH->isEqualAddr(peer.peerAddr, fromAddr)) {
      if (peer.peerAddr != nullptr) {
        ARCH->closeAddr(peer.peerAddr);
      }
      peer.peerAddr = ARCH->copyAddr(fromAddr);
      configureQos = true;
    }
    if (!peer.loggedReady) {
      peer.loggedReady = true;
      LOG_INFO("UDP motion fast-path active for clientId=%u (%s:%d)", cid, senderIp.c_str(), senderPort);
    }

    const uint32_t seq = nextNonZeroSeq(peer.txSeq);
    encodePacket(
        PacketType::Ack, peer.lastRelEnterSeq, peer.clientId, seq, static_cast<int16_t>(peer.cumRelX),
        static_cast<int16_t>(peer.cumRelY), peer.key, ackPacket
    );
    sock = m_socket;
    replyAddr = ARCH->copyAddr(peer.peerAddr);
  }

  if (sock != nullptr && replyAddr != nullptr) {
    if (configureQos) {
      ARCH->setLowLatencyOnSocket(sock, replyAddr);
    }
    try {
      ARCH->writeDatagram(sock, ackPacket, sizeof(ackPacket), replyAddr);
    } catch (const BaseException &) {
      // ignore transient error
    }
  }
  if (replyAddr != nullptr) {
    ARCH->closeAddr(replyAddr);
  }
}

void UdpMotionChannel::handleClientDatagram(const uint8_t *buf, size_t len, ArchNetAddress fromAddr)
{
  if (fromAddr == nullptr) {
    return;
  }

  const std::string senderIp = normalizeIpString(ARCH->addrToString(fromAddr));
  const int senderPort = ARCH->getAddrPort(fromAddr);

  bool shouldNotify = false;
  {
    std::scoped_lock lock{m_mutex};
    if (senderPort != static_cast<int>(m_expectedServerPort) ||
        (!m_expectedServerIp.empty() && senderIp != m_expectedServerIp)) {
      return;
    }

    DecodedPacket pkt{};
    if (!verifyAndDecode(buf, len, m_clientKey, pkt) || pkt.clientId != m_clientId) {
      return;
    }

    switch (pkt.type) {
    case PacketType::Ack:
      if (!m_hasRelBaseline) {
        m_rxCumRelX = pkt.x;
        m_rxCumRelY = pkt.y;
        m_rxRelEnterSeq = pkt.enterSeq;
        m_hasRelBaseline = true;
      }
      if (!m_clientAcked.exchange(true, std::memory_order_release)) {
        LOG_INFO("UDP motion fast-path connected to server");
      }
      break;

    case PacketType::MouseAbs:
      if (isNewerSeq(pkt.pktSeq, m_lastRxAbsSeq)) {
        m_lastRxAbsSeq = pkt.pktSeq;
        m_pendingBatch.hasAbs = true;
        m_pendingBatch.absEnterSeq = pkt.enterSeq;
        m_pendingBatch.absX = pkt.x;
        m_pendingBatch.absY = pkt.y;
        shouldNotify = true;
      }
      break;

    case PacketType::MouseRel:
      if (isNewerSeq(pkt.pktSeq, m_lastRxRelSeq)) {
        m_lastRxRelSeq = pkt.pktSeq;
        if (!m_hasRelBaseline || m_rxRelEnterSeq != pkt.enterSeq) {
          m_rxCumRelX = 0;
          m_rxCumRelY = 0;
          m_rxRelEnterSeq = pkt.enterSeq;
          m_hasRelBaseline = true;
        }
        if (!m_pendingBatch.hasRel || m_pendingBatch.relEnterSeq != pkt.enterSeq) {
          m_pendingBatch.relDx = 0;
          m_pendingBatch.relDy = 0;
        }
        const auto dx = static_cast<int16_t>(static_cast<uint16_t>(pkt.x) - static_cast<uint16_t>(m_rxCumRelX));
        const auto dy = static_cast<int16_t>(static_cast<uint16_t>(pkt.y) - static_cast<uint16_t>(m_rxCumRelY));
        m_rxCumRelX = pkt.x;
        m_rxCumRelY = pkt.y;
        m_pendingBatch.hasRel = true;
        m_pendingBatch.relEnterSeq = pkt.enterSeq;
        m_pendingBatch.relDx = saturatingAddInt32(m_pendingBatch.relDx, dx);
        m_pendingBatch.relDy = saturatingAddInt32(m_pendingBatch.relDy, dy);
        shouldNotify = true;
      }
      break;

    default:
      break;
    }
  }

  if (shouldNotify && m_events != nullptr) {
    if (!m_notifyQueued.exchange(true, std::memory_order_acq_rel)) {
      m_events->addEvent(Event(EventTypes::StreamInputReady, getEventTarget()));
    }
  }
}
