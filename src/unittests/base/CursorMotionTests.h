/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QTest>

class CursorMotionTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void moveTo_delayedEdgeEvent_rejected_data();
  void moveTo_delayedEdgeEvent_rejected();
  void moveTo_warpEchoAfterRejectedEvent_zeroDelta();
  void moveTo_smallMovementAfterRejectedEvent_preservesDelta();
  void moveTo_nearCenter_preservesMovement();
  void moveTo_expiredGuard_acceptsPosition();
  void moveTo_repeatedStaleEvents_preservesCenterBaseline();
  void setPosition_warp_updatesBaseline();
};
