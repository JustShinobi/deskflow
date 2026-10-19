/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "base/MonitorLayout.h"

#include <QTransform>
#include <QWidget>
#include <functional>

namespace deskflow::gui {

//! Interactive monitor rectangles owned by MonitorLayoutWidget.
class MonitorLayoutCanvas : public QWidget
{
  friend class MonitorLayoutWidget;

public:
  explicit MonitorLayoutCanvas(QWidget *parent);

protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *) override;

private:
  std::vector<DisplayPlacement> m_items;
  std::function<void()> m_changed;
  int m_selected = -1;
  QTransform m_transform;
  QPointF m_offset;
  int m_drag = -1;
};

} // namespace deskflow::gui
