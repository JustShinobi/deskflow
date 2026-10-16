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

namespace {
std::vector<deskflow::DisplayPlacement> setup()
{
  return {
      {"server", "lower", {0, 0, 5120, 1440}, true},
      {"server", "upper-left", {-2983, -2164, 3072, 1728}, true},
      {"server", "upper-right", {857, -1440, 3440, 1440}, true},
      {"laptop", "laptop", {-2560, -436, 2560, 1440}, false}
  };
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
  Settings::setSettingsFile(m_temp.filePath("Deskflow.conf"));
  Settings::setStateFile(m_temp.filePath("Deskflow.state"));
}
void MonitorLayoutWidgetTests::generatedLinksAndPersistence()
{
  Settings::setValue(Settings::Server::MonitorLayout, QVariant());
  const auto original = Settings::value(Settings::Server::MonitorLayout);
  {
    MonitorLayoutWidget widget("server", {"server", "laptop"});
    widget.setDisplays(setup());
    widget.findChild<QCheckBox *>()->setChecked(true);
    QVERIFY(widget.isModified());
    QVERIFY(widget.validationError().isEmpty());
    const auto text = widget.linksConfig();
    QVERIFY(text.contains("left(0.00000000,69.72222222) = laptop(30.27777778,100.00000000)"));
    QVERIFY(text.contains("up(0.00000000,100.00000000) = server(13.76953125,97.10286458)"));
  }
  QCOMPARE(Settings::value(Settings::Server::MonitorLayout), original);
  MonitorLayoutWidget widget("server", {"laptop"});
  widget.setDisplays(setup());
  widget.findChild<QCheckBox *>()->setChecked(true);
  widget.commit();
  QCOMPARE(Settings::value(Settings::Server::SwitchMonitorLeft).toString(), QString("lower"));
  QCOMPARE(Settings::value(Settings::Server::SwitchMonitorBottom).toString(), QString("upper-left"));
  const auto json = QJsonDocument::fromJson(Settings::value(Settings::Server::MonitorLayout).toByteArray()).object();
  QVERIFY(json.value("enabled").toBool());
  QVERIFY(!widget.isModified());
  auto changedGeometry = setup();
  changedGeometry.front().rect.width += 1;
  widget.setDisplays(changedGeometry);
  QVERIFY(widget.isModified());
  widget.findChild<QCheckBox *>()->setChecked(false);
  widget.commit();
  QVERIFY(Settings::value(Settings::Server::SwitchMonitorLeft).toString().isEmpty());
}
void MonitorLayoutWidgetTests::canvasDraggingAndRendering()
{
  MonitorLayoutWidget widget("server", {"laptop"});
  widget.setEnabled(true);
  widget.setDisplays(setup());
  widget.resize(900, 680);
  widget.show();
  QTest::qWait(100);
  auto *canvas = widget.findChild<QWidget *>("monitorCanvas");
  QVERIFY(canvas);
  // The canvas fits a desktop bounding box of 8103 x 3604 with a 20px margin.
  const double scale = std::min((canvas->width() - 40) / 8103.0, (canvas->height() - 40) / 3604.0);
  const double left = (canvas->width() - 8103 * scale) / 2;
  const double top = (canvas->height() - 3604 * scale) / 2;
  const QPoint start(qRound(left + (2983 - 1280) * scale), qRound(top + (2164 + 284) * scale));
  const auto local = widget.displays()[0].rect;
  const auto before = widget.displays().back().rect;
  QSignalSpy spy(&widget, &MonitorLayoutWidget::changed);
  QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, start);
  QTest::mouseMove(canvas, start + QPoint(-50, 30));
  QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, start + QPoint(-50, 30));
  QVERIFY(spy.count() > 1);
  QCOMPARE(widget.displays()[0].rect, local);
  QVERIFY(widget.displays().back().rect != before);
  widget.setDisplays(setup());
  QTest::qWait(50);
  const QPoint localStart(qRound(left + (2983 + 2560) * scale), qRound(top + (2164 + 720) * scale));
  QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, localStart);
  QTest::mouseMove(canvas, localStart + QPoint(50, 30));
  QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, localStart + QPoint(50, 30));
  QVERIFY(widget.displays()[0].rect != local);
  QCOMPARE(widget.displays()[0].nativeGeometry(), local);
  widget.setDisplays(setup());
  QTest::qWait(50);
  const auto output = qEnvironmentVariable("MONITOR_LAYOUT_SCREENSHOT");
  if (!output.isEmpty())
    QVERIFY(widget.grab().save(output));
}
void MonitorLayoutWidgetTests::dialogSaveAndCancel()
{
  Settings::setValue(Settings::Core::ComputerName, "server");
  Settings::setValue(Settings::Server::MonitorLayout, QVariant());
  Settings::setValue(Settings::Server::ExternalConfig, false);
  ServerConfig config;
  config.addClient("laptop");
  const auto layoutBefore = Settings::value(Settings::Server::MonitorLayout);
  const auto outputPath = m_temp.filePath("monitor-layout.conf");
  {
    ServerConfigDialog dialog(nullptr, config);
    auto *editor = dialog.findChild<MonitorLayoutWidget *>();
    QVERIFY(editor);
#ifdef Q_OS_WIN
    QVERIFY(editor->isEnabled());
#else
    editor->setEnabled(true);
#endif
    editor->setDisplays(setup());
    editor->findChild<QCheckBox *>()->setChecked(true);
    dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel)->click();
    QCOMPARE(dialog.result(), int(QDialog::Rejected));
  }
  QCOMPARE(Settings::value(Settings::Server::MonitorLayout), layoutBefore);
  QVERIFY(!QFile::exists(outputPath));
  ServerConfigDialog dialog(nullptr, config);
  auto *editor = dialog.findChild<MonitorLayoutWidget *>();
  QVERIFY(editor);
#ifdef Q_OS_WIN
  QVERIFY(editor->isEnabled());
#else
  editor->setEnabled(true);
#endif
  editor->setDisplays(setup());
  editor->findChild<QCheckBox *>()->setChecked(true);
  dialog.findChild<QTabWidget *>()->setCurrentWidget(editor);
  dialog.resize(1000, 800);
  dialog.show();
  QTest::qWait(100);
  const auto screenshot = qEnvironmentVariable("MONITOR_LAYOUT_SCREENSHOT");
  if (!screenshot.isEmpty())
    QVERIFY(dialog.grab().save(screenshot));
  auto *save = dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save);
  QVERIFY(save->isEnabled());
  save->click();
  QCOMPARE(dialog.result(), int(QDialog::Accepted));
  QFile output(outputPath);
  QVERIFY(output.open(QIODevice::ReadOnly));
  const auto text = QString::fromUtf8(output.readAll());
  QVERIFY(text.contains("section: links"));
  QVERIFY(text.contains("down(13.76953125,97.10286458) = laptop(0.00000000,100.00000000)"));
  QVERIFY(Settings::value(Settings::Server::ExternalConfig).toBool());
  QCOMPARE(Settings::value(Settings::Server::ExternalConfigFile).toString(), outputPath);
  output.close();
  QVERIFY(output.open(QIODevice::WriteOnly | QIODevice::Truncate));
  const auto withHotkey = text.toUtf8().replace(
      "section: options\n", "section: options\n    keystroke(Control+Alt+z) = switchToNextScreen\n"
  );
  QCOMPARE(output.write(withHotkey), withHotkey.size());
  output.close();
  ServerConfigDialog reopened(nullptr, config);
  auto *reopenedEditor = reopened.findChild<MonitorLayoutWidget *>();
  QVERIFY(reopenedEditor);
  reopenedEditor->setEnabled(true);
  QVERIFY(reopened.findChild<QTabWidget *>()->isTabEnabled(0));
  auto updated = setup();
  updated.back().rect.height += 1;
  reopenedEditor->setDisplays(updated);
  reopened.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
  QCOMPARE(reopened.result(), int(QDialog::Accepted));
  QVERIFY(output.open(QIODevice::ReadOnly));
  QVERIFY(output.readAll().contains("keystroke(Control+Alt+z) = switchToNextScreen"));
}

void MonitorLayoutWidgetTests::localDisplaysAreEditableAndPersisted()
{
  Settings::setValue(Settings::Server::MonitorLayout, QVariant());
  MonitorLayoutWidget widget("independent-server", {"remote"});
  widget.setEnabled(true);
  const auto originalDisplays = widget.displays();
  QVERIFY(!originalDisplays.empty());
  auto *selection = widget.findChild<QComboBox *>("displaySelection");
  auto *x = widget.findChild<QSpinBox *>("layoutX");
  auto *width = widget.findChild<QSpinBox *>("layoutWidth");
  QVERIFY(selection && x && width);
  selection->setCurrentIndex(0);
  const auto native = widget.displays().front().nativeGeometry();
  QVERIFY(x->isEnabled());
  x->setValue(native.x + 123);
  width->setValue(native.width / 2);
  QCOMPARE(widget.displays().front().rect.x, native.x + 123);
  QCOMPARE(widget.displays().front().nativeGeometry(), native);
  widget.commit();
  const auto saved = widget.displays().front().rect;
  MonitorLayoutWidget reopened("independent-server", {"remote"});
  QCOMPARE(reopened.displays().front().rect, saved);
  QCOMPARE(reopened.displays().front().nativeGeometry(), native);
  QVERIFY(!reopened.isModified());
  reopened.setEnabled(true);
  reopened.findChild<QPushButton *>("detectDisplays")->click();
  QCOMPARE(reopened.displays().front().rect, saved);
  reopened.setEnabled(true);
  reopened.findChild<QPushButton *>("resetDetectedLayout")->click();
  QCOMPARE(reopened.displays().front().rect, native);
  QVERIFY(reopened.isModified());
}

QTEST_MAIN(MonitorLayoutWidgetTests)
