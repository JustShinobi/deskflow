/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "CursorMotionTests.h"

#include "base/CursorMotion.h"

using deskflow::CursorMotion;
using namespace std::chrono_literals;

void CursorMotionTests::moveTo_delayedEdgeEvent_rejected_data()
{
  QTest::addColumn<int>("x");
  QTest::addColumn<int>("y");
  QTest::newRow("left-edge-slow-crossing") << 0 << 720;
  QTest::newRow("right-edge") << 5119 << 720;
  QTest::newRow("top-edge") << 2560 << 0;
  QTest::newRow("bottom-edge") << 2560 << 1439;
  QTest::newRow("negative-desktop-coordinate") << -2983 << -1000;
}

void CursorMotionTests::moveTo_delayedEdgeEvent_rejected()
{
  QFETCH(int, x);
  QFETCH(int, y);
  CursorMotion motion;
  const CursorMotion::Clock::time_point start{};
  motion.beginTransition(2560, 720, 5120, 1440, start);

  QVERIFY(!motion.moveTo(x, y, start + 1ms));
}

void CursorMotionTests::moveTo_warpEchoAfterRejectedEvent_zeroDelta()
{
  CursorMotion motion;
  const CursorMotion::Clock::time_point start{};
  motion.beginTransition(2560, 720, 5120, 1440, start);
  QVERIFY(!motion.moveTo(0, 720, start + 1ms));

  const auto delta = motion.moveTo(2560, 720, start + 2ms);

  QVERIFY(delta.has_value());
  QCOMPARE(delta->x, 0);
  QCOMPARE(delta->y, 0);
}

void CursorMotionTests::moveTo_smallMovementAfterRejectedEvent_preservesDelta()
{
  CursorMotion motion;
  const CursorMotion::Clock::time_point start{};
  motion.beginTransition(2560, 720, 5120, 1440, start);
  QVERIFY(!motion.moveTo(0, 720, start + 1ms));

  const auto delta = motion.moveTo(2559, 721, start + 2ms);

  QVERIFY(delta.has_value());
  QCOMPARE(delta->x, -1);
  QCOMPARE(delta->y, 1);
}

void CursorMotionTests::moveTo_nearCenter_preservesMovement()
{
  CursorMotion motion;
  const CursorMotion::Clock::time_point start{};
  motion.beginTransition(960, 540, 1920, 1080, start);

  const auto delta = motion.moveTo(952, 543, start + 1ms);

  QVERIFY(delta.has_value());
  QCOMPARE(delta->x, -8);
  QCOMPARE(delta->y, 3);
}

void CursorMotionTests::moveTo_expiredGuard_acceptsPosition()
{
  CursorMotion motion;
  const CursorMotion::Clock::time_point start{};
  motion.beginTransition(960, 540, 1920, 1080, start);

  QVERIFY(motion.moveTo(0, 540, start + 500ms).has_value());
}

void CursorMotionTests::moveTo_repeatedStaleEvents_preservesCenterBaseline()
{
  CursorMotion motion;
  const CursorMotion::Clock::time_point start{};
  motion.beginTransition(960, 540, 1920, 1080, start);
  QVERIFY(!motion.moveTo(0, 540, start + 1ms));
  QVERIFY(!motion.moveTo(1919, 540, start + 2ms));

  const auto delta = motion.moveTo(961, 540, start + 3ms);

  QVERIFY(delta.has_value());
  QCOMPARE(delta->x, 1);
}

void CursorMotionTests::setPosition_warp_updatesBaseline()
{
  CursorMotion motion;
  const CursorMotion::Clock::time_point start{};
  motion.beginTransition(960, 540, 1920, 1080, start);
  QVERIFY(motion.moveTo(970, 540, start + 1ms));
  motion.setPosition(960, 540);

  const auto delta = motion.moveTo(961, 540, start + 2ms);

  QVERIFY(delta.has_value());
  QCOMPARE(delta->x, 1);
}

QTEST_MAIN(CursorMotionTests)
