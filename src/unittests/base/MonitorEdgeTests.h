/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QTest>

class MonitorEdgeTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void project_monitorEdge_mapsToDesktop_data();
  void project_monitorEdge_mapsToDesktop();
  void restore_projectedPoint_returnsToMonitor_data();
  void restore_projectedPoint_returnsToMonitor();
  void project_adjacentMonitor_preservesLocalTransition();
  void project_separatedMonitor_preservesLocalTransition();
  void project_partialOcclusion_mapsExposedLength();
  void restore_partialOcclusion_avoidsBlockedSegment();
  void project_interiorPoint_unchanged();
  void project_invalidGeometry_unchanged();
  void project_outwardOvershoot_mapsToDesktop();
  void project_inwardTapZone_preservesDistance();
  void contains_otherMonitor_false();
  void restore_desktopInterior_unchanged();
  void restore_exposedEndpoints_staysOnMonitor();
};
