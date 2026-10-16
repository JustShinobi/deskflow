/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MonitorLayoutTests.h"
#include "base/MonitorLayout.h"
using deskflow::DisplayPlacement;
using deskflow::planMonitorLayout;
namespace {
std::vector<DisplayPlacement> setup()
{
  return {
      {"server", "lower", {0, 0, 5120, 1440}, true},
      {"server", "upper-left", {-2983, -2164, 3072, 1728}, true},
      {"server", "upper-right", {857, -1440, 3440, 1440}, true},
      {"laptop", "laptop", {-2560, -436, 2560, 1440}, false}
  };
}
} // namespace
void MonitorLayoutTests::notebookBetweenLocalMonitors()
{
  const auto result = planMonitorLayout(setup());
  QVERIFY2(result.error.empty(), result.error.c_str());
  QCOMPARE(result.links.size(), size_t(2));
  const auto &left = result.links[0];
  QCOMPARE(left.side, Direction::Left);
  QCOMPARE(left.monitor, std::string("lower"));
  QCOMPARE(left.localStart, 0.0);
  QCOMPARE(left.localEnd, 1004.0 / 1440);
  QCOMPARE(left.remoteStart, 436.0 / 1440);
  QCOMPARE(left.remoteEnd, 1.0);
  const auto &bottom = result.links[1];
  QCOMPARE(bottom.side, Direction::Bottom);
  QCOMPARE(bottom.monitor, std::string("upper-left"));
  QCOMPARE(bottom.localStart, 423.0 / 3072);
  QCOMPARE(bottom.localEnd, 2983.0 / 3072);
  QCOMPARE(bottom.remoteStart, 0.0);
  QCOMPARE(bottom.remoteEnd, 1.0);
}
void MonitorLayoutTests::overlappingComputerRejected()
{
  auto items = setup();
  items.back().rect.x = -2500;
  QVERIFY(!planMonitorLayout(items).error.empty());
}
void MonitorLayoutTests::disconnectedComputerRejected()
{
  auto items = setup();
  items.back().rect = {-10000, 10000, 1920, 1080};
  QVERIFY(!planMonitorLayout(items).error.empty());
}
void MonitorLayoutTests::coveredLocalEdgeExcluded()
{
  const std::vector<DisplayPlacement> items = {
      {"server", "a", {0, 0, 1000, 1000}, true},
      {"server", "b", {-500, 250, 500, 500}, true},
      {"laptop", "laptop", {-500, 0, 500, 250}, false}
  };
  const auto result = planMonitorLayout(items);
  QVERIFY2(result.error.empty(), result.error.c_str());
  QCOMPARE(result.links.size(), size_t(2));
  QCOMPARE(result.links[0].side, Direction::Left);
  QCOMPARE(result.links[0].localEnd, 0.5);
}
void MonitorLayoutTests::conflictingLocalSelectorsRejected()
{
  const std::vector<DisplayPlacement> items = {
      {"server", "a", {0, 0, 1000, 1000}, true},
      {"server", "b", {0, 1000, 1000, 1000}, true},
      {"laptop", "laptop", {-500, 0, 500, 2000}, false}
  };
  QVERIFY(!planMonitorLayout(items).error.empty());
}
void MonitorLayoutTests::manualPositionAndScaleMapToNativeGeometry()
{
  const std::vector<DisplayPlacement> items = {
      {"workstation", "detected-display", {3000, 2000, 800, 450}, true, {0, 0, 1600, 900}},
      {"remote-machine", "desktop", {2600, 2110, 400, 225}, false}
  };
  const auto result = planMonitorLayout(items);
  QVERIFY2(result.error.empty(), result.error.c_str());
  QCOMPARE(result.links.size(), size_t(1));
  QCOMPARE(result.links[0].localStart, 110.0 / 450);
  QCOMPARE(result.links[0].localEnd, 335.0 / 450);
  QCOMPARE(result.links[0].remoteStart, 0.0);
  QCOMPARE(result.links[0].remoteEnd, 1.0);
  QCOMPARE(items[0].nativeGeometry().width, 1600);
  QCOMPARE(items[0].nativeGeometry().x, 0);
}
void MonitorLayoutTests::manualLayoutPreservesNativeOcclusion()
{
  const std::vector<DisplayPlacement> items = {
      {"desk", "main", {3000, 2000, 800, 450}, true, {0, 0, 1600, 900}},
      {"desk", "side", {6000, 6000, 100, 225}, true, {-200, 225, 200, 450}},
      {"other", "desktop", {2600, 2000, 400, 450}, false}
  };
  const auto result = planMonitorLayout(items);
  QVERIFY2(result.error.empty(), result.error.c_str());
  QCOMPARE(result.links.size(), size_t(2));
  QCOMPARE(result.links[0].localStart, 0.0);
  QCOMPARE(result.links[0].localEnd, 0.5);
  QCOMPARE(result.links[0].remoteEnd, 0.25);
  QCOMPARE(result.links[1].localStart, 0.5);
  QCOMPARE(result.links[1].localEnd, 1.0);
  QCOMPARE(result.links[1].remoteStart, 0.75);
}
QTEST_MAIN(MonitorLayoutTests)
