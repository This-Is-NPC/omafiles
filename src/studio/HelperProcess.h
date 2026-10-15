#pragma once

#include <QProcess>
#include <optional>

namespace omafiles {
constexpr qint64 helperOutputLimit = 8 * 1024 * 1024;
void prepareHelper(QProcess &process, unsigned cpuSeconds, qint64 outputBytes = helperOutputLimit, std::optional<int> preferredCpu = std::nullopt);
qint64 helperResidentBytes(qint64 processId);
void killHelper(QProcess &process);
constexpr qint64 helperMemoryLimit = 512LL * 1024 * 1024;
}
