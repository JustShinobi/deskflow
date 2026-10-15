/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "base/DirectionTypes.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace deskflow {

struct MonitorRect
{
  int32_t x;
  int32_t y;
  int32_t width;
  int32_t height;

  bool operator==(const MonitorRect &) const = default;
};

//! Maps an exposed monitor edge to the corresponding virtual desktop edge.
class MonitorEdge
{
public:
  //! Select a physical edge and subtract segments covered by other local monitors.
  MonitorEdge(Direction side, MonitorRect monitor, MonitorRect desktop, const std::vector<MonitorRect> &monitors);

  Direction side() const;
  bool empty() const;

  //! Check that an outward movement originates on the selected monitor.
  bool contains(int32_t x, int32_t y) const;

  //! Project a point in the monitor's jump zone into desktop coordinates.
  bool project(int32_t &x, int32_t &y, int32_t zoneSize) const;

  //! Restore an incoming desktop point to an exposed part of the monitor.
  bool restore(int32_t &x, int32_t &y, int32_t zoneSize) const;

private:
  bool vertical() const;
  int32_t monitorBoundary() const;
  int32_t desktopBoundary() const;
  int32_t inwardSign() const;
  int32_t length() const;

  Direction m_side;
  MonitorRect m_monitor;
  MonitorRect m_desktop;
  std::vector<MonitorRect> m_monitors;
  std::vector<std::pair<int32_t, int32_t>> m_segments;
};

} // namespace deskflow
