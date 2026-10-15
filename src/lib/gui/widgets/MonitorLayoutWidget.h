/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "base/MonitorLayout.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QSpinBox;
class MonitorLayoutCanvas;

class MonitorLayoutWidget : public QWidget
{
  Q_OBJECT
public:
  MonitorLayoutWidget(const QString &server, const QStringList &computers, QWidget *parent = nullptr);
  bool isModified() const;
  bool isEnabledLayout() const;
  void reload();
  void setComputerNames(const QStringList &computers);
  QString validationError() const;
  QString linksConfig() const;
  void commit();
  void setDisplays(std::vector<deskflow::DisplayPlacement> displays);
  const std::vector<deskflow::DisplayPlacement> &displays() const;

Q_SIGNALS:
  void changed();

private:
  QByteArray state() const;
  void refresh();
  void selectRemote();
  void resizeRemote();
  void addRemote();
  QString m_server;
  QByteArray m_original;
  MonitorLayoutCanvas *m_canvas;
  QCheckBox *m_enabled;
  QComboBox *m_remote;
  QSpinBox *m_width;
  QSpinBox *m_height;
  QSpinBox *m_x;
  QSpinBox *m_y;
  QLabel *m_status;
};
