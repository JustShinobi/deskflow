/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace deskflow {

//! Rejects delayed absolute positions after parking a captured cursor.
class CursorMotion
{
public:
  using Clock = std::chrono::steady_clock;

  struct Delta
  {
    int32_t x;
    int32_t y;
  };

  //! Reset the relative-motion baseline to a known cursor position.
  void setPosition(int32_t x, int32_t y);

  //! Start a brief stale-event guard around the primary monitor's center.
  void beginTransition(
      int32_t centerX, int32_t centerY, int32_t width, int32_t height, Clock::time_point now = Clock::now()
  );

  //! Return relative motion, or reset to the parking point for a stale event.
  std::optional<Delta> moveTo(int32_t x, int32_t y, Clock::time_point now = Clock::now());

private:
  static constexpr auto GracePeriod = std::chrono::milliseconds(500);
  static constexpr int32_t RadiusDivisor = 3;

  int32_t m_x = 0;
  int32_t m_y = 0;
  int32_t m_centerX = 0;
  int32_t m_centerY = 0;
  int32_t m_radiusX = 0;
  int32_t m_radiusY = 0;
  Clock::time_point m_guardUntil{};
};

} // namespace deskflow
