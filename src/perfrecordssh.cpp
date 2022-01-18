/*
    SPDX-FileCopyrightText: Lieven Hey <lieven.hey@kdab.com>
    SPDX-FileCopyrightText: 2022 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "perfrecordssh.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include <csignal>

#include "hotspot-config.h"
#include "ssh.h"

PerfRecordSSH::PerfRecordSSH(QObject* parent)
    : PerfRecord(parent)
{
}

PerfRecordSSH::~PerfRecordSSH() = default;

void PerfRecordSSH::record(const QStringList& perfOptions, const QString& outputPath, bool /*elevatePrivileges*/,
                           const QString& exePath, const QStringList& exeOptions, const QString& workingDirectory)
{
    QStringList recordOptions = {exePath};
    recordOptions += exeOptions;

    startRecording(perfOptions, outputPath, recordOptions, workingDirectory);
}

void PerfRecordSSH::record(const QStringList& perfOptions, const QString& outputPath, bool /*elevatePrivileges*/,
                           const QStringList& pids)
{
    if (pids.empty()) {
        emit recordingFailed(tr("Process does not exist."));
        return;
    }

    QStringList options = perfOptions;
    options += {QStringLiteral("--pid"), pids.join(QLatin1Char(','))};
    startRecording(options, outputPath, {}, {});
}

void PerfRecordSSH::recordSystem(const QStringList& perfOptions, const QString& outputPath)
{
    auto options = perfOptions;
    options.append(QStringLiteral("--all-cpus"));
    startRecording(options, outputPath, {}, {});
}

void PerfRecordSSH::stopRecording()
{
    if (m_recordProcess) {
        m_userTerminated = true;
        m_outputFile->close();
        m_recordProcess->terminate();
        m_recordProcess->waitForFinished();
        m_recordProcess = nullptr;
    }
}

void PerfRecordSSH::sendInput(const QByteArray& input)
{
    if (m_recordProcess)
        m_recordProcess->write(input);
}

QString PerfRecordSSH::currentUsername()
{
    // this is only used to automatically check the elevate privileges checkbox if the user is root
    // since we currently do not support privilege elevation over ssh returning an empty string is fine
    return {};
}

bool PerfRecordSSH::canTrace(const QString& path)
{
    Q_UNUSED(path);

    if (m_deviceName.isEmpty())
        return false;

    // assume best case
    return true;
}

bool PerfRecordSSH::canProfileOffCpu()
{
    if (m_deviceName.isEmpty())
        return false;
    return canTrace(QStringLiteral("events/sched/sched_switch"));
}

QString perfRecordHelp(const QString& hostname)
{
    const QString recordHelp = [hostname]() {
        static QString help =
            sshOutput(hostname, {QLatin1String("perf"), QLatin1String("record"), QLatin1String("--help")});
        if (help.isEmpty()) {
            // no man page installed, assume the best
            help = QStringLiteral("--sample-cpu --switch-events");
        }
        return help;
    }();
    return recordHelp;
}

QString perfBuildOptions(const QString& hostname)
{
    const QString buildOptionsHelper = [hostname]() {
        static QString buildOptions =
            sshOutput(hostname, {QLatin1String("perf"), QLatin1String("version"), QLatin1String("--build-options")});
        return buildOptions;
    }();
    return buildOptionsHelper;
}

bool PerfRecordSSH::canSampleCpu()
{
    if (m_deviceName.isEmpty())
        return false;
    return perfRecordHelp(m_deviceName).contains(QLatin1String("--sample-cpu"));
}

bool PerfRecordSSH::canSwitchEvents()
{
    if (m_deviceName.isEmpty())
        return false;
    return perfRecordHelp(m_deviceName).contains(QLatin1String("--switch-events"));
}

bool PerfRecordSSH::canUseAio()
{
    // perf reports "error: Illegal seek" when trying to use aio and streaming data
    return false;
}

bool PerfRecordSSH::canCompress()
{
    // perf does not include the compressed header information
    // run perf record -o - --call-graph=dwarf -z ls > perf.data
    // perfparser will report: encountered PERF_RECORD_COMPRESSED without HEADER_COMPRESSED information
    return false;
}

bool PerfRecordSSH::isPerfInstalled()
{
    if (m_deviceName.isEmpty())
        return false;
    return sshExitCode(m_deviceName, {QLatin1String("command"), QLatin1String("-v"), QLatin1String("perf")}) == 0;
}

void PerfRecordSSH::startRecording(const QStringList& perfOptions, const QString& outputPath,
                                   const QStringList& recordOptions, const QString& workingDirectory)
{
    Q_UNUSED(workingDirectory);

    if (m_recordProcess) {
        stopRecording();
    }

    QFileInfo outputFileInfo(outputPath);
    QString folderPath = outputFileInfo.dir().path();
    QFileInfo folderInfo(folderPath);
    if (!folderInfo.exists()) {
        emit recordingFailed(tr("Folder '%1' does not exist.").arg(folderPath));
        return;
    }
    if (!folderInfo.isDir()) {
        emit recordingFailed(tr("'%1' is not a folder.").arg(folderPath));
        return;
    }
    if (!folderInfo.isWritable()) {
        emit recordingFailed(tr("Folder '%1' is not writable.").arg(folderPath));
        return;
    }

    qRegisterMetaType<QProcess::ExitStatus>("QProcess::ExitStatus");

    QStringList perfCommand = {QStringLiteral("cd"),   workingDirectory,         QStringLiteral("&&"),
                               QStringLiteral("perf"), QStringLiteral("record"), QStringLiteral("-o"),
                               QStringLiteral("-")};
    perfCommand += perfOptions;
    perfCommand += recordOptions;
    m_outputFile = new QFile(outputPath);
    m_outputFile->open(QIODevice::WriteOnly);

    m_recordProcess = createSshProcess(m_deviceName, perfCommand);

    emit recordingStarted(QStringLiteral("perf"), perfCommand);

    connect(m_recordProcess, &QProcess::readyReadStandardOutput, this,
            [this] { m_outputFile->write(m_recordProcess->readAllStandardOutput()); });

    connect(m_recordProcess, &QProcess::readyReadStandardError, this,
            [this] { emit recordingOutput(QString::fromUtf8(m_recordProcess->readAllStandardError())); });

    connect(m_recordProcess, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
                Q_UNUSED(exitStatus)

                m_outputFile->close();

                if ((exitCode == EXIT_SUCCESS || (exitCode == SIGTERM && m_userTerminated) || m_outputFile->size() > 0)
                    && m_outputFile->exists()) {
                    if (exitCode != EXIT_SUCCESS && !m_userTerminated) {
                        emit debuggeeCrashed();
                    }
                    emit recordingFinished(m_outputFile->fileName());
                } else {
                    emit recordingFailed(tr("Failed to record perf data, error code %1.").arg(exitCode));
                }
                m_userTerminated = false;

                emit recordingFinished(m_outputFile->fileName());
            });
}
