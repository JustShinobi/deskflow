/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MonitorLayoutWidgetTests.h"
#include "common/Settings.h"
#include "gui/config/ServerConfig.h"
#include "gui/dialogs/ServerConfigDialog.h"
#include "gui/widgets/MonitorLayoutWidget.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabWidget>
#include <algorithm>

using deskflow::DisplayPlacement;
using deskflow::gui::MonitorLayoutWidget;

namespace {
std::vector<DisplayPlacement> setup()
{
  return {
      {"server", "lower", {0, 0, 5120, 1440}, true},
      {"server", "upper-left", {-2983, -2164, 3072, 1728}, true},
      {"server", "upper-right", {857, -1440, 3440, 1440}, true},
      {"laptop", "laptop", {-2560, -436, 2560, 1440}, false}
  };
}

std::vector<DisplayPlacement> detected(const QString &server)
{
  auto displays = setup();
  displays.pop_back();
  for (auto &display : displays) {
    display.computer = server.toStdString();
    display.nativeRect = display.rect;
  }
  return displays;
}

void enableLayout(MonitorLayoutWidget &widget)
{
  widget.setEnabled(true);
  widget.setDisplays(setup());
  widget.findChild<QCheckBox *>()->setChecked(true);
}

QString outputPath()
{
  return QFileInfo(Settings::settingsFile()).dir().filePath("monitor-layout.conf");
}

void save(ServerConfigDialog &dialog)
{
  dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
}

void drag(MonitorLayoutWidget &widget, size_t displayIndex)
{
  widget.setEnabled(true);
  widget.resize(900, 680);
  widget.show();
  QTest::qWait(50);
  auto *canvas = widget.findChild<QWidget *>("monitorCanvas");
  const auto &r = widget.displays().at(displayIndex).rect;
  const double scale = std::min((canvas->width() - 40) / 8103.0, (canvas->height() - 40) / 3604.0);
  const double left = (canvas->width() - 8103 * scale) / 2;
  const double top = (canvas->height() - 3604 * scale) / 2;
  const QPoint start(
      qRound(left + (2983 + r.x + r.width / 2.0) * scale), qRound(top + (2164 + r.y + r.height / 2.0) * scale)
  );
  QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, start);
  QTest::mouseMove(canvas, start + QPoint(-50, 30));
  QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, start + QPoint(-50, 30));
}
} // namespace

void MonitorLayoutWidgetTests::initTestCase()
{
#ifdef Q_OS_WIN
  const auto font =
      QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("WINDIR")).filePath("Fonts/segoeui.ttf"));
  if (font >= 0 && !QFontDatabase::applicationFontFamilies(font).isEmpty())
    QGuiApplication::setFont(QFont(QFontDatabase::applicationFontFamilies(font).first(), 10));
#endif
  QVERIFY(m_temp.isValid());
}

void MonitorLayoutWidgetTests::init()
{
  const auto path = m_temp.filePath(QString::fromLatin1(QTest::currentTestFunction()));
  QVERIFY(QDir().mkpath(path));
  Settings::setSettingsFile(path + "/Deskflow.conf");
  Settings::setStateFile(path + "/Deskflow.state");
  Settings::setValue(Settings::Core::ComputerName, "server");
}

void MonitorLayoutWidgetTests::linksConfig_touchingDisplays_generatesBidirectionalRanges()
{
  MonitorLayoutWidget widget("server", {"laptop"}, nullptr, detected);
  enableLayout(widget);

  const auto text = widget.linksConfig();

  QVERIFY(text.contains("left(0.00000000,69.72222222) = laptop(30.27777778,100.00000000)"));
  QVERIFY(text.contains("up(0.00000000,100.00000000) = server(13.76953125,97.10286458)"));
}

void MonitorLayoutWidgetTests::commit_enabledLayout_storesSelectors()
{
  MonitorLayoutWidget widget("server", {"laptop"}, nullptr, detected);
  enableLayout(widget);

  widget.commit();

  QCOMPARE(Settings::value(Settings::Server::SwitchMonitorLeft).toString(), QString("lower"));
  QCOMPARE(Settings::value(Settings::Server::SwitchMonitorBottom).toString(), QString("upper-left"));
  QVERIFY(!widget.isModified());
}

void MonitorLayoutWidgetTests::commit_disabledLayout_clearsSelectors()
{
  MonitorLayoutWidget widget("server", {"laptop"}, nullptr, detected);
  enableLayout(widget);
  widget.commit();
  widget.findChild<QCheckBox *>()->setChecked(false);

  widget.commit();

  QVERIFY(Settings::value(Settings::Server::SwitchMonitorLeft).toString().isEmpty());
  QVERIFY(Settings::value(Settings::Server::SwitchMonitorBottom).toString().isEmpty());
}

void MonitorLayoutWidgetTests::setDisplays_changedGeometry_marksModified()
{
  MonitorLayoutWidget widget("server", {"laptop"}, nullptr, detected);
  auto displays = widget.displays();
  displays.front().rect.width += 1;

  widget.setDisplays(displays);

  QVERIFY(widget.isModified());
}

void MonitorLayoutWidgetTests::drag_remoteDesktop_preservesLocalGeometry()
{
  MonitorLayoutWidget widget("server", {"laptop"}, nullptr, detected);
  widget.setDisplays(setup());
  const auto local = widget.displays().front().rect;
  const auto remote = widget.displays().back().rect;

  drag(widget, 3);

  QVERIFY(widget.displays().back().rect != remote);
  QCOMPARE(widget.displays().front().rect, local);
}

void MonitorLayoutWidgetTests::drag_localDisplay_preservesNativeGeometry()
{
  MonitorLayoutWidget widget("server", {"laptop"}, nullptr, detected);
  widget.setDisplays(setup());
  const auto native = widget.displays().front().nativeGeometry();

  drag(widget, 0);

  QVERIFY(widget.displays().front().rect != native);
  QCOMPARE(widget.displays().front().nativeGeometry(), native);
}

void MonitorLayoutWidgetTests::cancel_editedLayout_leavesSettingsUnchanged()
{
  ServerConfig config;
  config.addClient("laptop");
  const auto before = Settings::value(Settings::Server::MonitorLayout);
  ServerConfigDialog dialog(nullptr, config, detected);
  enableLayout(*dialog.findChild<MonitorLayoutWidget *>());

  dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel)->click();

  QCOMPARE(dialog.result(), int(QDialog::Rejected));
  QCOMPARE(Settings::value(Settings::Server::MonitorLayout), before);
  QVERIFY(!QFile::exists(outputPath()));
}

void MonitorLayoutWidgetTests::save_validLayout_writesExternalLinks()
{
  ServerConfig config;
  config.addClient("laptop");
  ServerConfigDialog dialog(nullptr, config, detected);
  auto *editor = dialog.findChild<MonitorLayoutWidget *>();
  QVERIFY(editor);
#ifdef Q_OS_WIN
  QVERIFY(editor->isEnabled());
#endif
  enableLayout(*editor);
  dialog.findChild<QTabWidget *>()->setCurrentWidget(editor);
  dialog.resize(1000, 800);
  dialog.show();
  QTest::qWait(50);
  const auto screenshot = qEnvironmentVariable("MONITOR_LAYOUT_SCREENSHOT");
  if (!screenshot.isEmpty())
    QVERIFY(dialog.grab().save(screenshot));

  save(dialog);

  QCOMPARE(dialog.result(), int(QDialog::Accepted));
  QFile output(outputPath());
  QVERIFY(output.open(QIODevice::ReadOnly));
  QVERIFY(output.readAll().contains("down(13.76953125,97.10286458) = laptop(0.00000000,100.00000000)"));
  QVERIFY(Settings::value(Settings::Server::ExternalConfig).toBool());
  QCOMPARE(Settings::value(Settings::Server::ExternalConfigFile).toString(), outputPath());
}

void MonitorLayoutWidgetTests::save_externalConfig_preservesOtherSections()
{
  const QByteArray retained = "# Keep custom settings\nsection: screens\n    server:\n    laptop:\nend\n"
                              "section: aliases\n    laptop:\n        portable\nend\n"
                              "section: options\n    keystroke(Control+Alt+z) = switchToNextScreen\nend\n";
  QFile output(outputPath());
  QVERIFY(output.open(QIODevice::WriteOnly));
  output.write("section: links\n    server:\n        left = laptop\nend\n" + retained);
  output.close();
  Settings::setValue(Settings::Server::ExternalConfigFile, outputPath());
  Settings::setValue(Settings::Server::ExternalConfig, true);
  ServerConfig config;
  config.addClient("laptop");
  ServerConfigDialog dialog(nullptr, config, detected);
  enableLayout(*dialog.findChild<MonitorLayoutWidget *>());

  save(dialog);

  QCOMPARE(dialog.result(), int(QDialog::Accepted));
  QVERIFY(output.open(QIODevice::ReadOnly));
  const auto text = output.readAll();
  QVERIFY(text.contains(retained));
  QVERIFY(!text.contains("left = laptop"));
  QCOMPARE(text.count("section: links"), 1);
}

void MonitorLayoutWidgetTests::reload_manualLocalGeometry_restoresOverride()
{
  MonitorLayoutWidget widget("server", {"laptop"}, nullptr, detected);
  widget.setEnabled(true);
  const auto native = widget.displays().front().nativeGeometry();
  widget.findChild<QSpinBox *>("layoutX")->setValue(native.x + 123);
  widget.findChild<QSpinBox *>("layoutWidth")->setValue(native.width / 2);
  const auto saved = widget.displays().front().rect;
  widget.commit();

  MonitorLayoutWidget reopened("server", {"laptop"}, nullptr, detected);

  QCOMPARE(reopened.displays().front().rect, saved);
  QCOMPARE(reopened.displays().front().nativeGeometry(), native);
  QVERIFY(!reopened.isModified());
}

void MonitorLayoutWidgetTests::detectDisplays_manualOverride_preservesPlacement()
{
  MonitorLayoutWidget widget("server", {"laptop"}, nullptr, detected);
  widget.setEnabled(true);
  widget.findChild<QSpinBox *>("layoutX")->setValue(123);
  const auto saved = widget.displays().front().rect;

  widget.findChild<QPushButton *>("detectDisplays")->click();

  QCOMPARE(widget.displays().front().rect, saved);
}

void MonitorLayoutWidgetTests::useDetectedLayout_manualOverride_restoresNativePlacement()
{
  MonitorLayoutWidget widget("server", {"laptop"}, nullptr, detected);
  widget.setEnabled(true);
  const auto native = widget.displays().front().nativeGeometry();
  widget.findChild<QSpinBox *>("layoutX")->setValue(123);

  widget.findChild<QPushButton *>("resetDetectedLayout")->click();

  QCOMPARE(widget.displays().front().rect, native);
}

void MonitorLayoutWidgetTests::detectDisplays_changedHardware_updatesNativeGeometry()
{
  auto available = detected("server");
  MonitorLayoutWidget widget("server", {"laptop"}, nullptr, [&available](const QString &) { return available; });
  widget.setEnabled(true);
  widget.findChild<QSpinBox *>("layoutX")->setValue(123);
  const auto saved = widget.displays().front().rect;
  available.front().rect.width = 2560;
  available.front().nativeRect = available.front().rect;
  available.erase(available.begin() + 1);

  widget.findChild<QPushButton *>("detectDisplays")->click();

  QCOMPARE(widget.displays().size(), size_t(2));
  QCOMPARE(widget.displays().front().nativeGeometry().width, 2560);
  QCOMPARE(widget.displays().front().rect, saved);
}

void MonitorLayoutWidgetTests::reopen_enabledLayout_keepsComputersEditable()
{
  MonitorLayoutWidget widget("server", {"laptop"}, nullptr, detected);
  enableLayout(widget);
  widget.commit();
  ServerConfig config;
  config.addClient("laptop");
  Settings::setValue(Settings::Server::ExternalConfig, true);

  ServerConfigDialog dialog(nullptr, config, detected);

  QVERIFY(dialog.findChild<QTabWidget *>()->isTabEnabled(0));
}

QTEST_MAIN(MonitorLayoutWidgetTests)
