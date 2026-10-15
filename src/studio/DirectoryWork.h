#pragma once

#include <QMutex>
#include <QMutexLocker>
#include <QRunnable>
#include <QSemaphore>
#include <QScopeGuard>
#include <QVector>
#include <QThreadPool>

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <memory>

namespace omafiles {

using DirectoryCancellation = std::shared_ptr<std::atomic_bool>;

inline bool directoryCancelled(const DirectoryCancellation &cancellation)
{
    return cancellation && cancellation->load(std::memory_order_relaxed);
}

class DirectoryWork
{
public:
    static constexpr qsizetype minimumChunkSize = 32768;
    static constexpr qsizetype minimumFilterChunkSize = 65536;
    struct Range { qsizetype first = 0; qsizetype last = 0; };
    struct Chunks { std::array<Range, 4> ranges{}; int size = 0; };

    explicit DirectoryWork(QVector<int> cpus);
    int workerLimit() const { return m_pool ? m_pool->maxThreadCount() : 1; }
    Chunks chunks(qsizetype count, qsizetype minimum = minimumChunkSize) const;
    unsigned partitionPasses() const { return m_partitionPasses.load(std::memory_order_relaxed); }

    template <typename Work>
    void run(const Chunks &chunks, Work &&work,
             const DirectoryCancellation &cancellation = {}) const
    {
        if (chunks.size == 1) {
            if (!directoryCancelled(cancellation))
                work(0, chunks.ranges[0].first, chunks.ranges[0].last);
            return;
        }
        QSemaphore finished;
        QMutex failureMutex;
        std::exception_ptr failure;
        for (int part = 0; part < chunks.size; ++part) {
            m_pool->start(QRunnable::create([&, part] {
                try {
                    if (!directoryCancelled(cancellation)) {
                        const int slot = acquireCpu();
                        const auto release = qScopeGuard([&] { releaseCpu(slot); });
                        if (!directoryCancelled(cancellation))
                            work(part, chunks.ranges[part].first, chunks.ranges[part].last);
                    }
                } catch (...) {
                    QMutexLocker lock(&failureMutex);
                    if (!failure) failure = std::current_exception();
                }
                finished.release();
            }));
        }
        finished.acquire(chunks.size);
        if (failure) std::rethrow_exception(failure);
    }

    template <typename Iterator, typename Compare>
    bool sort(Iterator first, Iterator last, Compare compare,
              const DirectoryCancellation &cancellation = {}) const
    {
        if (directoryCancelled(cancellation)) return false;
        if (first == last) return true;
        const Chunks parts = chunks(last - first);
        run(parts, [&](int, qsizetype begin, qsizetype end) {
            std::sort(first + begin, first + end, compare);
        }, cancellation);
        for (int width = 1; width < parts.size; width *= 2) {
            if (directoryCancelled(cancellation)) return false;
            Chunks merges;
            for (int begin = 0; begin + width < parts.size; begin += 2 * width)
                merges.ranges[merges.size++] = {begin, qMin(begin + 2 * width, parts.size)};
            run(merges, [&](int, qsizetype begin, qsizetype end) {
                const auto middle = first + parts.ranges[begin + width].first;
                if (!compare(*middle, *(middle - 1))) return;
                std::inplace_merge(first + parts.ranges[begin].first, middle,
                                   first + parts.ranges[end - 1].last, compare);
            }, cancellation);
        }
        return !directoryCancelled(cancellation);
    }

private:
    int acquireCpu() const;
    void releaseCpu(int slot) const;
    QVector<int> m_cpus;
    mutable std::atomic_uint m_activeCpus{0};
    mutable std::atomic_uint m_partitionPasses{0};
    std::unique_ptr<QThreadPool> m_pool;
};

std::shared_ptr<const DirectoryWork> directoryWork();

}
