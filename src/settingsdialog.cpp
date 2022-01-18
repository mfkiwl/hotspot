/*
    SPDX-FileCopyrightText: Petr Lyapidevskiy <p.lyapidevskiy@nips.ru>
    SPDX-FileCopyrightText: Milian Wolff <milian.wolff@kdab.com>
    SPDX-FileCopyrightText: 2016-2022 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "settingsdialog.h"

#include "ui_callgraphsettingspage.h"
#include "ui_debuginfodpage.h"
#include "ui_flamegraphsettingspage.h"
#include "ui_sshsettingspage.h"
#include "ui_unwindsettingspage.h"

#include "multiconfigwidget.h"
#include "settings.h"

#include <KComboBox>
#include <KConfigGroup>
#include <KSharedConfig>
#include <KUrlRequester>

#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QProcess>

#include "multiconfigwidget.h"
#include "ssh.h"

#include "hotspot-config.h"

namespace {
auto config()
{
    return KSharedConfig::openConfig();
}
}

SettingsDialog::SettingsDialog(QWidget* parent)
    : KPageDialog(parent)
    , unwindPage(new Ui::UnwindSettingsPage)
    , flamegraphPage(new Ui::FlamegraphSettingsPage)
    , debuginfodPage(new Ui::DebuginfodPage)
#if KGraphViewerPart_FOUND
    , callgraphPage(new Ui::CallgraphSettingsPage)
#endif
    , sshPage(new Ui::SshSettingsPage)
{
    addPathSettingsPage();
    addFlamegraphPage();
    addDebuginfodPage();
#if KGraphViewerPart_FOUND
    addCallgraphPage();
#endif
    addSshPage();
}

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::initSettings()
{
    const auto configName = Settings::instance()->lastUsedEnvironment();
    if (!configName.isEmpty()) {
        m_configs->selectConfig(configName);
    }
}

void SettingsDialog::initSettings(const QString& sysroot, const QString& appPath, const QString& extraLibPaths,
                                  const QString& debugPaths, const QString& kallsyms, const QString& arch,
                                  const QString& objdump)
{
    auto fromPathString = [](KEditListWidget* listWidget, const QString& string) {
        listWidget->setItems(string.split(QLatin1Char(':'), Qt::SkipEmptyParts));
    };
    fromPathString(unwindPage->extraLibraryPaths, extraLibPaths);
    fromPathString(unwindPage->debugPaths, debugPaths);

    unwindPage->lineEditSysroot->setText(sysroot);
    unwindPage->lineEditApplicationPath->setText(appPath);
    unwindPage->lineEditKallsyms->setText(kallsyms);
    unwindPage->lineEditObjdump->setText(objdump);

    int itemIndex = 0;
    if (!arch.isEmpty()) {
        itemIndex = unwindPage->comboBoxArchitecture->findText(arch);
        if (itemIndex == -1) {
            itemIndex = unwindPage->comboBoxArchitecture->count();
            unwindPage->comboBoxArchitecture->addItem(arch);
        }
    }
    unwindPage->comboBoxArchitecture->setCurrentIndex(itemIndex);
}

QString SettingsDialog::sysroot() const
{
    return unwindPage->lineEditSysroot->text();
}

QString SettingsDialog::appPath() const
{
    return unwindPage->lineEditApplicationPath->text();
}

QString SettingsDialog::extraLibPaths() const
{
    return unwindPage->extraLibraryPaths->items().join(QLatin1Char(':'));
}

QString SettingsDialog::debugPaths() const
{
    return unwindPage->debugPaths->items().join(QLatin1Char(':'));
}

QString SettingsDialog::kallsyms() const
{
    return unwindPage->lineEditKallsyms->text();
}

QString SettingsDialog::arch() const
{
    QString sArch = unwindPage->comboBoxArchitecture->currentText();
    return (sArch == QLatin1String("auto-detect")) ? QString() : sArch;
}

QString SettingsDialog::objdump() const
{
    return unwindPage->lineEditObjdump->text();
}

void SettingsDialog::addPathSettingsPage()
{
    auto page = new QWidget(this);
    auto item = addPage(page, tr("Unwinding"));
    item->setHeader(tr("Unwind Options"));
    item->setIcon(QIcon::fromTheme(QStringLiteral("preferences-system-windows-behavior")));

    unwindPage->setupUi(page);

    auto setupMultiPath = [](KEditListWidget* listWidget, QLabel* buddy, QWidget* previous) {
        auto editor = new KUrlRequester(listWidget);
        editor->setPlaceholderText(tr("auto-detect"));
        editor->setMode(KFile::LocalOnly | KFile::Directory | KFile::ExistingOnly);
        buddy->setBuddy(editor);
        listWidget->setCustomEditor(editor->customEditor());
        QWidget::setTabOrder(previous, editor);
        QWidget::setTabOrder(editor, listWidget->listView());
        QWidget::setTabOrder(listWidget->listView(), listWidget->addButton());
        QWidget::setTabOrder(listWidget->addButton(), listWidget->removeButton());
        QWidget::setTabOrder(listWidget->removeButton(), listWidget->upButton());
        QWidget::setTabOrder(listWidget->upButton(), listWidget->downButton());
        return listWidget->downButton();
    };
    auto lastExtraLibsWidget = setupMultiPath(unwindPage->extraLibraryPaths, unwindPage->extraLibraryPathsLabel,
                                              unwindPage->lineEditApplicationPath);
    setupMultiPath(unwindPage->debugPaths, unwindPage->debugPathsLabel, lastExtraLibsWidget);

    auto* label = new QLabel(this);
    label->setText(tr("Config:"));

    auto pathConfig = config()->group("PerfPaths");

    auto saveFunction = [this](KConfigGroup group) {
        group.writeEntry("sysroot", sysroot());
        group.writeEntry("appPath", appPath());
        group.writeEntry("extraLibPaths", extraLibPaths());
        group.writeEntry("debugPaths", debugPaths());
        group.writeEntry("kallsyms", kallsyms());
        group.writeEntry("arch", arch());
        group.writeEntry("objdump", objdump());
    };

    auto restoreFunction = [this, &pathConfig](const KConfigGroup& group) {
        const auto sysroot = group.readEntry("sysroot");
        const auto appPath = group.readEntry("appPath");
        const auto extraLibPaths = group.readEntry("extraLibPaths");
        const auto debugPaths = group.readEntry("debugPaths");
        const auto kallsyms = group.readEntry("kallsyms");
        const auto arch = group.readEntry("arch");
        const auto objdump = group.readEntry("objdump");
        initSettings(sysroot, appPath, extraLibPaths, debugPaths, kallsyms, arch, objdump);
        pathConfig.writeEntry("lastUsed", m_configs->currentConfig());
    };

    m_configs = new MultiConfigWidget(this);
    m_configs->setConfig(pathConfig);
    m_configs->restoreCurrent();

    connect(m_configs, &MultiConfigWidget::saveConfig, this, saveFunction);
    connect(m_configs, &MultiConfigWidget::restoreConfig, this, restoreFunction);

    unwindPage->formLayout->insertRow(0, label, m_configs);

    connect(this, &KPageDialog::accepted, this, [this] { m_configs->updateCurrentConfig(); });

    for (auto field : {unwindPage->lineEditSysroot, unwindPage->lineEditApplicationPath, unwindPage->lineEditKallsyms,
                       unwindPage->lineEditObjdump}) {
        connect(field, &KUrlRequester::textEdited, m_configs, &MultiConfigWidget::updateCurrentConfig);
        connect(field, &KUrlRequester::urlSelected, m_configs, &MultiConfigWidget::updateCurrentConfig);
    }

    connect(unwindPage->comboBoxArchitecture, QOverload<int>::of(&QComboBox::currentIndexChanged), m_configs,
            &MultiConfigWidget::updateCurrentConfig);

    connect(unwindPage->debugPaths, &KEditListWidget::changed, m_configs, &MultiConfigWidget::updateCurrentConfig);
    connect(unwindPage->extraLibraryPaths, &KEditListWidget::changed, m_configs,
            &MultiConfigWidget::updateCurrentConfig);
}

void SettingsDialog::keyPressEvent(QKeyEvent* event)
{
    // disable the return -> accept policy since it prevents the user from confirming name changes in the combobox
    // you can still press CTRL + Enter to close the dialog
    if (event->modifiers() != Qt::Key_Control && (event->key() == Qt::Key_Enter || event->key() == Qt::Key_Return)) {
        return;
    }
    QDialog::keyPressEvent(event);
}

void SettingsDialog::addFlamegraphPage()
{
    auto page = new QWidget(this);
    auto item = addPage(page, tr("Flamegraph"));
    item->setHeader(tr("Flamegraph Options"));
    item->setIcon(QIcon::fromTheme(QStringLiteral("preferences-system-windows-behavior")));

    flamegraphPage->setupUi(page);

    auto setupMultiPath = [](KEditListWidget* listWidget, QLabel* buddy, QWidget* previous) {
        auto editor = new KUrlRequester(listWidget);
        editor->setMode(KFile::LocalOnly | KFile::Directory | KFile::ExistingOnly);
        buddy->setBuddy(editor);
        listWidget->setCustomEditor(editor->customEditor());
        QWidget::setTabOrder(previous, editor);
        QWidget::setTabOrder(editor, listWidget->listView());
        QWidget::setTabOrder(listWidget->listView(), listWidget->addButton());
        QWidget::setTabOrder(listWidget->addButton(), listWidget->removeButton());
        QWidget::setTabOrder(listWidget->removeButton(), listWidget->upButton());
        QWidget::setTabOrder(listWidget->upButton(), listWidget->downButton());
        return listWidget->downButton();
    };

    auto lastUserPath = setupMultiPath(flamegraphPage->userPaths, flamegraphPage->userPathsLabel, nullptr);
    setupMultiPath(flamegraphPage->systemPaths, flamegraphPage->systemPathsLabel, lastUserPath);

    flamegraphPage->userPaths->insertStringList(Settings::instance()->userPaths());
    flamegraphPage->systemPaths->insertStringList(Settings::instance()->systemPaths());

    connect(Settings::instance(), &Settings::pathsChanged, this, [this] {
        flamegraphPage->userPaths->clear();
        flamegraphPage->systemPaths->clear();
        flamegraphPage->userPaths->insertStringList(Settings::instance()->userPaths());
        flamegraphPage->systemPaths->insertStringList(Settings::instance()->systemPaths());
    });

    connect(buttonBox(), &QDialogButtonBox::accepted, this, [this] {
        Settings::instance()->setPaths(flamegraphPage->userPaths->items(), flamegraphPage->systemPaths->items());
    });
}

void SettingsDialog::addDebuginfodPage()
{
    auto page = new QWidget(this);
    auto item = addPage(page, tr("debuginfod"));
    item->setHeader(tr("debuginfod Urls"));
    item->setIcon(QIcon::fromTheme(QStringLiteral("preferences-system-windows-behavior")));

    debuginfodPage->setupUi(page);

    debuginfodPage->urls->insertStringList(Settings::instance()->debuginfodUrls());

    connect(Settings::instance(), &Settings::debuginfodUrlsChanged, this, [this] {
        debuginfodPage->urls->clear();
        debuginfodPage->urls->insertStringList(Settings::instance()->debuginfodUrls());
    });

    connect(buttonBox(), &QDialogButtonBox::accepted, this,
            [this] { Settings::instance()->setDebuginfodUrls(debuginfodPage->urls->items()); });
}

void SettingsDialog::addCallgraphPage()
{
    auto page = new QWidget(this);
    auto item = addPage(page, tr("Callgraph"));
    item->setHeader(tr("Callgraph Settings"));
    item->setIcon(QIcon::fromTheme(QStringLiteral("preferences-system-windows-behavior")));

    callgraphPage->setupUi(page);

    connect(Settings::instance(), &Settings::callgraphChanged, this, [this] {
        auto settings = Settings::instance();
        callgraphPage->parentSpinBox->setValue(settings->callgraphParentDepth());
        callgraphPage->childSpinBox->setValue(settings->callgraphChildDepth());
        callgraphPage->currentFunctionColor->setColor(settings->callgraphActiveColor());
        callgraphPage->functionColor->setColor(settings->callgraphColor());
    });

    connect(buttonBox(), &QDialogButtonBox::accepted, this, [this] {
        auto settings = Settings::instance();
        settings->setCallgraphParentDepth(callgraphPage->parentSpinBox->value());
        settings->setCallgraphChildDepth(callgraphPage->childSpinBox->value());
        settings->setCallgraphColors(callgraphPage->currentFunctionColor->color().name(),
                                     callgraphPage->functionColor->color().name());
    });
}

void SettingsDialog::addSshPage()
{
    const auto sshPath = QStandardPaths::findExecutable(QStringLiteral("ssh"));

    // only show page if ssh is installed
    if (sshPath.isEmpty()) {
        return;
    }

    auto page = new QWidget(this);
    auto item = addPage(page, tr("SSH"));
    item->setHeader(tr("SSH Settings"));
    item->setIcon(QIcon::fromTheme(QStringLiteral("preferences-system-windows-behavior")));

    sshPage->setupUi(page);

    auto deviceConfig = config()->group("devices");

    auto saveFunction = [this](KConfigGroup group) {
        group.writeEntry("hostname", sshPage->hostnameLineEdit->text());
        group.writeEntry("username", sshPage->usernameLineEdit->text());
        group.writeEntry("sshoptions", sshPage->sshOptionsLineEdit->text());
    };

    auto restoreFunction = [this](const KConfigGroup& group) {
        const auto hostname = group.readEntry("hostname");
        const auto username = group.readEntry("username");
        const auto sshOptions = group.readEntry("sshoptions");

        sshPage->hostnameLineEdit->setText(hostname);
        sshPage->usernameLineEdit->setText(username);
        sshPage->sshOptionsLineEdit->setText(sshOptions);
    };

    MultiConfigWidget* devices = new MultiConfigWidget(this);

    // TODO: fix tab order

    connect(devices, &MultiConfigWidget::saveConfig, this, saveFunction);
    connect(devices, &MultiConfigWidget::restoreConfig, this, restoreFunction);

    devices->setConfig(deviceConfig);
    devices->restoreCurrent();

    sshPage->groupBox->layout()->replaceWidget(sshPage->multiConfigWidget, devices);

    Settings* settings = Settings::instance();
    sshPage->sshaskpassLineEdit->setText(settings->sshaskPassPath());

    connect(settings, &Settings::sshaskPassChanged, this,
            [this](const QString& path) { sshPage->sshaskpassLineEdit->setText(path); });

    connect(sshPage->testButton, &QPushButton::pressed, this, [this, devices] {
        auto ssh = createSshProcess(sshPage->usernameLineEdit->text(), sshPage->hostnameLineEdit->text(),
                                    sshPage->sshOptionsLineEdit->text(), QStringList({QStringLiteral("perf")}));

        connect(
            ssh, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, ssh, devices](int exitCode, QProcess::ExitStatus exitStatus) {
                Q_UNUSED(exitStatus);

                auto showError = [this](const QString& errorText) {
                    sshPage->errorMessageWidget->setText(errorText);
                    sshPage->errorMessageWidget->show();
                };

                const auto hostname = sshPage->hostnameLineEdit->text();

                if (exitCode == 255) {
                    showError(tr("Failed to connect to %1").arg(hostname));
                } else if (exitCode == 127) {
                    showError(tr("Could not find perf binary"));
                } else if (exitCode == 1) {
                    sshPage->successMessageWidget->setText(tr("Successfully connected to %1").arg(hostname));
                    sshPage->successMessageWidget->show();
                    devices->updateCurrentConfig();
                } else {
                    auto cmd = QLatin1String("ssh %3 %1@%2 %4")
                                   .arg(sshPage->usernameLineEdit->text(), sshPage->hostnameLineEdit->text(),
                                        sshPage->sshOptionsLineEdit->text(), QStringLiteral("perf"));
                    showError(
                        tr("Command Failed: %1\nError: %2").arg(cmd, QString::fromUtf8(ssh->readAllStandardError())));
                }

                ssh->deleteLater();
            });

        ssh->start();
    });

    connect(sshPage->copyKeyButton, &QPushButton::pressed, this, [this] {
        auto sshCopyId = createSshProcess(sshPage->usernameLineEdit->text(), sshPage->hostnameLineEdit->text(),
                                          sshPage->sshOptionsLineEdit->text(), {}, QStringLiteral("ssh-copy-id"));

        connect(sshCopyId, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this, sshCopyId](int exitCode, QProcess::ExitStatus exitStatus) {
                    Q_UNUSED(exitStatus);

                    if (exitCode == 1) {
                        auto cmd = QLatin1String("ssh-copy-id %3 %1@%2")
                                       .arg(sshPage->usernameLineEdit->text(), sshPage->hostnameLineEdit->text(),
                                            sshPage->sshOptionsLineEdit->text());
                        sshPage->errorMessageWidget->setText(
                            tr("Command Failed: %1\nError: %2")
                                .arg(cmd, QString::fromUtf8(sshCopyId->readAllStandardError())));
                        sshPage->errorMessageWidget->show();
                    } else {
                        sshPage->successMessageWidget->setText(tr("Successfully installed ssh key"));
                        sshPage->successMessageWidget->show();
                    }

                    sshCopyId->deleteLater();
                });

        sshCopyId->start();
    });

    sshPage->errorMessageWidget->setVisible(false);
    sshPage->successMessageWidget->setVisible(false);
}
