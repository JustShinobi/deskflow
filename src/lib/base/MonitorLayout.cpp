/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "base/MonitorLayout.h"

#include <algorithm>
#include <map>
#include <set>

namespace deskflow {

MonitorLayoutResult planMonitorLayout(const std::vector<DisplayPlacement> &displays)
{
  MonitorLayoutResult result;
  std::vector<MonitorRect> locals;
  std::set<std::string> remotes;
  for (const auto &display : displays) {
    if (display.rect.width < 2 || display.rect.height < 2) {
      result.error = "Each display must have a valid resolution.";
      return result;
    }
    if (display.local) {
      locals.push_back(display.nativeGeometry());
    } else if (!remotes.insert(display.computer).second) {
      result.error = "This version supports one desktop rectangle per remote computer.";
      return result;
    }
  }
  if (locals.empty()) {
    result.error = "No local displays detected.";
    return result;
  }
  std::map<Direction, std::string> selected;
  std::set<std::string> connected;
  for (size_t i = 0; i < displays.size(); ++i) {
    const auto &a = displays[i];
    for (size_t j = i + 1; j < displays.size(); ++j) {
      const auto &b = displays[j];
      if (std::max(a.rect.x, b.rect.x) < std::min(a.rect.x + a.rect.width, b.rect.x + b.rect.width) &&
          std::max(a.rect.y, b.rect.y) < std::min(a.rect.y + a.rect.height, b.rect.y + b.rect.height)) {
        result.error = "Display layout rectangles must not overlap.";
        return result;
      }
    }
  }
  for (const auto &local : displays) {
    if (!local.local)
      continue;
    const auto &a = local.rect;
    for (const auto &remote : displays) {
      if (remote.local)
        continue;
      const auto &b = remote.rect;
      Direction side = Direction::NoDirection;
      if (a.x == b.x + b.width)
        side = Direction::Left;
      else if (a.x + a.width == b.x)
        side = Direction::Right;
      else if (a.y == b.y + b.height)
        side = Direction::Top;
      else if (a.y + a.height == b.y)
        side = Direction::Bottom;
      if (side == Direction::NoDirection)
        continue;
      const bool vertical = side == Direction::Left || side == Direction::Right;
      const int remoteBegin = vertical ? b.y : b.x;
      const int remoteLength = vertical ? b.height : b.width;
      const auto native = local.nativeGeometry();
      const MonitorEdge edge(side, native, native, locals);
      const int nativeOrigin = vertical ? native.y : native.x;
      const int nativeSpan = vertical ? native.height : native.width;
      const int layoutOrigin = vertical ? a.y : a.x;
      const int layoutSpan = vertical ? a.height : a.width;
      int length = 0;
      for (const auto &[begin, end] : edge.segments())
        length += end - begin;
      int offset = 0;
      for (const auto &[begin, end] : edge.segments()) {
        const double exposedStart = layoutOrigin + static_cast<double>(begin - nativeOrigin) * layoutSpan / nativeSpan;
        const double exposedEnd = layoutOrigin + static_cast<double>(end - nativeOrigin) * layoutSpan / nativeSpan;
        const double start = std::max(exposedStart, static_cast<double>(remoteBegin));
        const double stop = std::min(exposedEnd, static_cast<double>(remoteBegin + remoteLength));
        if (start < stop) {
          if (selected.contains(side) && selected[side] != local.id) {
            result.error = "Only one local monitor can be linked in each direction.";
            return result;
          }
          selected[side] = local.id;
          result.links.push_back(
              {local.id, remote.computer, side,
               (offset + (start - layoutOrigin) * nativeSpan / layoutSpan + nativeOrigin - begin) / length,
               (offset + (stop - layoutOrigin) * nativeSpan / layoutSpan + nativeOrigin - begin) / length,
               static_cast<double>(start - remoteBegin) / remoteLength,
               static_cast<double>(stop - remoteBegin) / remoteLength}
          );
          connected.insert(remote.computer);
        }
        offset += end - begin;
      }
    }
  }
  if (remotes.empty() || connected != remotes) {
    result.error = "Each remote desktop must touch a local edge that is exposed in Windows.";
  }
  return result;
}

} // namespace deskflow
