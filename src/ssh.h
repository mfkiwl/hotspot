/*
    SPDX-FileCopyrightText: Lieven Hey <lieven.hey@kdab.com>
    SPDX-FileCopyrightText: 2022 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

class QString;
class QStringList;
class QProcessEnvironment;

QStringList assembleSSHArguments(const QString& deviceName, const QStringList& command);
QProcessEnvironment sshEnvironment();
QString sshOutput(const QString& deviceName, const QStringList& command);
int sshExitCode(const QString& deviceName, const QStringList& command);
