/*
    SPDX-FileCopyrightText: Lieven Hey <lieven.hey@kdab.com>
    SPDX-FileCopyrightText: 2022 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QDebug>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>

#include <KShell>
#include <KUser>
#include <ThreadWeaver/ThreadWeaver>

#include <fstream>
#include <sys/stat.h>

#include "recordhost.h"
#include "settings.h"
#include "util.h"

#include "hotspot-config.h"

namespace {
QByteArray perfOutput(const QStringList& arguments)
{
    // TODO handle error if man is not installed
    QProcess process;

    auto reportError = [&]() {
        qWarning() << "Failed to run perf" << process.arguments() << process.error() << process.errorString()
                   << process.readAllStandardError();
    };

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LANG"), QStringLiteral("C"));
    process.setProcessEnvironment(env);

    QObject::connect(&process, &QProcess::errorOccurred, &process, reportError);
    process.start(QStringLiteral("perf"), arguments);
    if (!process.waitForFinished(1000) || process.exitCode() != 0)
        reportError();
    return process.readAllStandardOutput();
}

QByteArray perfRecordHelp()
{
    static const QByteArray recordHelp = []() {
        static QByteArray help = perfOutput({QStringLiteral("record"), QStringLiteral("--help")});
        if (help.isEmpty()) {
            // no man page installed, assume the best
            help = "--sample-cpu --switch-events";
        }
        return help;
    }();
    return recordHelp;
}

QByteArray perfBuildOptions()
{
    static const QByteArray buildOptions = perfOutput({QStringLiteral("version"), QStringLiteral("--build-options")});
    return buildOptions;
}

bool canTrace(const QString& path)
{
    QFileInfo info(QLatin1String("/sys/kernel/debug/tracing/") + path);
    if (!info.isDir() || !info.isReadable()) {
        return false;
    }
    QFile paranoid(QStringLiteral("/proc/sys/kernel/perf_event_paranoid"));
    return paranoid.open(QIODevice::ReadOnly) && paranoid.readAll().trimmed() == "-1";
}

bool canElevatePrivileges()
{
    const auto isRoot = KUser().isSuperUser();

    if (isRoot)
        return true;

    if (Util::sudoUtil().isEmpty() && !KF5Auth_FOUND)
        return false;

    return !Util::findLibexecBinary(QStringLiteral("elevate_perf_privileges.sh")).isEmpty();
}

bool privsAlreadyElevated()
{
    auto readSysctl = [](const char* path) {
        std::ifstream ifs {path};
        int i = std::numeric_limits<int>::min();
        if (ifs) {
            ifs >> i;
        }
        return i;
    };

    bool isElevated = readSysctl("/proc/sys/kernel/kptr_restrict") == 0;
    if (!isElevated) {
        return false;
    }

    isElevated = readSysctl("/proc/sys/kernel/perf_event_paranoid") == -1;
    if (!isElevated) {
        return false;
    }

    auto checkPerms = [](const char* path) {
        const mode_t required = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH; // 755
        struct stat buf;
        return stat(path, &buf) == 0 && ((buf.st_mode & 07777) & required) == required;
    };
    static const auto paths = {"/sys/kernel/debug", "/sys/kernel/debug/tracing"};
    isElevated = std::all_of(paths.begin(), paths.end(), checkPerms);

    return isElevated;
}

RecordHost::PerfCapabilities fetchPerfCapabilities()
{
    RecordHost::PerfCapabilities capabilities;

    capabilities.canCompress = Zstd_FOUND && perfBuildOptions().contains("zstd: [ on  ]");
    capabilities.canUseAio = perfBuildOptions().contains("aio: [ on  ]");
    capabilities.canSwitchEvents = perfRecordHelp().contains("--switch-events");
    capabilities.canSampleCpu = perfRecordHelp().contains("--sample-cpu");
    capabilities.canProfileOffCpu = canTrace(QStringLiteral("events/sched/sched_switch"));
    capabilities.canElevatePrivileges = canElevatePrivileges();
    capabilities.privilegesAlreadyElevated = privsAlreadyElevated();

    return capabilities;
}
}

RecordHost::RecordHost(QObject* parent)
    : QObject(parent)
    , m_checkPerfCapabilitiesJob(this)
    , m_checkPerfInstalledJob(this)
{
    connect(this, &RecordHost::errorOccurred, this, [this](const QString& message) { m_error = message; });

    auto connectIsReady = [this](auto&& signal) {
        connect(this, signal, this, [this] { emit isReadyChanged(isReady()); });
    };

    connectIsReady(&RecordHost::clientApplicationChanged);
    connectIsReady(&RecordHost::isPerfInstalledChanged);
    connectIsReady(&RecordHost::perfCapabilitiesChanged);
    connectIsReady(&RecordHost::recordTypeChanged);
    connectIsReady(&RecordHost::pidsChanged);
}

RecordHost::~RecordHost() { }

bool RecordHost::isReady() const
{
    if (!m_isPerfInstalled)
        return false;

    // it is save to run, when all queries where resolved and there are now errors
    std::initializer_list<const JobTracker*> jobs = {&m_checkPerfCapabilitiesJob, &m_checkPerfInstalledJob};

    switch (m_recordType) {
    case LaunchApplication:
        // client application is already validated in  the setter
        if (m_clientApplication.isEmpty())
            return false;
        break;
    case AttachToProcess:
        if (m_pids.isEmpty())
            return false;
        break;
    case ProfileSystem:
        break;
    case NUM_RECORD_TYPES:
        Q_ASSERT(false);
    }

    return m_error.isEmpty()
        && std::none_of(jobs.begin(), jobs.end(), [](const JobTracker* job) { return job->isJobRunning(); });
}

void RecordHost::setHost(const QString& host)
{
    Q_ASSERT(QThread::currentThread() == thread());

    // don't refresh if on the same host
    if (host == m_host)
        return;

    emit isReadyChanged(false);

    m_host = host;
    emit hostChanged();

    // invalidate everything
    m_cwd.clear();
    emit currentWorkingDirectoryChanged(m_cwd);

    m_clientApplication.clear();
    emit clientApplicationChanged(m_clientApplication);

    m_perfCapabilities = {};
    emit perfCapabilitiesChanged(m_perfCapabilities);

    m_checkPerfCapabilitiesJob.startJob([](auto&&) { return fetchPerfCapabilities(); },
                                        [this](RecordHost::PerfCapabilities capabilities) {
                                            Q_ASSERT(QThread::currentThread() == thread());

                                            m_perfCapabilities = capabilities;
                                            emit perfCapabilitiesChanged(m_perfCapabilities);
                                        });

    auto perfPath = Settings::instance()->perfPath();
    m_checkPerfInstalledJob.startJob(
        [isLocal = isLocal(), perfPath](auto&&) {
            if (isLocal) {
                if (perfPath.isEmpty()) {
                    return !QStandardPaths::findExecutable(QStringLiteral("perf")).isEmpty();
                }

                return QFileInfo::exists(perfPath);
            }

            qWarning() << "remote is not implemented";
            return false;
        },
        [this](bool isInstalled) {
            if (!isInstalled) {
                emit errorOccurred(tr("perf is not installed"));
            }
            m_isPerfInstalled = isInstalled;
            emit isPerfInstalledChanged(isInstalled);
        });
}

void RecordHost::setCurrentWorkingDirectory(const QString& cwd)
{
    Q_ASSERT(QThread::currentThread() == thread());

    if (isLocal()) {
        QFileInfo folder(cwd);

        if (!folder.exists()) {
            emit errorOccurred(tr("Working directory folder cannot be found: %1").arg(cwd));
        } else if (!folder.isDir()) {
            emit errorOccurred(tr("Working directory folder is not valid: %1").arg(cwd));
        } else if (!folder.isWritable()) {
            emit errorOccurred(tr("Working directory folder is not writable: %1").arg(cwd));
        } else {
            emit errorOccurred({});
            m_cwd = cwd;
            emit currentWorkingDirectoryChanged(cwd);
        }
        return;
    }

    qWarning() << __FUNCTION__ << "is not implemented for remote";
}

void RecordHost::setClientApplication(const QString& clientApplication)
{
    Q_ASSERT(QThread::currentThread() == thread());

    if (isLocal()) {
        QFileInfo application(KShell::tildeExpand(clientApplication));
        if (!application.exists()) {
            application.setFile(QStandardPaths::findExecutable(clientApplication));
        }

        if (!application.exists()) {
            emit errorOccurred(tr("Application file cannot be found: %1").arg(clientApplication));
        } else if (!application.isFile()) {
            emit errorOccurred(tr("Application file is not valid: %1").arg(clientApplication));
        } else if (!application.isExecutable()) {
            emit errorOccurred(tr("Application file is not executable: %1").arg(clientApplication));
        } else {
            emit errorOccurred({});
            m_clientApplication = clientApplication;
            emit clientApplicationChanged(m_clientApplication);
        }
        return;
    }

    qWarning() << __FUNCTION__ << "is not implemented for remote";
}

QString RecordHost::perfBinary() const
{
    return Settings::instance()->perfPath();
}

void RecordHost::setOutputFileName(const QString& filePath)
{
    if (isLocal()) {
        const auto perfDataExtension = QStringLiteral(".data");

        QFileInfo file(filePath);
        QFileInfo folder(file.absolutePath());

        if (!folder.exists()) {
            emit errorOccurred(tr("Output file directory folder cannot be found: %1").arg(folder.path()));
        } else if (!folder.isDir()) {
            emit errorOccurred(tr("Output file directory folder is not valid: %1").arg(folder.path()));
        } else if (!folder.isWritable()) {
            emit errorOccurred(tr("Output file directory folder is not writable: %1").arg(folder.path()));
        } else if (!file.absoluteFilePath().endsWith(perfDataExtension)) {
            emit errorOccurred(tr("Output file must end with %1").arg(perfDataExtension));
        } else {
            emit errorOccurred({});
            m_outputFileName = filePath;
            emit outputFileNameChanged(m_outputFileName);
        }

        return;
    }

    qWarning() << __FUNCTION__ << "is not implemented for remote";
}

void RecordHost::setRecordType(RecordType type)
{
    if (m_recordType != type) {
        m_recordType = type;
        emit recordTypeChanged(m_recordType);

        m_pids.clear();
        emit pidsChanged();
    }
}

void RecordHost::setPids(const QStringList& pids)
{
    if (m_pids != pids) {
        m_pids = pids;
        emit pidsChanged();
    }
}
