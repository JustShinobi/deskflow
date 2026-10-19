/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "base/MonitorLayout.h"

#include <QWidget>
#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QSpinBox;
namespace deskflow::gui {

class MonitorLayoutCanvas;

//! Edits display contacts without changing the operating system display arrangement.
class MonitorLayoutWidget : public QWidget
{
  Q_OBJECT
public:
  //! Supplies active local displays in native desktop coordinates.
  using DisplayDetector = std::function<std::vector<DisplayPlacement>(const QString &server)>;

  //! Detect displays and load saved placements; an empty detector uses the platform backend.
  MonitorLayoutWidget(
      const QString &server, const QStringList &computers, QWidget *parent = nullptr, DisplayDetector detector = {}
  );
  //! Whether the editable state differs from the last reload or commit.
  bool isModified() const;
  //! Whether monitor contacts replace the computer grid links.
  bool isEnabledLayout() const;
  //! Return an empty string for a valid layout, or a user-facing error.
  QString validationError() const;
  //! Generate the bidirectional links section for a validated layout.
  QString linksConfig() const;
  //! Current local and remote placements, including native local geometry.
  const std::vector<DisplayPlacement> &displays() const;

  //! Update the list of computers available for remote placement.
  void setComputerNames(const QStringList &computers);
  //! Replace editable placements without writing settings.
  void setDisplays(std::vector<DisplayPlacement> displays);

  //! Reload settings and detect currently available local displays.
  void reload();
  //! Store layout and edge selectors in Settings after successful validation.
  void commit();

Q_SIGNALS:
  void changed();

private:
  QByteArray state() const;
  void refresh();
  void rebuildDisplays();
  void selectDisplay(int index);
  void refreshDetectedDisplays(bool resetLayout);
  void resizeDisplay();
  void addRemote();
  QString m_server;
  DisplayDetector m_detectDisplays;
  QByteArray m_original;
  MonitorLayoutCanvas *m_widgetCanvas;
  QCheckBox *m_cbEnabled;
  QComboBox *m_comboRemote;
  QComboBox *m_comboDisplay;
  QSpinBox *m_spinBoxWidth;
  QSpinBox *m_spinBoxHeight;
  QSpinBox *m_spinBoxX;
  QSpinBox *m_spinBoxY;
  QLabel *m_lblStatus;
};

} // namespace deskflow::gui
