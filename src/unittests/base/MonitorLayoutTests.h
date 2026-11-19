/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once
#include <QTest>
class MonitorLayoutTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void planMonitorLayout_manualScale_mapsNativeFractions();
  void planMonitorLayout_manualPlacement_preservesNativeOcclusion();
  void planMonitorLayout_remoteBetweenMonitors_createsTwoContacts();
  void planMonitorLayout_overlappingRemote_rejectsLayout();
  void planMonitorLayout_disconnectedRemote_rejectsLayout();
  void planMonitorLayout_coveredEdge_excludesLocalTransition();
  void planMonitorLayout_competingSelectors_rejectsLayout();
};
