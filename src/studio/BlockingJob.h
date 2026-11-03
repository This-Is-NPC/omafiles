#pragma once

#include <QScopeGuard>
#include <QThreadPool>
#include <QtConcurrentRun>

#include <utility>

namespace omafiles {

// Runs work that can wait in the kernel, such as a filesystem call on an
// unresponsive mount, on the global pool. The job gives back its pool slot until
// it returns, so the pool starts another thread for queued jobs instead of
// leaving them behind it.
template <typename Work>
auto runBlocking(Work &&work)
{
    return QtConcurrent::run([work = std::forward<Work>(work)]() mutable {
        auto *pool = QThreadPool::globalInstance();
        pool->releaseThread();
        const auto reserve = qScopeGuard([pool] { pool->reserveThread(); });
        return work();
    });
}

} // namespace omafiles
