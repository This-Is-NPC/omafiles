#include "HelperProcess.h"
#include "Paths.h"
#include <QFile>
#include <QSet>
#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

namespace omafiles {
qint64 helperResidentBytes(qint64 processId)
{
    QList<qint64> pending{processId};
    QSet<qint64> seen;
    qint64 total = 0;
    const qint64 page = ::sysconf(_SC_PAGESIZE);
    while (!pending.isEmpty()) {
        const qint64 pid = pending.takeLast();
        if (pid <= 0 || seen.contains(pid)) continue;
        seen.insert(pid);
        const QString base = QStringLiteral("/proc/%1/").arg(pid);
        QFile memory(base + "statm");
        if (openToRead(&memory, QIODevice::ReadOnly, nullptr) == PathState::Present)
            total += memory.readAll().simplified().split(' ').value(1).toLongLong() * page;
        QFile children(base + QStringLiteral("task/%1/children").arg(pid));
        if (openToRead(&children, QIODevice::ReadOnly, nullptr) == PathState::Present)
            for (const QByteArray &child : children.readAll().simplified().split(' '))
                if (const qint64 id = child.toLongLong(); id > 0) pending.append(id);
    }
    return total;
}

void prepareHelper(QProcess &process, unsigned cpuSeconds, qint64 outputBytes, std::optional<int> preferredCpu)
{
    process.setStandardErrorFile(QProcess::nullDevice());
    process.setChildProcessModifier([cpuSeconds, outputBytes, preferredCpu] {
        ::setsid();
        cpu_set_t allowed;
        if (preferredCpu && ::sched_getaffinity(0, sizeof(allowed), &allowed) == 0) {
            int cpu = *preferredCpu >= 0 && *preferredCpu < CPU_SETSIZE
                && CPU_ISSET(*preferredCpu, &allowed) ? *preferredCpu : -1;
            if (cpu < 0)
                for (int candidate = 0; candidate < CPU_SETSIZE; ++candidate)
                    if (CPU_ISSET(candidate, &allowed)) { cpu = candidate; break; }
            if (cpu >= 0) {
                cpu_set_t single;
                CPU_ZERO(&single);
                CPU_SET(cpu, &single);
                ::sched_setaffinity(0, sizeof(single), &single);
            }
        }
        if (cpuSeconds) {
            const rlimit cpu{cpuSeconds, cpuSeconds};
            ::setrlimit(RLIMIT_CPU, &cpu);
        }
        if (outputBytes > 0) {
            const rlimit output{rlim_t(outputBytes), rlim_t(outputBytes)};
            ::setrlimit(RLIMIT_FSIZE, &output);
        }
        const rlimit core{0, 0};
        ::setrlimit(RLIMIT_CORE, &core);
    });
}

void killHelper(QProcess &process)
{
    if (process.processId() > 0)
        ::kill(-pid_t(process.processId()), SIGKILL);
    process.kill();
}
}
