/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "base/MonitorEdge.h"

#include <string>

namespace deskflow {

struct DisplayPlacement
{
  std::string computer;
  std::string id;
  MonitorRect rect;
  bool local;
};

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

struct MonitorLayoutResult
{
  std::vector<MonitorLink> links;
  std::string error;
};

//! Derive bidirectional links from touching, exposed local/remote edges.
MonitorLayoutResult planMonitorLayout(const std::vector<DisplayPlacement> &displays);

} // namespace deskflow
