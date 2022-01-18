/*
    SPDX-FileCopyrightText: Lieven Hey <lieven.hey@kdab.com>
    SPDX-FileCopyrightText: 2022 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QString>

class QStringList;
class QProcess;

// used in the dialog to test connection and copy ssh keys
// executable is the program used to connect (can be changed to ssh-copy-id)
// command the the command to run on the remote device
// options are the ssh options
QProcess* createSshProcess(const QString& username, const QString& hostname, const QString& options,
                           const QStringList& command, const QString& executable = QStringLiteral("ssh"));

// same as above, but load the values from the config file
QProcess* createSshProcess(const QString& deviceName, const QStringList& command);
QString sshOutput(const QString& deviceName, const QStringList& command);
int sshExitCode(const QString& deviceName, const QStringList& command);
