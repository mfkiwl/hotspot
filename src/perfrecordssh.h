/*
    SPDX-FileCopyrightText: Lieven Hey <lieven.hey@kdab.com>
    SPDX-FileCopyrightText: 2022 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "perfrecord.h"

class QFile;

class PerfRecordSSH : public PerfRecord
{
    Q_OBJECT
public:
    PerfRecordSSH(QObject* parent = nullptr);
    ~PerfRecordSSH();

    void record(const QStringList& perfOptions, const QString& outputPath, bool elevatePrivileges,
                const QString& exePath, const QStringList& exeOptions, const QString& workingDirectory) override;
    void record(const QStringList& perfOptions, const QString& outputPath, bool elevatePrivileges,
                const QStringList& pids) override;
    void recordSystem(const QStringList& perfOptions, const QString& outputPath) override;
    void stopRecording() override;
    void sendInput(const QByteArray& input) override;

    QString sudoUtil() override
    {
        return {};
    };
    bool canElevatePrivileges() override
    {
        return false;
    }
    QString currentUsername() override;

    bool canTrace(const QString& path) override;
    bool canProfileOffCpu() override;
    bool canSampleCpu() override;
    bool canSwitchEvents() override;
    bool canUseAio() override;
    bool canCompress() override;
    bool isPerfInstalled() override;

private:
    void startRecording(const QStringList& perfOptions, const QString& outputPath, const QStringList& recordOptions,
                        const QString& workingDirectory);

    QProcess* m_recordProcess = nullptr;
    QFile* m_outputFile;
    QString m_hostname;
    bool m_userTerminated = false;
};
