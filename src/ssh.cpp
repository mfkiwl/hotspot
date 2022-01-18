/*
    SPDX-FileCopyrightText: Lieven Hey <lieven.hey@kdab.com>
    SPDX-FileCopyrightText: 2022 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "ssh.h"

#include <QProcess>

#include <KConfigGroup>
#include <KSharedConfig>
#include <KShell>

#include "settings.h"
#include "util.h"

namespace {
QStringList sshConnectArguments(const QString& username, const QString& hostname, const QString& sshOptions)
{
    const auto sshConnectArg = username.isEmpty() ? hostname : QLatin1String("%1@%2").arg(username, hostname);
    QStringList arguments = {sshConnectArg};
    if (!sshOptions.isEmpty()) {
        arguments.append(KShell::splitArgs(sshOptions));
    }
    return arguments;
};

QStringList assembleSSHArguments(const QString& deviceName, const QStringList& command)
{
    KConfigGroup device = KSharedConfig::openConfig()->group("devices").group(deviceName);

    const auto username = device.readEntry("username", QString());
    const auto hostname = device.readEntry("hostname", QString());
    const auto sshOptions = device.readEntry("sshoptions", QString());

    auto arguments = sshConnectArguments(username, hostname, sshOptions);

    if (!command.isEmpty()) {
        arguments += command;
    }

    return arguments;
}

// set SSH_ASKPASS if not set
QProcessEnvironment sshEnvironment()
{
    auto env = Util::appImageEnvironment();
    Settings* settings = Settings::instance();
    const auto path = settings->sshaskPassPath();
    if (!path.isEmpty() && env.value(QStringLiteral("SSH_ASKPASS")).isEmpty()) {
        env.insert(QStringLiteral("SSH_ASKPASS"), path);
    }
    return env;
}
}
QProcess* createSshProcess(const QString& username, const QString& hostname, const QString& options,
                           const QStringList& command, const QString& executable)
{
    auto ssh = new QProcess;
    ssh->setProgram(QStandardPaths::findExecutable(executable));
    auto arguments = sshConnectArguments(username, hostname, options);

    if (!command.isEmpty()) {
        arguments += command;
    }

    ssh->setArguments(arguments);
    ssh->setProcessEnvironment(sshEnvironment());
    ssh->start();
    return ssh;
}

QProcess* createSshProcess(const QString& deviceName, const QStringList& command)
{
    auto ssh = new QProcess;
    ssh->setProgram(QStandardPaths::findExecutable(QStringLiteral("ssh")));
    const auto arguments = assembleSSHArguments(deviceName, command);
    ssh->setArguments(arguments);
    ssh->setProcessEnvironment(sshEnvironment());
    ssh->start();
    return ssh;
}

QString sshOutput(const QString& deviceName, const QStringList& command)
{
    auto ssh = createSshProcess(deviceName, command);
    ssh->waitForFinished();
    auto output = QString::fromUtf8(ssh->readAll());
    delete ssh;
    return output;
}

int sshExitCode(const QString& deviceName, const QStringList& command)
{
    auto ssh = createSshProcess(deviceName, command);
    ssh->waitForFinished();
    auto code = ssh->exitCode();
    delete ssh;
    return code;
}
