#include "CpuBudget.h"
#include "Paths.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <sched.h>

namespace omafiles {
QList<int> workerCpus(const QString &procRoot)
{
    cpu_set_t allowed;
    QList<int> cpus;
    if (::sched_getaffinity(0, sizeof(allowed), &allowed) == 0)
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
            if (CPU_ISSET(cpu, &allowed)) cpus.append(cpu);
    if (cpus.isEmpty()) return {0};
    if (cpus.size() == 1) return cpus;
    int limit = qMin(4, int(cpus.size()));
    QFile membership(procRoot + "/self/cgroup"), mounts(procRoot + "/self/mountinfo");
    if (!membership.open(QIODevice::ReadOnly) || !mounts.open(QIODevice::ReadOnly))
        return cpus.first(1);
    const QList<QByteArray> groups = membership.readAll().split('\n');
    const auto unescape = [](QByteArray path) {
        for (const auto &escape : {QByteArray("040"), QByteArray("011"), QByteArray("012"), QByteArray("134")})
            path.replace('\\' + escape, QByteArray(1, char(escape.toInt(nullptr, 8))));
        QString name;
        return nameFromBytes(path, &name) ? name : QString();
    };
    bool mapped = false;
    for (const QByteArray &line : mounts.readAll().split('\n')) {
        const QList<QByteArray> mount = line.trimmed().split(' ');
        const qsizetype separator = mount.indexOf("-");
        if (separator < 6 || separator + 3 >= mount.size()) continue;
        const bool unified = mount[separator + 1] == "cgroup2";
        if (!unified && (mount[separator + 1] != "cgroup"
            || !mount[separator + 3].split(',').contains("cpu"))) continue;
        for (const QByteArray &group : groups) {
            const QList<QByteArray> fields = group.split(':');
            if (fields.size() != 3 || (unified ? !fields[1].isEmpty() : !fields[1].split(',').contains("cpu")))
                continue;
            QString root = unescape(mount[3]), mountpoint = unescape(mount[4]), member;
            if (!root.startsWith('/') || !mountpoint.startsWith('/')
                || !nameFromBytes(fields[2], &member) || !member.startsWith('/'))
                return cpus.first(1);
            root = QDir::cleanPath(root);
            mountpoint = QDir::cleanPath(mountpoint);
            member = QDir::cleanPath(member);
            QString relative;
            if (member == "/" || member == root) relative = QString();
            else if (root == "/") relative = member.mid(1);
            else if (member.startsWith(root + '/')) relative = member.mid(root.size() + 1);
            else continue;
            QString directory = QDir::cleanPath(mountpoint + '/' + relative);
            if (directory != mountpoint && !directory.startsWith(mountpoint + '/')) continue;
            mapped = true;
            for (;;) {
                QFile quotaFile(directory + (unified ? "/cpu.max" : "/cpu.cfs_quota_us"));
                if (quotaFile.exists()) {
                    if (!quotaFile.open(QIODevice::ReadOnly)) return cpus.first(1);
                    QList<QByteArray> values = quotaFile.readAll().simplified().split(' ');
                    if (!unified) {
                        QFile periodFile(directory + "/cpu.cfs_period_us");
                        if (!periodFile.open(QIODevice::ReadOnly)) return cpus.first(1);
                        values.append(periodFile.readAll().trimmed());
                    }
                    bool quotaOk = false, periodOk = false;
                    const qint64 quota = values.value(0).toLongLong(&quotaOk);
                    const qint64 period = values.value(1).toLongLong(&periodOk);
                    if (values.value(0) != "max" && !(!unified && quotaOk && quota == -1)) {
                        if (!quotaOk || !periodOk || quota <= 0 || period <= 0) return cpus.first(1);
                        limit = qMin(limit, int(qBound(qint64(1), quota / period, qint64(limit))));
                    }
                }
                if (directory == mountpoint) break;
                directory = QFileInfo(directory).path();
            }
        }
    }
    return cpus.first(mapped ? limit : 1);
}

}
