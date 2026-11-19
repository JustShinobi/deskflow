/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "UdpMotionChannelTests.h"

#include "base/EventQueue.h"
#include "net/NetworkAddress.h"
#include "net/SocketMultiplexer.h"
#include "net/UdpMotionChannel.h"

#include <QHostAddress>
#include <QTest>
#include <QUdpSocket>

#include <cstdint>

namespace {

uint16_t findAvailableLocalUdpPort()
{
  QUdpSocket probe;
  if (!probe.bind(QHostAddress::LocalHost, 0)) {
    return 0;
  }
  return probe.localPort();
}

uint16_t peekWireBE16(const uint8_t *p)
{
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]));
}

} // namespace

void UdpMotionChannelTests::initTestCase()
{
  m_arch.init();
  m_log.setFilter(LogLevel::Level::Warning);
}

void UdpMotionChannelTests::encodeAndVerify_roundTripAndTamperRejection()
{
  UdpMotionChannel::SessionKey key{};
  QVERIFY(UdpMotionChannel::generateSessionKey(key));
  uint8_t wire[UdpMotionChannel::kPacketSize]{};

  UdpMotionChannel::encodePacket(UdpMotionChannel::PacketType::MouseAbs, 42, 0x12345678u, 99u, -1920, 1080, key, wire);

  // Sensitive coordinates/enterSeq must be masked on the wire (Encrypt-then-MAC)
  const bool allPlaintextOnWire = (peekWireBE16(wire + 6) == 42u) &&
                                  (static_cast<int16_t>(peekWireBE16(wire + 16)) == -1920) &&
                                  (static_cast<int16_t>(peekWireBE16(wire + 18)) == 1080);
  QVERIFY(!allPlaintextOnWire);

  UdpMotionChannel::DecodedPacket decoded{};
  QVERIFY(UdpMotionChannel::verifyAndDecode(wire, sizeof(wire), key, decoded));
  QVERIFY(decoded.type == UdpMotionChannel::PacketType::MouseAbs);
  QCOMPARE(decoded.enterSeq, uint16_t{42});
  QCOMPARE(decoded.clientId, uint32_t{0x12345678u});
  QCOMPARE(decoded.pktSeq, uint32_t{99u});
  QCOMPARE(decoded.x, int16_t{-1920});
  QCOMPARE(decoded.y, int16_t{1080});

  // Flip one ciphertext coordinate bit -> MAC verification must reject before unmasking
  uint8_t tamperedPayload = wire[16];
  wire[16] ^= 0x01u;
  QVERIFY(!UdpMotionChannel::verifyAndDecode(wire, sizeof(wire), key, decoded));
  wire[16] = tamperedPayload;

  // Flip one MAC bit -> MAC verification must reject
  wire[27] ^= 0x80u;
  QVERIFY(!UdpMotionChannel::verifyAndDecode(wire, sizeof(wire), key, decoded));
  wire[27] ^= 0x80u;

  // Wrong session key -> MAC verification must reject
  auto wrongKey = key;
  wrongKey[0] ^= 0xdeadbeefu;
  QVERIFY(!UdpMotionChannel::verifyAndDecode(wire, sizeof(wire), wrongKey, decoded));
}

void UdpMotionChannelTests::loopbackHandshakeAndMotion_coalescesAbsAndRecoversRelativeDeltas()
{
  const uint16_t port = findAvailableLocalUdpPort();
  QVERIFY(port != 0);

  EventQueue events;
  SocketMultiplexer multiplexer;

  NetworkAddress listenAddr("127.0.0.1", port);
  listenAddr.resolve();

  UdpMotionChannel server(&events, &multiplexer);
  QVERIFY(server.openServer(listenAddr));
  QCOMPARE(server.boundPort(), port);

  int dummyClientHandle = 0;
  const auto reg1 = server.registerClient(&dummyClientHandle, "127.0.0.1");
  QVERIFY(reg1.clientId != 0);

  UdpMotionChannel client(&events, &multiplexer);
  QVERIFY(client.openClient(listenAddr, port, reg1.clientId, reg1.key));

  QVERIFY(
      QTest::qWaitFor(
          [&]() { return server.isClientActive(&dummyClientHandle) && client.isServerHandshakeAcked(); }, 2000
      )
  );

  // Re-registering the same client handle (simulating Server::setConfig reload) must rotate credentials
  // and immediately re-handshake on a fresh ephemeral client socket.
  const auto reg2 = server.registerClient(&dummyClientHandle, "::ffff:127.0.0.1");
  QVERIFY(reg2.clientId != 0);
  QVERIFY(reg2.clientId != reg1.clientId);
  QVERIFY(!server.isClientActive(&dummyClientHandle));

  QVERIFY(client.openClient(listenAddr, port, reg2.clientId, reg2.key));
  QVERIFY(
      QTest::qWaitFor(
          [&]() { return server.isClientActive(&dummyClientHandle) && client.isServerHandshakeAcked(); }, 2000
      )
  );

  // Send two absolute frames and two relative frames; client must coalesce latest abs and sum rel deltas
  QVERIFY(server.sendMouseAbs(&dummyClientHandle, 7, 400, 300));
  QVERIFY(server.sendMouseAbs(&dummyClientHandle, 7, 1280, 720));
  QVERIFY(server.sendMouseRel(&dummyClientHandle, 7, 15, -10));
  QVERIFY(server.sendMouseRel(&dummyClientHandle, 7, -3, 25));

  UdpMotionChannel::ClientMotionBatch received{};
  QVERIFY(
      QTest::qWaitFor(
          [&]() {
            UdpMotionChannel::ClientMotionBatch step{};
            if (client.consumeClientMotion(step)) {
              if (step.hasAbs) {
                received.hasAbs = true;
                received.absEnterSeq = step.absEnterSeq;
                received.absX = step.absX;
                received.absY = step.absY;
              }
              if (step.hasRel) {
                received.hasRel = true;
                received.relEnterSeq = step.relEnterSeq;
                received.relDx += step.relDx;
                received.relDy += step.relDy;
              }
            }
            return received.hasAbs && received.absX == 1280 && received.hasRel && received.relDx == 12 &&
                   received.relDy == 15;
          },
          2000
      )
  );

  QCOMPARE(received.absEnterSeq, uint16_t{7});
  QCOMPARE(received.absX, 1280);
  QCOMPARE(received.absY, 720);
  QCOMPARE(received.relEnterSeq, uint16_t{7});
  QCOMPARE(received.relDx, 12);
  QCOMPARE(received.relDy, 15);

  client.close();
  server.close();
}

QTEST_MAIN(UdpMotionChannelTests)
