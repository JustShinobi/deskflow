/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MonitorEdgeTests.h"

#include "base/MonitorEdge.h"

#include <cmath>

using deskflow::MonitorEdge;
using deskflow::MonitorRect;

namespace {
const MonitorRect Monitor{0, 0, 5120, 1440};
const MonitorRect Desktop{-2983, -2164, 8103, 3604};
const std::vector<MonitorRect> Monitors = {Monitor, {-2983, -2164, 3072, 1728}, {857, -1440, 3440, 1440}};
} // namespace

void MonitorEdgeTests::project_monitorEdge_mapsToDesktop_data()
{
  QTest::addColumn<int>("direction");
  QTest::addColumn<int>("x");
  QTest::addColumn<int>("y");
  QTest::addColumn<int>("expectedX");
  QTest::addColumn<int>("expectedY");
  QTest::newRow("left") << static_cast<int>(Direction::Left) << 0 << 720 << -2983 << -361;
  QTest::newRow("right") << static_cast<int>(Direction::Right) << 5119 << 720 << 5119 << -361;
  QTest::newRow("top-exposed") << static_cast<int>(Direction::Top) << 4708 << 0 << 3135 << -2164;
  QTest::newRow("bottom") << static_cast<int>(Direction::Bottom) << 2560 << 1439 << 1069 << 1439;
}

void MonitorEdgeTests::project_monitorEdge_mapsToDesktop()
{
  QFETCH(int, direction);
  QFETCH(int, x);
  QFETCH(int, y);
  QFETCH(int, expectedX);
  QFETCH(int, expectedY);
  const MonitorEdge edge(static_cast<Direction>(direction), Monitor, Desktop, Monitors);

  const bool mapped = edge.project(x, y, 1);

  QVERIFY(mapped);
  QCOMPARE(x, expectedX);
  QCOMPARE(y, expectedY);
}

void MonitorEdgeTests::restore_projectedPoint_returnsToMonitor_data()
{
  QTest::addColumn<int>("direction");
  QTest::addColumn<int>("x");
  QTest::addColumn<int>("y");
  QTest::newRow("left") << static_cast<int>(Direction::Left) << 0 << 720;
  QTest::newRow("right") << static_cast<int>(Direction::Right) << 5119 << 720;
  QTest::newRow("top") << static_cast<int>(Direction::Top) << 4708 << 0;
  QTest::newRow("bottom") << static_cast<int>(Direction::Bottom) << 2560 << 1439;
}

void MonitorEdgeTests::restore_projectedPoint_returnsToMonitor()
{
  QFETCH(int, direction);
  QFETCH(int, x);
  QFETCH(int, y);
  const MonitorEdge edge(static_cast<Direction>(direction), Monitor, Desktop, Monitors);
  const int originalX = x;
  const int originalY = y;
  QVERIFY(edge.project(x, y, 1));

  const bool restored = edge.restore(x, y, 1);

  QVERIFY(restored);
  QVERIFY(std::abs(x - originalX) <= 2);
  QVERIFY(std::abs(y - originalY) <= 2);
  QVERIFY(!edge.project(x, y, 1));
}

void MonitorEdgeTests::project_adjacentMonitor_preservesLocalTransition()
{
  const MonitorEdge edge(Direction::Left, Monitor, Desktop, {Monitor, {-1920, 0, 1920, 1440}});
  int x = 0;
  int y = 720;

  QVERIFY(!edge.project(x, y, 1));
}

void MonitorEdgeTests::project_separatedMonitor_preservesLocalTransition()
{
  const MonitorEdge edge(Direction::Left, Monitor, Desktop, {Monitor, {-2000, 0, 1920, 1440}});
  int x = -100;
  int y = 720;

  QVERIFY(!edge.project(x, y, 1));
}

void MonitorEdgeTests::project_partialOcclusion_mapsExposedLength()
{
  const MonitorEdge edge(Direction::Left, Monitor, Desktop, {Monitor, {-100, 360, 100, 720}});
  int x = 0;
  int y = 1080;

  QVERIFY(edge.project(x, y, 1));
  QCOMPARE(y, -360);
}

void MonitorEdgeTests::restore_partialOcclusion_avoidsBlockedSegment()
{
  const MonitorEdge edge(Direction::Left, Monitor, Desktop, {Monitor, {-100, 360, 100, 720}});
  int x = -2982;
  int y = -362;

  QVERIFY(edge.restore(x, y, 1));
  QVERIFY(y < 360 || y >= 1080);
}

void MonitorEdgeTests::project_interiorPoint_unchanged()
{
  const MonitorEdge edge(Direction::Left, Monitor, Desktop, Monitors);
  int x = 100;
  int y = 720;

  QVERIFY(!edge.project(x, y, 1));
  QCOMPARE(x, 100);
}

void MonitorEdgeTests::project_invalidGeometry_unchanged()
{
  const MonitorEdge edge(Direction::Left, {0, 0, 0, 1440}, Desktop, Monitors);
  int x = 0;
  int y = 720;

  QVERIFY(!edge.project(x, y, 1));
}

void MonitorEdgeTests::project_outwardOvershoot_mapsToDesktop()
{
  const MonitorEdge edge(Direction::Left, Monitor, Desktop, Monitors);
  int x = -150;
  int y = 720;

  QVERIFY(edge.project(x, y, 1));
  QCOMPARE(x, Desktop.x);
}

void MonitorEdgeTests::restore_desktopInterior_unchanged()
{
  const MonitorEdge edge(Direction::Left, Monitor, Desktop, Monitors);
  int x = 100;
  int y = 720;

  QVERIFY(!edge.restore(x, y, 1));
  QCOMPARE(x, 100);
}

void MonitorEdgeTests::project_inwardTapZone_preservesDistance()
{
  const MonitorEdge edge(Direction::Left, Monitor, Desktop, Monitors);
  int x = 3;
  int y = 720;

  QVERIFY(edge.project(x, y, 4));
  QCOMPARE(x, Desktop.x + 3);
}

void MonitorEdgeTests::contains_otherMonitor_false()
{
  const MonitorEdge edge(Direction::Left, Monitor, Desktop, Monitors);

  QVERIFY(!edge.contains(-2000, -1000));
}

void MonitorEdgeTests::restore_exposedEndpoints_staysOnMonitor()
{
  const MonitorEdge edge(Direction::Left, Monitor, Desktop, Monitors);
  for (const int point : {Desktop.y, Desktop.y + Desktop.height - 1}) {
    int x = Desktop.x + 1;
    int y = point;
    QVERIFY(edge.restore(x, y, 1));
    QVERIFY(y >= Monitor.y && y < Monitor.y + Monitor.height);
  }
}

void MonitorEdgeTests::restore_explicitEntry_preservesMonitorWithOvershoot()
{
  for (const auto direction : {Direction::Left, Direction::Right, Direction::Top, Direction::Bottom}) {
    const MonitorEdge edge(direction, Monitor, Desktop, Monitors);
    for (const int depth : {2, 9, 100, 10000}) {
      int x = 0;
      int y = 720;
      if (direction == Direction::Top || direction == Direction::Bottom) {
        x = 4708;
        y = direction == Direction::Top ? 0 : 1439;
      } else if (direction == Direction::Right) {
        x = 5119;
      }
      QVERIFY(edge.project(x, y, 1));
      if (direction == Direction::Left)
        x += depth;
      if (direction == Direction::Right)
        x -= depth;
      if (direction == Direction::Top)
        y += depth;
      if (direction == Direction::Bottom)
        y -= depth;
      QVERIFY(edge.restore(x, y, 1, true));
      QVERIFY(x >= 0 && x < Monitor.width);
      QVERIFY(y >= 0 && y < Monitor.height);
      if (direction == Direction::Left)
        QCOMPARE(x, 2);
      if (direction == Direction::Right)
        QCOMPARE(x, Monitor.width - 3);
      if (direction == Direction::Top)
        QCOMPARE(y, 2);
      if (direction == Direction::Bottom)
        QCOMPARE(y, Monitor.height - 3);
      QVERIFY(!edge.project(x, y, 1));
    }
  }
}

QTEST_MAIN(MonitorEdgeTests)
