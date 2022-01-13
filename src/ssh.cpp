/*
    SPDX-FileCopyrightText: Lieven Hey <lieven.hey@kdab.com>
    SPDX-FileCopyrightText: 2022 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "ssh.h"

#include <QProcess>

#include <KConfigGroup>
#include <KSharedConfig>

#include "settings.h"
#include "util.h"

QStringList assembleSSHArguments(const QString& deviceName, const QStringList& command)
{
    KConfigGroup device = KSharedConfig::openConfig()->group("devices").group(deviceName);

    QStringList arguments = {
        QStringLiteral("%1@%2").arg(device.readEntry("username", QString()), device.readEntry("hostname", QString()))};
    const auto sshOptions = device.readEntry("sshoptions", QString());
    if (!sshOptions.isEmpty()) {
        arguments.append(sshOptions.split(QLatin1Char(' ')));
    }

    arguments.append(command);

    return arguments;
}

// set SSH_ASKPASS if not set
QProcessEnvironment sshEnvironment()
{
    auto env = Util::appImageEnvironment();
    Settings* settings = Settings::instance();
    env.insert(QLatin1String("SSH_ASKPASS"), settings->sshaskpassPath());
    return env;
}

QString sshOutput(const QString& deviceName, const QStringList& command)
{
    QProcess ssh;
    ssh.setProgram(QStandardPaths::findExecutable(QLatin1String("ssh")));
    const auto arguments = assembleSSHArguments(deviceName, command);
    ssh.setArguments(arguments);
    ssh.setProcessEnvironment(sshEnvironment());
    ssh.start();
    ssh.waitForFinished();
    return QString::fromUtf8(ssh.readAll());
}

int sshExitCode(const QString& deviceName, const QStringList& command)
{
    QProcess ssh;
    ssh.setProgram(QStandardPaths::findExecutable(QLatin1String("ssh")));
    const auto arguments = assembleSSHArguments(deviceName, command);
    ssh.setArguments(arguments);
    ssh.setProcessEnvironment(sshEnvironment());
    ssh.start();
    ssh.waitForFinished();
    return ssh.exitCode();
}
