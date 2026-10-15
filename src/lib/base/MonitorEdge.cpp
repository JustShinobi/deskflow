/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "base/MonitorEdge.h"

#include <algorithm>
#include <cmath>

namespace deskflow {

MonitorEdge::MonitorEdge(
    Direction side, MonitorRect monitor, MonitorRect desktop, const std::vector<MonitorRect> &monitors
)
    : m_side(side),
      m_monitor(monitor),
      m_desktop(desktop),
      m_monitors(monitors)
{
  if (side == Direction::NoDirection || monitor.width <= 0 || monitor.height <= 0 || desktop.width <= 0 ||
      desktop.height <= 0) {
    return;
  }

  const int32_t start = vertical() ? monitor.y : monitor.x;
  m_segments.emplace_back(start, start + (vertical() ? monitor.height : monitor.width));
  const int32_t outside = monitorBoundary() - inwardSign();
  for (const auto &other : monitors) {
    if (other == monitor) {
      continue;
    }
    const int32_t near = vertical() ? other.x : other.y;
    const int32_t far = near + (vertical() ? other.width : other.height);
    if (outside < near || outside >= far) {
      continue;
    }
    const int32_t blockedStart = vertical() ? other.y : other.x;
    const int32_t blockedEnd = blockedStart + (vertical() ? other.height : other.width);
    std::vector<std::pair<int32_t, int32_t>> exposed;
    for (const auto &[begin, end] : m_segments) {
      if (blockedEnd <= begin || blockedStart >= end) {
        exposed.emplace_back(begin, end);
      } else {
        if (begin < blockedStart) {
          exposed.emplace_back(begin, blockedStart);
        }
        if (blockedEnd < end) {
          exposed.emplace_back(blockedEnd, end);
        }
      }
    }
    m_segments = std::move(exposed);
  }
}

Direction MonitorEdge::side() const
{
  return m_side;
}

bool MonitorEdge::empty() const
{
  return m_segments.empty();
}

bool MonitorEdge::contains(int32_t x, int32_t y) const
{
  return x >= m_monitor.x && x < m_monitor.x + m_monitor.width && y >= m_monitor.y &&
         y < m_monitor.y + m_monitor.height;
}

bool MonitorEdge::vertical() const
{
  return m_side == Direction::Left || m_side == Direction::Right;
}

int32_t MonitorEdge::inwardSign() const
{
  return m_side == Direction::Left || m_side == Direction::Top ? 1 : -1;
}

int32_t MonitorEdge::monitorBoundary() const
{
  switch (m_side) {
  case Direction::Left:
    return m_monitor.x;
  case Direction::Right:
    return m_monitor.x + m_monitor.width - 1;
  case Direction::Top:
    return m_monitor.y;
  default:
    return m_monitor.y + m_monitor.height - 1;
  }
}

int32_t MonitorEdge::desktopBoundary() const
{
  switch (m_side) {
  case Direction::Left:
    return m_desktop.x;
  case Direction::Right:
    return m_desktop.x + m_desktop.width - 1;
  case Direction::Top:
    return m_desktop.y;
  default:
    return m_desktop.y + m_desktop.height - 1;
  }
}

int32_t MonitorEdge::length() const
{
  int32_t total = 0;
  for (const auto &[begin, end] : m_segments) {
    total += end - begin;
  }
  return total;
}

bool MonitorEdge::project(int32_t &x, int32_t &y, int32_t zoneSize) const
{
  if (empty() || zoneSize <= 0) {
    return false;
  }
  const int32_t depth = ((vertical() ? x : y) - monitorBoundary()) * inwardSign();
  if (depth >= zoneSize) {
    return false;
  }
  if (depth < 0) {
    for (const auto &other : m_monitors) {
      if (other != m_monitor && x >= other.x && x < other.x + other.width && y >= other.y &&
          y < other.y + other.height) {
        return false;
      }
    }
  }
  const int32_t along = vertical() ? y : x;
  int32_t offset = 0;
  for (const auto &[begin, end] : m_segments) {
    if (along >= begin && along < end) {
      const double fraction = (offset + along - begin + 0.5) / length();
      const int32_t mapped =
          static_cast<int32_t>(std::floor(fraction * (vertical() ? m_desktop.height : m_desktop.width)));
      if (vertical()) {
        x = desktopBoundary() + inwardSign() * std::max(depth, 0);
        y = m_desktop.y + mapped;
      } else {
        x = m_desktop.x + mapped;
        y = desktopBoundary() + inwardSign() * std::max(depth, 0);
      }
      return true;
    }
    offset += end - begin;
  }
  return false;
}

bool MonitorEdge::restore(int32_t &x, int32_t &y, int32_t zoneSize) const
{
  if (empty() || zoneSize <= 0) {
    return false;
  }
  const int32_t depth = ((vertical() ? x : y) - desktopBoundary()) * inwardSign();
  if (depth < 0 || depth > zoneSize) {
    return false;
  }
  const int32_t span = vertical() ? m_desktop.height : m_desktop.width;
  const int32_t along = (vertical() ? y - m_desktop.y : x - m_desktop.x);
  if (along < 0 || along >= span) {
    return false;
  }
  int32_t offset = std::clamp(static_cast<int32_t>((along + 0.5) / span * length()), 0, length() - 1);
  for (const auto &[begin, end] : m_segments) {
    if (offset < end - begin) {
      const int32_t extent = vertical() ? m_monitor.width : m_monitor.height;
      const int32_t inset = std::min(zoneSize + 1, (extent - 1) / 2);
      if (vertical()) {
        x = monitorBoundary() + inwardSign() * inset;
        y = begin + offset;
      } else {
        x = begin + offset;
        y = monitorBoundary() + inwardSign() * inset;
      }
      return true;
    }
    offset -= end - begin;
  }
  return false;
}

} // namespace deskflow
