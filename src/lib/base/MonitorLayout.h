/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "base/MonitorEdge.h"

#include <string>

namespace deskflow {

//! One local monitor or a remote computer's aggregate desktop.
struct DisplayPlacement
{
  std::string computer;
  std::string id;
  MonitorRect rect; //!< Editable layout coordinates, independent of the OS desktop.
  bool local;
  MonitorRect nativeRect{}; //!< Native local coordinates; unused for remote desktops.

  //! Fall back to the placement when no separate native geometry is provided.
  MonitorRect nativeGeometry() const
  {
    return nativeRect.width > 0 && nativeRect.height > 0 ? nativeRect : rect;
  }
};

//! Contact ranges as fractions of exposed native and remote edges.
struct MonitorLink
{
  std::string monitor;
  std::string remote;
  Direction side;
  double localStart;
  double localEnd;
  double remoteStart;
  double remoteEnd;
};

//! Planned contacts or a validation error; links are usable only when error is empty.
struct MonitorLayoutResult
{
  std::vector<MonitorLink> links;
  std::string error;
};

//! Derive bidirectional links from touching, exposed local/remote edges.
MonitorLayoutResult planMonitorLayout(const std::vector<DisplayPlacement> &displays);

} // namespace deskflow
