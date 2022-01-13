/*
    SPDX-FileCopyrightText: Nate Rogers <nate.rogers@kdab.com>
    SPDX-FileCopyrightText: Milian Wolff <milian.wolff@kdab.com>
    SPDX-FileCopyrightText: 2016-2022 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QObject>
#include <QPointer>

class QProcess;

class PerfRecord : public QObject
{
    Q_OBJECT
public:
    explicit PerfRecord(QObject* parent = nullptr);
    ~PerfRecord();

    virtual void record(const QStringList& perfOptions, const QString& outputPath, bool elevatePrivileges,
                        const QString& exePath, const QStringList& exeOptions,
                        const QString& workingDirectory = QString());
    virtual void record(const QStringList& perfOptions, const QString& outputPath, bool elevatePrivileges,
                        const QStringList& pids);
    virtual void recordSystem(const QStringList& perfOptions, const QString& outputPath);

    const QString perfCommand();
    virtual void stopRecording();
    virtual void sendInput(const QByteArray& input);

    virtual QString sudoUtil();
    virtual bool canElevatePrivileges();
    virtual QString currentUsername();

    virtual bool canTrace(const QString& path);
    virtual bool canProfileOffCpu();
    virtual bool canSampleCpu();
    virtual bool canSwitchEvents();
    virtual bool canUseAio();
    virtual bool canCompress();

    static QStringList offCpuProfilingOptions();

    virtual bool isPerfInstalled();

signals:
    void recordingStarted(const QString& perfBinary, const QStringList& arguments);
    void recordingFinished(const QString& fileLocation);
    void recordingFailed(const QString& errorMessage);
    void recordingOutput(const QString& errorMessage);
    void debuggeeCrashed();

private:
    QPointer<QProcess> m_perfRecordProcess;
    QPointer<QProcess> m_elevatePrivilegesProcess;
    QString m_outputPath;
    bool m_userTerminated;

    void startRecording(bool elevatePrivileges, const QStringList& perfOptions, const QString& outputPath,
                        const QStringList& recordOptions, const QString& workingDirectory = QString());
    void startRecording(const QStringList& perfOptions, const QString& outputPath, const QStringList& recordOptions,
                        const QString& workingDirectory = QString());
};
