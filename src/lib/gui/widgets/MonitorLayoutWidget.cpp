/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MonitorLayoutWidget.h"
#include "MonitorLayoutCanvas.h"

#include "common/Settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace deskflow::gui {

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
        auto *pItems = reinterpret_cast<std::vector<DisplayPlacement> *>(data);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO *>(&info))) {
          const auto &r = info.rcMonitor;
          pItems->push_back(
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

} // namespace

MonitorLayoutWidget::MonitorLayoutWidget(
    const QString &server, const QStringList &computers, QWidget *parent, DisplayDetector detector
)
    : QWidget(parent),
      m_server(server),
      m_detectDisplays(detector ? std::move(detector) : detectDisplays),
      m_widgetCanvas(new MonitorLayoutCanvas(this)),
      m_cbEnabled(new QCheckBox(tr("Use monitor layout"), this)),
      m_comboRemote(new QComboBox(this)),
      m_comboDisplay(new QComboBox(this)),
      m_spinBoxWidth(new QSpinBox(this)),
      m_spinBoxHeight(new QSpinBox(this)),
      m_spinBoxX(new QSpinBox(this)),
      m_spinBoxY(new QSpinBox(this)),
      m_lblStatus(new QLabel(this))
{
  auto *layout = new QVBoxLayout(this);
  layout->addWidget(m_cbEnabled);
  auto *lblHint = new QLabel(
      tr("Displays are detected automatically. Select or drag any display and edit its layout size and position. "
         "Detected Windows geometry remains the reference for cursor mapping. Green edges switch in both directions. "
         "Local-to-local movement still follows Windows. One rectangle represents each remote desktop."),
      this
  );
  lblHint->setWordWrap(true);
  layout->addWidget(lblHint);
  auto *controls = new QHBoxLayout;
  for (const auto &computer : computers) {
    if (!computer.isEmpty() && computer != server && m_comboRemote->findText(computer) < 0)
      m_comboRemote->addItem(computer);
  }
  m_comboRemote->setAccessibleName(tr("Remote computer"));
  controls->addWidget(m_comboRemote);
  auto *btnAdd = new QPushButton(tr("Add remote"), this);
  auto *btnRemove = new QPushButton(tr("Remove remote"), this);
  auto *btnDetect = new QPushButton(tr("Detect displays"), this);
  btnDetect->setObjectName("detectDisplays");
  auto *btnReset = new QPushButton(tr("Use detected layout"), this);
  btnReset->setObjectName("resetDetectedLayout");
  controls->addWidget(btnAdd);
  controls->addWidget(btnRemove);
  controls->addWidget(btnDetect);
  controls->addWidget(btnReset);
  controls->addStretch();
  layout->addLayout(controls);
  m_comboDisplay->setObjectName("displaySelection");
  layout->addWidget(m_comboDisplay);
  auto *geometry = new QHBoxLayout;
  const std::vector<std::pair<QString, QSpinBox *>> fields = {
      {tr("Layout width"), m_spinBoxWidth},
      {tr("Layout height"), m_spinBoxHeight},
      {tr("X"), m_spinBoxX},
      {tr("Y"), m_spinBoxY}
  };
  for (const auto &[label, spinBox] : fields) {
    auto *lblField = new QLabel(label, this);
    lblField->setBuddy(spinBox);
    spinBox->setAccessibleName(label);
    geometry->addWidget(lblField);
    spinBox->setRange(spinBox == m_spinBoxWidth || spinBox == m_spinBoxHeight ? 64 : -32767, 32767);
    geometry->addWidget(spinBox);
    connect(spinBox, &QSpinBox::valueChanged, this, &MonitorLayoutWidget::resizeDisplay);
  }
  m_spinBoxWidth->setObjectName("layoutWidth");
  m_spinBoxHeight->setObjectName("layoutHeight");
  m_spinBoxX->setObjectName("layoutX");
  m_spinBoxY->setObjectName("layoutY");
  layout->addLayout(geometry);
  layout->addWidget(m_widgetCanvas, 1);
  m_lblStatus->setWordWrap(true);
  layout->addWidget(m_lblStatus);
  m_widgetCanvas->m_changed = [this] {
    refresh();
    Q_EMIT changed();
  };
  connect(m_cbEnabled, &QCheckBox::toggled, this, [this] {
    refresh();
    Q_EMIT changed();
  });
  connect(m_comboDisplay, &QComboBox::currentIndexChanged, this, &MonitorLayoutWidget::selectDisplay);
  connect(btnAdd, &QPushButton::clicked, this, &MonitorLayoutWidget::addRemote);
  connect(btnRemove, &QPushButton::clicked, this, [this] {
    std::erase_if(m_widgetCanvas->m_items, [this](const auto &item) {
      return !item.local && item.computer == m_comboRemote->currentText().toStdString();
    });
    rebuildDisplays();
    refresh();
    Q_EMIT changed();
  });
  connect(btnDetect, &QPushButton::clicked, this, [this] { refreshDetectedDisplays(false); });
  connect(btnReset, &QPushButton::clicked, this, [this] { refreshDetectedDisplays(true); });
  reload();
#ifndef Q_OS_WIN
  setEnabled(false);
  lblHint->setText(tr("Monitor edge switching currently requires a Windows server."));
#endif
}

void MonitorLayoutWidget::setDisplays(std::vector<DisplayPlacement> displays)
{
  m_widgetCanvas->m_items = std::move(displays);
  rebuildDisplays();
  refresh();
  Q_EMIT changed();
}

const std::vector<DisplayPlacement> &MonitorLayoutWidget::displays() const
{
  return m_widgetCanvas->m_items;
}
void MonitorLayoutWidget::setComputerNames(const QStringList &computers)
{
  const auto current = m_comboRemote->currentText();
  const QSignalBlocker blocked(m_comboRemote);
  m_comboRemote->clear();
  for (const auto &name : computers) {
    if (!name.isEmpty() && name != m_server && m_comboRemote->findText(name) < 0)
      m_comboRemote->addItem(name);
  }
  if (m_comboRemote->findText(current) >= 0)
    m_comboRemote->setCurrentText(current);
  rebuildDisplays();
}

bool MonitorLayoutWidget::isEnabledLayout() const
{
  return m_cbEnabled->isChecked();
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
  for (const auto &item : m_widgetCanvas->m_items) {
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
  const QSignalBlocker blocker(m_cbEnabled);
  m_cbEnabled->setChecked(json.value("enabled").toBool());
  auto items = m_detectDisplays(m_server);
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
    if (m_comboRemote->findText(name) < 0)
      continue;
    items.push_back(
        {name.toStdString(),
         name.toStdString(),
         {object.value("x").toInt(), object.value("y").toInt(), object.value("width").toInt(),
          object.value("height").toInt()},
         false}
    );
  }
  m_widgetCanvas->m_items = std::move(items);
  rebuildDisplays();
  refresh();
  m_original = json.isEmpty() ? state() : QJsonDocument(json).toJson(QJsonDocument::Compact);
}

void MonitorLayoutWidget::addRemote()
{
  const auto name = m_comboRemote->currentText();
  if (name.isEmpty())
    return;
  for (const auto &item : m_widgetCanvas->m_items)
    if (!item.local && item.computer == name.toStdString())
      return;
  const int width = Settings::value(Settings::Computer::LastWidth.arg(name)).toInt();
  const int height = Settings::value(Settings::Computer::LastHeight.arg(name)).toInt();
  auto local = std::find_if(m_widgetCanvas->m_items.begin(), m_widgetCanvas->m_items.end(), [](const auto &item) {
    return item.local;
  });
  if (m_widgetCanvas->m_selected >= 0 && m_widgetCanvas->m_items.at(m_widgetCanvas->m_selected).local)
    local = m_widgetCanvas->m_items.begin() + m_widgetCanvas->m_selected;
  const int anchorX = local == m_widgetCanvas->m_items.end() ? 0 : local->rect.x;
  const int anchorY = local == m_widgetCanvas->m_items.end() ? 0 : local->rect.y;
  const int w = width > 0 ? width : 1920;
  const int h = height > 0 ? height : 1080;
  m_widgetCanvas->m_items.push_back({name.toStdString(), name.toStdString(), {anchorX - w, anchorY, w, h}, false});
  rebuildDisplays();
  m_widgetCanvas->m_selected = static_cast<int>(m_widgetCanvas->m_items.size()) - 1;
  refresh();
  Q_EMIT changed();
}

void MonitorLayoutWidget::rebuildDisplays()
{
  const auto previous = m_comboDisplay->currentData().toString();
  const QSignalBlocker blocked(m_comboDisplay);
  m_comboDisplay->clear();
  for (const auto &item : m_widgetCanvas->m_items) {
    const auto key = QString::fromStdString(item.computer) + "/" + QString::fromStdString(item.id);
    m_comboDisplay->addItem(QString::fromStdString(item.computer) + " / " + QString::fromStdString(item.id), key);
  }
  const int index = m_comboDisplay->findData(previous);
  selectDisplay(index >= 0 ? index : (m_comboDisplay->count() > 0 ? 0 : -1));
}

void MonitorLayoutWidget::selectDisplay(int index)
{
  m_widgetCanvas->m_selected = index;
  refresh();
}

void MonitorLayoutWidget::refreshDetectedDisplays(bool resetLayout)
{
  auto items = m_detectDisplays(m_server);
  if (!resetLayout) {
    for (auto &item : items) {
      const auto previous =
          std::find_if(m_widgetCanvas->m_items.begin(), m_widgetCanvas->m_items.end(), [&](const auto &old) {
            return old.local && old.id == item.id;
          });
      if (previous != m_widgetCanvas->m_items.end() && previous->rect != previous->nativeGeometry())
        item.rect = previous->rect;
    }
  }
  for (const auto &item : m_widgetCanvas->m_items)
    if (!item.local)
      items.push_back(item);
  setDisplays(std::move(items));
}

void MonitorLayoutWidget::resizeDisplay()
{
  if (m_widgetCanvas->m_selected < 0)
    return;
  auto &item = m_widgetCanvas->m_items.at(m_widgetCanvas->m_selected);
  if (item.local && item.nativeRect.width == 0)
    item.nativeRect = item.rect;
  item.rect = {m_spinBoxX->value(), m_spinBoxY->value(), m_spinBoxWidth->value(), m_spinBoxHeight->value()};
  refresh();
  Q_EMIT changed();
}

QString MonitorLayoutWidget::validationError() const
{
  if (!isEnabledLayout())
    return {};
  for (const auto &item : m_widgetCanvas->m_items) {
    if (!item.local && m_comboRemote->findText(QString::fromStdString(item.computer)) < 0)
      return tr("A remote desktop is no longer in the Computers tab. Remove it from the monitor layout.");
  }
  return QString::fromStdString(deskflow::planMonitorLayout(m_widgetCanvas->m_items).error);
}

void MonitorLayoutWidget::refresh()
{
  const bool selected =
      m_widgetCanvas->m_selected >= 0 && m_widgetCanvas->m_selected < static_cast<int>(m_widgetCanvas->m_items.size());
  for (auto *spinBox : {m_spinBoxWidth, m_spinBoxHeight, m_spinBoxX, m_spinBoxY})
    spinBox->setEnabled(selected);
  if (selected) {
    const auto &item = m_widgetCanvas->m_items.at(m_widgetCanvas->m_selected);
    if (!item.local) {
      const QSignalBlocker remoteBlocked(m_comboRemote);
      m_comboRemote->setCurrentText(QString::fromStdString(item.computer));
    }
    const QSignalBlocker blocker(m_comboDisplay);
    m_comboDisplay->setCurrentIndex(m_widgetCanvas->m_selected);
    const std::vector<std::pair<QSpinBox *, int>> fields = {
        {m_spinBoxWidth, item.rect.width},
        {m_spinBoxHeight, item.rect.height},
        {m_spinBoxX, item.rect.x},
        {m_spinBoxY, item.rect.y}
    };
    for (const auto &[spinBox, value] : fields) {
      const QSignalBlocker blocked(spinBox);
      spinBox->setValue(value);
    }
  }
  const auto plan = deskflow::planMonitorLayout(m_widgetCanvas->m_items);
  QStringList routes;
  for (const auto &link : plan.links)
    routes.append(
        QString::fromStdString(link.monitor) + " " + sideName(link.side) + " <-> " +
        QString::fromStdString(link.remote) + " " + sideName(oppositeDirection(link.side))
    );
  m_lblStatus->setText(plan.error.empty() ? routes.join("\n") : tr(plan.error.c_str()));
  m_widgetCanvas->update();
}

QString MonitorLayoutWidget::linksConfig() const
{
  const auto plan = deskflow::planMonitorLayout(m_widgetCanvas->m_items);
  if (!plan.error.empty())
    return {};
  QString result = "section: links\n";
  QStringList names{m_server};
  for (const auto &item : m_widgetCanvas->m_items)
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
    const auto plan = deskflow::planMonitorLayout(m_widgetCanvas->m_items);
    if (isEnabledLayout())
      for (const auto &link : plan.links)
        Settings::setValue(selectorKey(link.side), QString::fromStdString(link.monitor));
  }
  m_original = state();
}

} // namespace deskflow::gui
