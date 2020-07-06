#include "monitor.h"

// Qt lib import
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QThread>

// Windows lib import
#ifdef Q_OS_WIN
#include "qt_windows.h"
#endif

// macOS lib import
#ifdef Q_OS_MAC
#include <mach/mach_host.h>
#endif

// Helper
#ifdef Q_OS_WIN
QDateTime fileTimeToQDateTime(const FILETIME &fileTime) {
    return QDateTime(QDate(1601, 1, 1), QTime(0, 0, 0, 0), Qt::UTC)
        .addMSecs(
            ((qint64(fileTime.dwHighDateTime) << 32) | fileTime.dwLowDateTime) /
            10000);
}

qint64 fileTimeToMSecs(const FILETIME &fileTime) {
    return ((qint64(fileTime.dwHighDateTime) << 32) | fileTime.dwLowDateTime) /
           10000;
}
#endif

// JQCPUMonitor
QPointer<JQCPUMonitor> JQCPUMonitor::cpuMonitor_;
bool JQCPUMonitor::continueFlag_ = true;

QMutex JQCPUMonitor::mutex_;
QList<QPair<qint64, qreal> >
    JQCPUMonitor::cpuUsagePercentageRecords_;  // [ { time, percentage }, ... ]

void JQCPUMonitor::initialize() {
    if (cpuMonitor_) {
        QThread::sleep(1);
        qDebug() << "JQCPUMonitor: duplicate initialize";
        return;
    }

    cpuMonitor_ = new JQCPUMonitor;
    cpuMonitor_->start();

    QObject::connect(qApp, &QCoreApplication::aboutToQuit,
                     []() { continueFlag_ = false; });
}

qreal JQCPUMonitor::cpuUsagePercentage() {
    QMutexLocker locker(&mutex_);

    if (cpuUsagePercentageRecords_.isEmpty()) {
        return 0.0;
    }

    return cpuUsagePercentageRecords_.last().second;
}

void JQCPUMonitor::run() {
    while (continueFlag_) {
        QThread::msleep(1200);

        tick();
    }
}

void JQCPUMonitor::tick() {
    static qint64 lastCpuTime = 0;
    static qint64 lastQueryTime = 0;

    qint64 currentCpuTime = 0;
    const auto &&currentMSecsSinceEpoch = QDateTime::currentMSecsSinceEpoch();

#if (defined Q_OS_WIN)

    FILETIME idleTime;
    FILETIME kernelTime;
    FILETIME userTime;

    const auto &&isGetSucceed =
        GetSystemTimes(&idleTime, &kernelTime, &userTime);
    if (!isGetSucceed) {
        qDebug() << "JQCPUMonitor::tick: error";
        return;
    }

    currentCpuTime += fileTimeToMSecs(kernelTime);
    currentCpuTime += fileTimeToMSecs(userTime);
    currentCpuTime -= fileTimeToMSecs(idleTime);

    currentCpuTime = qMax(currentCpuTime, static_cast<qint64>(0));
    currentCpuTime /= QThread::idealThreadCount();

#elif (defined Q_OS_MAC)

    natural_t processorCount;
    processor_cpu_load_info_t cpuLoad;
    mach_msg_type_number_t processorMsgCount;

    host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO,
                        &processorCount,
                        reinterpret_cast<processor_info_array_t *>(&cpuLoad),
                        &processorMsgCount);

    for (natural_t index = 0; index < processorCount; ++index) {
        currentCpuTime += cpuLoad[index].cpu_ticks[CPU_STATE_USER];
        currentCpuTime += cpuLoad[index].cpu_ticks[CPU_STATE_SYSTEM];
        currentCpuTime += cpuLoad[index].cpu_ticks[CPU_STATE_NICE];
    }

#elif (defined Q_OS_LINUX)

    QFile file("/proc/stat");
    if (!file.open(QIODevice::ReadOnly)) {
        qDebug() << "JQCPUMonitor::tick: error(1)";
        return;
    }
#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
    const auto &&dataList =
        QString(file.readLine()).split(' ', Qt::SkipEmptyParts);
#else
    const auto &&dataList =
        QString(file.readLine()).split(' ', QString::SkipEmptyParts);
#endif
    // qDebug() << dataList;

    if (dataList.size() < 4) {
        qDebug() << "JQCPUMonitor::tick: error(2)";
        return;
    }

    currentCpuTime += dataList[1].toLongLong() * 10;  // user
    currentCpuTime += dataList[2].toLongLong() * 10;  // nice
    currentCpuTime += dataList[3].toLongLong() * 10;  // system

    currentCpuTime /= QThread::idealThreadCount();

#endif

    if (lastCpuTime && (lastCpuTime < currentMSecsSinceEpoch)) {
        const auto &&userTimeOffset = currentCpuTime - lastCpuTime;
        const auto &&queryTimeOffset = currentMSecsSinceEpoch - lastQueryTime;

        QMutexLocker locker(&mutex_);

        cpuUsagePercentageRecords_.push_back(
            {currentMSecsSinceEpoch,
             qMax(qMin(static_cast<qreal>(userTimeOffset) /
                           static_cast<qreal>(queryTimeOffset),
                       1.0),
                  0.0)});

        while (cpuUsagePercentageRecords_.size() > 1800) {
            cpuUsagePercentageRecords_.pop_front();
        }
    }

    lastCpuTime = currentCpuTime;
    lastQueryTime = currentMSecsSinceEpoch;
}
