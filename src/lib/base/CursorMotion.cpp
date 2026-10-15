/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "base/CursorMotion.h"

#include <algorithm>
#include <cstdlib>

namespace deskflow {

void CursorMotion::setPosition(int32_t x, int32_t y)
{
  m_x = x;
  m_y = y;
}

void CursorMotion::beginTransition(
    int32_t centerX, int32_t centerY, int32_t width, int32_t height, Clock::time_point now
)
{
  m_centerX = centerX;
  m_centerY = centerY;
  m_radiusX = std::max(width / RadiusDivisor, 1);
  m_radiusY = std::max(height / RadiusDivisor, 1);
  m_guardUntil = now + GracePeriod;
  setPosition(centerX, centerY);
}

std::optional<CursorMotion::Delta> CursorMotion::moveTo(int32_t x, int32_t y, Clock::time_point now)
{
  const auto fromCenterX = static_cast<int64_t>(x) - m_centerX;
  const auto fromCenterY = static_cast<int64_t>(y) - m_centerY;
  if (now < m_guardUntil && (std::abs(fromCenterX) > m_radiusX || std::abs(fromCenterY) > m_radiusY)) {
    // A rejected edge position must not become the baseline for the warp echo.
    setPosition(m_centerX, m_centerY);
    return std::nullopt;
  }

  const Delta delta{x - m_x, y - m_y};
  setPosition(x, y);
  return delta;
}

} // namespace deskflow
