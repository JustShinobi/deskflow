/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MonitorLayoutCanvas.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <algorithm>
#include <cmath>

namespace deskflow::gui {
namespace {
QString displayLabel(const DisplayPlacement &display)
{
  QString label = QString::fromStdString(display.id);
  for (const auto *screen : QGuiApplication::screens()) {
    if (screen->name() == label && !screen->model().isEmpty()) {
      label = screen->model() + "\n" + label;
      break;
    }
  }
  const auto native = display.nativeGeometry();
  QString dimensions =
      display.local
          ? QCoreApplication::translate("MonitorLayoutWidget", "Detected: %1 x %2").arg(native.width).arg(native.height)
          : QCoreApplication::translate("MonitorLayoutWidget", "Desktop: %1 x %2")
                .arg(display.rect.width)
                .arg(display.rect.height);
  if (display.local && display.rect != native)
    dimensions += "\n" + QCoreApplication::translate("MonitorLayoutWidget", "Layout: %1 x %2")
                             .arg(display.rect.width)
                             .arg(display.rect.height);
  return QString::fromStdString(display.computer) + "\n" + label + "\n" + dimensions;
}
} // namespace

MonitorLayoutCanvas::MonitorLayoutCanvas(QWidget *parent) : QWidget(parent)
{
  setObjectName("monitorCanvas");
  setMinimumSize(680, 360);
  setMouseTracking(true);
}

void MonitorLayoutCanvas::paintEvent(QPaintEvent *)
{
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.fillRect(rect(), palette().base());
  QRectF bounds;
  for (const auto &item : m_items) {
    const auto &r = item.rect;
    bounds = bounds.united(QRectF(r.x, r.y, r.width, r.height));
  }
  if (bounds.isEmpty())
    return;
  if (m_drag < 0) {
    const double scale = std::min((width() - 40) / bounds.width(), (height() - 40) / bounds.height());
    m_transform = QTransform();
    m_transform.translate((width() - bounds.width() * scale) / 2, (height() - bounds.height() * scale) / 2);
    m_transform.scale(scale, scale);
    m_transform.translate(-bounds.x(), -bounds.y());
  }
  for (size_t i = 0; i < m_items.size(); ++i) {
    const auto &item = m_items[i];
    const auto &r = item.rect;
    const QRectF box = m_transform.mapRect(QRectF(r.x, r.y, r.width, r.height));
    painter.setBrush(item.local ? QColor(50, 85, 115) : QColor(105, 70, 140));
    painter.setPen(QPen(static_cast<int>(i) == m_selected ? QColor(255, 195, 60) : QColor(150, 170, 190), 2));
    painter.drawRect(box.adjusted(1, 1, -1, -1));
    painter.setPen(Qt::white);
    painter.drawText(box.adjusted(5, 5, -5, -5), Qt::AlignCenter, displayLabel(item));
  }
  const auto plan = deskflow::planMonitorLayout(m_items);
  if (plan.error.empty()) {
    painter.setPen(QPen(QColor(70, 220, 145), 4));
    for (const auto &link : plan.links) {
      const auto local = std::find_if(m_items.begin(), m_items.end(), [&](const auto &item) {
        return item.local && item.id == link.monitor;
      });
      const auto remote = std::find_if(m_items.begin(), m_items.end(), [&](const auto &item) {
        return !item.local && item.computer == link.remote;
      });
      if (local == m_items.end() || remote == m_items.end())
        continue;
      const auto &a = local->rect;
      const auto &b = remote->rect;
      const bool vertical = link.side == Direction::Left || link.side == Direction::Right;
      const double start = (vertical ? b.y : b.x) + link.remoteStart * (vertical ? b.height : b.width);
      const double end = (vertical ? b.y : b.x) + link.remoteEnd * (vertical ? b.height : b.width);
      const double edge = vertical ? (link.side == Direction::Left ? a.x : a.x + a.width)
                                   : (link.side == Direction::Top ? a.y : a.y + a.height);
      painter.drawLine(
          m_transform.map(vertical ? QPointF(edge, start) : QPointF(start, edge)),
          m_transform.map(vertical ? QPointF(edge, end) : QPointF(end, edge))
      );
    }
  }
}

void MonitorLayoutCanvas::mousePressEvent(QMouseEvent *event)
{
  if (event->button() != Qt::LeftButton)
    return;
  const auto point = m_transform.inverted().map(event->position());
  for (size_t i = 0; i < m_items.size(); ++i) {
    auto &item = m_items[i];
    const auto &r = item.rect;
    if (QRectF(r.x, r.y, r.width, r.height).contains(point)) {
      m_selected = m_drag = static_cast<int>(i);
      if (item.local && item.nativeRect.width == 0)
        item.nativeRect = item.rect;
      m_offset = point - QPointF(r.x, r.y);
      if (m_changed)
        m_changed();
      update();
      return;
    }
  }
}

void MonitorLayoutCanvas::mouseMoveEvent(QMouseEvent *event)
{
  if (m_drag < 0)
    return;
  auto &r = m_items.at(m_drag).rect;
  const auto point = m_transform.inverted().map(event->position()) - m_offset;
  r.x = qRound(point.x());
  r.y = qRound(point.y());
  const int threshold = qRound(15 / m_transform.m11());
  int bestX = threshold + 1, bestY = threshold + 1;
  int snapX = r.x, snapY = r.y;
  for (size_t i = 0; i < m_items.size(); ++i) {
    if (static_cast<int>(i) == m_drag)
      continue;
    const auto &other = m_items[i].rect;
    for (const int x : {other.x, other.x + other.width}) {
      for (const int candidate : {x, x - r.width}) {
        const int distance = std::abs(candidate - r.x);
        if (distance < bestX) {
          bestX = distance;
          snapX = candidate;
        }
      }
    }
    for (const int y : {other.y, other.y + other.height}) {
      for (const int candidate : {y, y - r.height}) {
        const int distance = std::abs(candidate - r.y);
        if (distance < bestY) {
          bestY = distance;
          snapY = candidate;
        }
      }
    }
  }
  r.x = snapX;
  r.y = snapY;
  if (m_changed)
    m_changed();
  update();
}

void MonitorLayoutCanvas::mouseReleaseEvent(QMouseEvent *)
{
  m_drag = -1;
  update();
}

} // namespace deskflow::gui
