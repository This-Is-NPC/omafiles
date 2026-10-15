#include "DirectoryWork.h"
#include "../core/CpuBudget.h"

#include <sched.h>
#include <stdexcept>

namespace omafiles {

DirectoryWork::DirectoryWork(QVector<int> cpus)
{
    if (cpus.size() > 1) {
        cpus.resize(qMin(qsizetype(4), cpus.size()));
        m_cpus = std::move(cpus);
        m_pool = std::make_unique<QThreadPool>();
        m_pool->setMaxThreadCount(m_cpus.size());
    }
}

int DirectoryWork::acquireCpu() const
{
    unsigned active = m_activeCpus.load(std::memory_order_relaxed);
    int slot;
    do {
        slot = 0;
        while (slot < m_cpus.size() && (active & (1u << slot))) ++slot;
        if (slot == m_cpus.size())
            throw std::logic_error("Directory worker CPU budget exhausted");
    } while (!m_activeCpus.compare_exchange_weak(active, active | (1u << slot)));
    cpu_set_t target;
    CPU_ZERO(&target);
    CPU_SET(m_cpus.at(slot), &target);
    // Private workers use only budgeted CPUs; the kernel enforces live cpusets.
    ::sched_setaffinity(0, sizeof(target), &target);
    return slot;
}

void DirectoryWork::releaseCpu(int slot) const
{
    m_activeCpus.fetch_and(~(1u << slot));
}

DirectoryWork::Chunks DirectoryWork::chunks(qsizetype count, qsizetype minimum) const
{
    m_partitionPasses.fetch_add(1, std::memory_order_relaxed);
    Chunks result;
    result.size = int(qBound(qsizetype(1), count / minimum, qsizetype(workerLimit())));
    for (int part = 0; part < result.size; ++part)
        result.ranges[part] = {count * part / result.size, count * (part + 1) / result.size};
    return result;
}

std::shared_ptr<const DirectoryWork> directoryWork()
{
    static const auto work = std::make_shared<const DirectoryWork>(workerCpus());
    return work;
}

}
