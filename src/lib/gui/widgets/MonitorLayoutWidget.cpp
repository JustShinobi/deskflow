/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MonitorLayoutWidget.h"

#include "common/Settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using deskflow::DisplayPlacement;
using deskflow::MonitorRect;

namespace {
QString sideName(Direction side)
{
  switch (side) {
  case Direction::Left:
    return QStringLiteral("left");
  case Direction::Right:
    return QStringLiteral("right");
  case Direction::Top:
    return QStringLiteral("up");
  case Direction::Bottom:
    return QStringLiteral("down");
  default:
    return {};
  }
}

QString selectorKey(Direction side)
{
  switch (side) {
  case Direction::Left:
    return Settings::Server::SwitchMonitorLeft;
  case Direction::Right:
    return Settings::Server::SwitchMonitorRight;
  case Direction::Top:
    return Settings::Server::SwitchMonitorTop;
  case Direction::Bottom:
    return Settings::Server::SwitchMonitorBottom;
  default:
    return {};
  }
}

std::vector<DisplayPlacement> detectDisplays(const QString &server)
{
  std::vector<DisplayPlacement> displays;
#ifdef Q_OS_WIN
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        auto *items = reinterpret_cast<std::vector<DisplayPlacement> *>(data);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO *>(&info))) {
          const auto &r = info.rcMonitor;
          items->push_back(
              {{},
               QString::fromWCharArray(info.szDevice).toStdString(),
               {r.left, r.top, r.right - r.left, r.bottom - r.top},
               true}
          );
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&displays)
  );
#else
  for (const auto *screen : QGuiApplication::screens()) {
    const auto r = screen->geometry();
    displays.push_back({{}, screen->name().toStdString(), {r.x(), r.y(), r.width(), r.height()}, true});
  }
#endif
  for (auto &display : displays) {
    display.computer = server.toStdString();
    display.nativeRect = display.rect;
  }
  return displays;
}

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

class MonitorLayoutCanvas : public QWidget
{
public:
  explicit MonitorLayoutCanvas(QWidget *parent) : QWidget(parent)
  {
    setObjectName("monitorCanvas");
    setMinimumSize(680, 360);
    setMouseTracking(true);
  }
  std::vector<DisplayPlacement> items;
  std::function<void()> changed;
  int selected = -1;

protected:
  void paintEvent(QPaintEvent *) override
  {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), palette().base());
    QRectF bounds;
    for (const auto &item : items) {
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
    for (size_t i = 0; i < items.size(); ++i) {
      const auto &item = items[i];
      const auto &r = item.rect;
      const QRectF box = m_transform.mapRect(QRectF(r.x, r.y, r.width, r.height));
      painter.setBrush(item.local ? QColor(50, 85, 115) : QColor(105, 70, 140));
      painter.setPen(QPen(static_cast<int>(i) == selected ? QColor(255, 195, 60) : QColor(150, 170, 190), 2));
      painter.drawRect(box.adjusted(1, 1, -1, -1));
      painter.setPen(Qt::white);
      painter.drawText(box.adjusted(5, 5, -5, -5), Qt::AlignCenter, displayLabel(item));
    }
    const auto plan = deskflow::planMonitorLayout(items);
    if (plan.error.empty()) {
      painter.setPen(QPen(QColor(70, 220, 145), 4));
      for (const auto &link : plan.links) {
        const auto local = std::find_if(items.begin(), items.end(), [&](const auto &item) {
          return item.local && item.id == link.monitor;
        });
        const auto remote = std::find_if(items.begin(), items.end(), [&](const auto &item) {
          return !item.local && item.computer == link.remote;
        });
        if (local == items.end() || remote == items.end())
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
  void mousePressEvent(QMouseEvent *event) override
  {
    if (event->button() != Qt::LeftButton)
      return;
    const auto point = m_transform.inverted().map(event->position());
    for (size_t i = 0; i < items.size(); ++i) {
      auto &item = items[i];
      const auto &r = item.rect;
      if (QRectF(r.x, r.y, r.width, r.height).contains(point)) {
        selected = m_drag = static_cast<int>(i);
        if (item.local && item.nativeRect.width == 0)
          item.nativeRect = item.rect;
        m_offset = point - QPointF(r.x, r.y);
        if (changed)
          changed();
        update();
        return;
      }
    }
  }
  void mouseMoveEvent(QMouseEvent *event) override
  {
    if (m_drag < 0)
      return;
    auto &r = items.at(m_drag).rect;
    const auto point = m_transform.inverted().map(event->position()) - m_offset;
    r.x = qRound(point.x());
    r.y = qRound(point.y());
    const int threshold = qRound(15 / m_transform.m11());
    int bestX = threshold + 1, bestY = threshold + 1;
    int snapX = r.x, snapY = r.y;
    for (size_t i = 0; i < items.size(); ++i) {
      if (static_cast<int>(i) == m_drag)
        continue;
      const auto &other = items[i].rect;
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
    if (changed)
      changed();
    update();
  }
  void mouseReleaseEvent(QMouseEvent *) override
  {
    m_drag = -1;
    update();
  }

private:
  QTransform m_transform;
  QPointF m_offset;
  int m_drag = -1;
};

MonitorLayoutWidget::MonitorLayoutWidget(const QString &server, const QStringList &computers, QWidget *parent)
    : QWidget(parent),
      m_server(server),
      m_canvas(new MonitorLayoutCanvas(this)),
      m_enabled(new QCheckBox(tr("Use monitor layout"), this)),
      m_remote(new QComboBox(this)),
      m_display(new QComboBox(this)),
      m_width(new QSpinBox(this)),
      m_height(new QSpinBox(this)),
      m_x(new QSpinBox(this)),
      m_y(new QSpinBox(this)),
      m_status(new QLabel(this))
{
  auto *layout = new QVBoxLayout(this);
  layout->addWidget(m_enabled);
  auto *hint = new QLabel(
      tr("Displays are detected automatically. Select or drag any display and edit its layout size and position. "
         "Detected Windows geometry remains the reference for cursor mapping. Green edges switch in both directions. "
         "Local-to-local movement still follows Windows. One rectangle represents each remote desktop."),
      this
  );
  hint->setWordWrap(true);
  layout->addWidget(hint);
  auto *controls = new QHBoxLayout;
  for (const auto &computer : computers) {
    if (!computer.isEmpty() && computer != server && m_remote->findText(computer) < 0)
      m_remote->addItem(computer);
  }
  m_remote->setAccessibleName(tr("Remote computer"));
  controls->addWidget(m_remote);
  auto *add = new QPushButton(tr("Add remote"), this);
  auto *remove = new QPushButton(tr("Remove remote"), this);
  auto *detect = new QPushButton(tr("Detect displays"), this);
  detect->setObjectName("detectDisplays");
  auto *reset = new QPushButton(tr("Use detected layout"), this);
  reset->setObjectName("resetDetectedLayout");
  controls->addWidget(add);
  controls->addWidget(remove);
  controls->addWidget(detect);
  controls->addWidget(reset);
  controls->addStretch();
  layout->addLayout(controls);
  m_display->setObjectName("displaySelection");
  layout->addWidget(m_display);
  auto *geometry = new QHBoxLayout;
  const std::vector<std::pair<QString, QSpinBox *>> fields = {
      {tr("Layout width"), m_width}, {tr("Layout height"), m_height}, {tr("X"), m_x}, {tr("Y"), m_y}
  };
  for (const auto &[label, spin] : fields) {
    auto *fieldLabel = new QLabel(label, this);
    fieldLabel->setBuddy(spin);
    spin->setAccessibleName(label);
    geometry->addWidget(fieldLabel);
    spin->setRange(spin == m_width || spin == m_height ? 64 : -32767, 32767);
    geometry->addWidget(spin);
    connect(spin, &QSpinBox::valueChanged, this, &MonitorLayoutWidget::resizeDisplay);
  }
  m_width->setObjectName("layoutWidth");
  m_height->setObjectName("layoutHeight");
  m_x->setObjectName("layoutX");
  m_y->setObjectName("layoutY");
  layout->addLayout(geometry);
  layout->addWidget(m_canvas, 1);
  m_status->setWordWrap(true);
  layout->addWidget(m_status);
  m_canvas->changed = [this] {
    refresh();
    Q_EMIT changed();
  };
  connect(m_enabled, &QCheckBox::toggled, this, [this] {
    refresh();
    Q_EMIT changed();
  });
  connect(m_display, &QComboBox::currentIndexChanged, this, &MonitorLayoutWidget::selectDisplay);
  connect(add, &QPushButton::clicked, this, &MonitorLayoutWidget::addRemote);
  connect(remove, &QPushButton::clicked, this, [this] {
    std::erase_if(m_canvas->items, [this](const auto &item) {
      return !item.local && item.computer == m_remote->currentText().toStdString();
    });
    rebuildDisplays();
    refresh();
    Q_EMIT changed();
  });
  connect(detect, &QPushButton::clicked, this, [this] { refreshDetectedDisplays(false); });
  connect(reset, &QPushButton::clicked, this, [this] { refreshDetectedDisplays(true); });
  reload();
#ifndef Q_OS_WIN
  setEnabled(false);
  hint->setText(tr("Monitor edge switching currently requires a Windows server."));
#endif
}

void MonitorLayoutWidget::setDisplays(std::vector<DisplayPlacement> displays)
{
  m_canvas->items = std::move(displays);
  rebuildDisplays();
  refresh();
  Q_EMIT changed();
}

const std::vector<DisplayPlacement> &MonitorLayoutWidget::displays() const
{
  return m_canvas->items;
}
void MonitorLayoutWidget::setComputerNames(const QStringList &computers)
{
  const auto current = m_remote->currentText();
  const QSignalBlocker blocked(m_remote);
  m_remote->clear();
  for (const auto &name : computers) {
    if (!name.isEmpty() && name != m_server && m_remote->findText(name) < 0)
      m_remote->addItem(name);
  }
  if (m_remote->findText(current) >= 0)
    m_remote->setCurrentText(current);
  rebuildDisplays();
}

bool MonitorLayoutWidget::isEnabledLayout() const
{
  return m_enabled->isChecked();
}
bool MonitorLayoutWidget::isModified() const
{
  return state() != m_original;
}

QByteArray MonitorLayoutWidget::state() const
{
  QJsonArray remotes;
  QJsonArray locals;
  QJsonArray overrides;
  for (const auto &item : m_canvas->items) {
    if (item.local) {
      const auto native = item.nativeGeometry();
      locals.append(
          QJsonObject{
              {"id", QString::fromStdString(item.id)},
              {"x", native.x},
              {"y", native.y},
              {"width", native.width},
              {"height", native.height}
          }
      );
      if (item.rect != native) {
        overrides.append(
            QJsonObject{
                {"id", QString::fromStdString(item.id)},
                {"x", item.rect.x},
                {"y", item.rect.y},
                {"width", item.rect.width},
                {"height", item.rect.height}
            }
        );
      }
    }
    if (!item.local)
      remotes.append(
          QJsonObject{
              {"computer", QString::fromStdString(item.computer)},
              {"x", item.rect.x},
              {"y", item.rect.y},
              {"width", item.rect.width},
              {"height", item.rect.height}
          }
      );
  }
  return QJsonDocument(
             QJsonObject{
                 {"enabled", isEnabledLayout()}, {"remotes", remotes}, {"locals", locals}, {"overrides", overrides}
             }
  ).toJson(QJsonDocument::Compact);
}

void MonitorLayoutWidget::reload()
{
  const auto json = QJsonDocument::fromJson(Settings::value(Settings::Server::MonitorLayout).toByteArray()).object();
  const QSignalBlocker blocker(m_enabled);
  m_enabled->setChecked(json.value("enabled").toBool());
  auto items = detectDisplays(m_server);
  for (auto &item : items) {
    for (const auto &value : json.value("overrides").toArray()) {
      const auto object = value.toObject();
      if (object.value("id").toString().toStdString() == item.id) {
        const MonitorRect rect{
            object.value("x").toInt(), object.value("y").toInt(), object.value("width").toInt(),
            object.value("height").toInt()
        };
        if (rect.width >= 64 && rect.height >= 64)
          item.rect = rect;
      }
    }
  }
  for (const auto &value : json.value("remotes").toArray()) {
    const auto object = value.toObject();
    const auto name = object.value("computer").toString();
    if (m_remote->findText(name) < 0)
      continue;
    items.push_back(
        {name.toStdString(),
         name.toStdString(),
         {object.value("x").toInt(), object.value("y").toInt(), object.value("width").toInt(),
          object.value("height").toInt()},
         false}
    );
  }
  m_canvas->items = std::move(items);
  rebuildDisplays();
  refresh();
  m_original = json.isEmpty() ? state() : QJsonDocument(json).toJson(QJsonDocument::Compact);
}

void MonitorLayoutWidget::addRemote()
{
  const auto name = m_remote->currentText();
  if (name.isEmpty())
    return;
  for (const auto &item : m_canvas->items)
    if (!item.local && item.computer == name.toStdString())
      return;
  const int width = Settings::value(Settings::Computer::LastWidth.arg(name)).toInt();
  const int height = Settings::value(Settings::Computer::LastHeight.arg(name)).toInt();
  auto local =
      std::find_if(m_canvas->items.begin(), m_canvas->items.end(), [](const auto &item) { return item.local; });
  if (m_canvas->selected >= 0 && m_canvas->items.at(m_canvas->selected).local)
    local = m_canvas->items.begin() + m_canvas->selected;
  const int anchorX = local == m_canvas->items.end() ? 0 : local->rect.x;
  const int anchorY = local == m_canvas->items.end() ? 0 : local->rect.y;
  const int w = width > 0 ? width : 1920;
  const int h = height > 0 ? height : 1080;
  m_canvas->items.push_back({name.toStdString(), name.toStdString(), {anchorX - w, anchorY, w, h}, false});
  rebuildDisplays();
  m_canvas->selected = static_cast<int>(m_canvas->items.size()) - 1;
  refresh();
  Q_EMIT changed();
}

void MonitorLayoutWidget::rebuildDisplays()
{
  const auto previous = m_display->currentData().toString();
  const QSignalBlocker blocked(m_display);
  m_display->clear();
  for (const auto &item : m_canvas->items) {
    const auto key = QString::fromStdString(item.computer) + "/" + QString::fromStdString(item.id);
    m_display->addItem(QString::fromStdString(item.computer) + " / " + QString::fromStdString(item.id), key);
  }
  const int index = m_display->findData(previous);
  selectDisplay(index >= 0 ? index : (m_display->count() > 0 ? 0 : -1));
}

void MonitorLayoutWidget::selectDisplay(int index)
{
  m_canvas->selected = index;
  refresh();
}

void MonitorLayoutWidget::refreshDetectedDisplays(bool resetLayout)
{
  auto items = detectDisplays(m_server);
  if (!resetLayout) {
    for (auto &item : items) {
      const auto previous = std::find_if(m_canvas->items.begin(), m_canvas->items.end(), [&](const auto &old) {
        return old.local && old.id == item.id;
      });
      if (previous != m_canvas->items.end() && previous->rect != previous->nativeGeometry())
        item.rect = previous->rect;
    }
  }
  for (const auto &item : m_canvas->items)
    if (!item.local)
      items.push_back(item);
  setDisplays(std::move(items));
}

void MonitorLayoutWidget::resizeDisplay()
{
  if (m_canvas->selected < 0)
    return;
  auto &item = m_canvas->items.at(m_canvas->selected);
  if (item.local && item.nativeRect.width == 0)
    item.nativeRect = item.rect;
  item.rect = {m_x->value(), m_y->value(), m_width->value(), m_height->value()};
  refresh();
  Q_EMIT changed();
}

QString MonitorLayoutWidget::validationError() const
{
  if (!isEnabledLayout())
    return {};
  for (const auto &item : m_canvas->items) {
    if (!item.local && m_remote->findText(QString::fromStdString(item.computer)) < 0)
      return tr("A remote desktop is no longer in the Computers tab. Remove it from the monitor layout.");
  }
  return QString::fromStdString(deskflow::planMonitorLayout(m_canvas->items).error);
}

void MonitorLayoutWidget::refresh()
{
  const bool selected = m_canvas->selected >= 0 && m_canvas->selected < static_cast<int>(m_canvas->items.size());
  for (auto *spin : {m_width, m_height, m_x, m_y})
    spin->setEnabled(selected);
  if (selected) {
    const auto &item = m_canvas->items.at(m_canvas->selected);
    if (!item.local) {
      const QSignalBlocker remoteBlocked(m_remote);
      m_remote->setCurrentText(QString::fromStdString(item.computer));
    }
    const QSignalBlocker blocker(m_display);
    m_display->setCurrentIndex(m_canvas->selected);
    const std::vector<std::pair<QSpinBox *, int>> fields = {
        {m_width, item.rect.width}, {m_height, item.rect.height}, {m_x, item.rect.x}, {m_y, item.rect.y}
    };
    for (const auto &[spin, value] : fields) {
      const QSignalBlocker blocked(spin);
      spin->setValue(value);
    }
  }
  const auto plan = deskflow::planMonitorLayout(m_canvas->items);
  QStringList routes;
  for (const auto &link : plan.links)
    routes.append(
        QString::fromStdString(link.monitor) + " " + sideName(link.side) + " ↔ " + QString::fromStdString(link.remote) +
        " " + sideName(oppositeDirection(link.side))
    );
  m_status->setText(plan.error.empty() ? routes.join("\n") : tr(plan.error.c_str()));
  m_canvas->update();
}

QString MonitorLayoutWidget::linksConfig() const
{
  const auto plan = deskflow::planMonitorLayout(m_canvas->items);
  if (!plan.error.empty())
    return {};
  QString result = "section: links\n";
  QStringList names{m_server};
  for (const auto &item : m_canvas->items)
    if (!item.local)
      names.append(QString::fromStdString(item.computer));
  for (const auto &name : names) {
    result += "\t" + name + ":\n";
    for (const auto &link : plan.links) {
      const bool local = name == m_server;
      if (!local && name.toStdString() != link.remote)
        continue;
      const auto side = local ? link.side : oppositeDirection(link.side);
      const auto start = local ? link.localStart : link.remoteStart;
      const auto end = local ? link.localEnd : link.remoteEnd;
      const auto targetStart = local ? link.remoteStart : link.localStart;
      const auto targetEnd = local ? link.remoteEnd : link.localEnd;
      result += QStringLiteral("\t\t%1(%2,%3) = %4(%5,%6)\n")
                    .arg(sideName(side))
                    .arg(start * 100, 0, 'f', 8)
                    .arg(end * 100, 0, 'f', 8)
                    .arg(local ? QString::fromStdString(link.remote) : m_server)
                    .arg(targetStart * 100, 0, 'f', 8)
                    .arg(targetEnd * 100, 0, 'f', 8);
    }
  }
  return result + "end\n\n";
}

void MonitorLayoutWidget::commit()
{
  Settings::setValue(Settings::Server::MonitorLayout, state());
  const bool wasEnabled = QJsonDocument::fromJson(m_original).object().value("enabled").toBool();
  if (isEnabledLayout() || wasEnabled) {
    for (const auto side : {Direction::Left, Direction::Right, Direction::Top, Direction::Bottom})
      Settings::setValue(selectorKey(side), QString());
    const auto plan = deskflow::planMonitorLayout(m_canvas->items);
    if (isEnabledLayout())
      for (const auto &link : plan.links)
        Settings::setValue(selectorKey(link.side), QString::fromStdString(link.monitor));
  }
  m_original = state();
}
