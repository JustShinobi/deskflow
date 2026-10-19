/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QTemporaryDir>
#include <QTest>

class MonitorLayoutWidgetTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void init();
  void linksConfig_touchingDisplays_generatesBidirectionalRanges();
  void commit_enabledLayout_storesSelectors();
  void commit_disabledLayout_clearsSelectors();
  void setDisplays_changedGeometry_marksModified();
  void drag_remoteDesktop_preservesLocalGeometry();
  void drag_localDisplay_preservesNativeGeometry();
  void cancel_editedLayout_leavesSettingsUnchanged();
  void save_validLayout_writesExternalLinks();
  void save_externalConfig_preservesOtherSections();
  void reload_manualLocalGeometry_restoresOverride();
  void detectDisplays_manualOverride_preservesPlacement();
  void useDetectedLayout_manualOverride_restoresNativePlacement();
  void detectDisplays_changedHardware_updatesNativeGeometry();
  void reopen_enabledLayout_keepsComputersEditable();

private:
  QTemporaryDir m_temp;
};
