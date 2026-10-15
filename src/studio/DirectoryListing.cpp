#include "DirectoryListing.h"
#include "DirectoryWork.h"

#include <QFileInfo>

#include <algorithm>
#include <array>
#include <dirent.h>
#include <cstring>
#include <type_traits>

namespace omafiles {
namespace {
constexpr qsizetype nameBlockSize = 64 * 1024;

template <typename String, typename View, typename Append>
quint32 appendName(QVector<String> &blocks, View name, Append append)
{
    if (blocks.isEmpty() || blocks.last().size() + name.size() + 1 > nameBlockSize) {
        blocks.append(String());
        blocks.last().reserve(nameBlockSize);
    }
    String &block = blocks.last();
    const quint32 offset = quint32((blocks.size() - 1) * nameBlockSize + block.size());
    append(block, name);
    return offset;
}

// Qt's case-insensitive order restricted to ASCII, which compact names are.
constexpr auto asciiLower = [] {
    std::array<unsigned char, 256> table{};
    for (unsigned i = 0; i < table.size(); ++i)
        table[i] = i - 'A' < 26u ? (i | 0x20u) : i;
    return table;
}();

int compareAscii(QLatin1StringView a, QLatin1StringView b)
{
    const auto *left = reinterpret_cast<const unsigned char *>(a.data());
    const auto *right = reinterpret_cast<const unsigned char *>(b.data());
    const qsizetype size = std::min(a.size(), b.size());
    for (qsizetype i = 0; i < size; ++i) {
        if (left[i] != right[i]) {
            if (const int difference = int(asciiLower[left[i]]) - int(asciiLower[right[i]]))
                return difference;
        }
    }
    return int(a.size() - b.size());
}
}

DirectoryListing DirectoryListing::read(const QString &directory,
                                        const std::shared_ptr<std::atomic_bool> &cancellation,
                                        const std::shared_ptr<const DirectoryWork> &work)
{
    DirectoryListing result;
    result.directory = directory;
    const auto cancelled = [&] {
        return cancellation && cancellation->load(std::memory_order_relaxed);
    };
    if (cancelled()) {
        result.cancelled = true;
        return result;
    }

    const QString prefix = directory.endsWith(QLatin1Char('/'))
                               ? directory : directory + QLatin1Char('/');
    const bool complete = visitDirectory(directory, [&](QAnyStringView name, unsigned char type) {
        if (cancelled()) {
            result.cancelled = true;
            return false;
        }
        const bool compact = name.visit([](auto text) {
            return std::is_same_v<decltype(text), QLatin1StringView>;
        });
        const qsizetype blocks = compact ? result.m_latin1Names.size() : result.m_names.size();
        if (name.size() > 255
            || quint64(blocks) * nameBlockSize + quint64(name.size())
            >= (quint64(1) << 29)) {
            result.error = QStringLiteral("Directory names exceed the listing capacity");
            result.state = PathState::Unknown;
            return false;
        }
        quint32 flags = type == DT_DIR ? quint32(Directory) : 0;
        if (type == DT_LNK || type == DT_UNKNOWN) {
            ++result.metadataReads;
            const QFileInfo info(prefix + name.toString());
            if (info.isDir())
                flags |= Directory;
            if (type == DT_LNK || info.isSymbolicLink())
                flags |= Link;
        }
        quint32 offset;
        if (compact) {
            flags |= Latin1;
            const QLatin1StringView text(static_cast<const char *>(name.data()), name.size());
            offset = appendName(result.m_latin1Names, text, [](QByteArray &block, QLatin1StringView text) {
                const qsizetype start = block.size();
                block.resize(start + text.size() + 1);
                char *destination = block.data() + start;
                *destination = char(text.size());
                std::memcpy(destination + 1, text.data(), size_t(text.size()));
            });
        } else {
            const QStringView text(static_cast<const QChar *>(name.data()), name.size());
            offset = appendName(result.m_names, text, [](QString &block, QStringView text) {
                block.append(QChar(ushort(text.size())));
                block.append(text);
            });
        }
        result.hasHiddenNames = result.hasHiddenNames || name.front() == QLatin1Char('.');
        result.m_spans.append(Span{offset | (flags << 29)});
        return true;
    }, &result.unnameable, &result.error, &result.state);

    result.cancelled = result.cancelled || cancelled();
    if (complete && result.error.isEmpty() && !result.cancelled) {
        const auto executor = work ? work : directoryWork();
        executor->sort(result.m_spans.begin(), result.m_spans.end(), [&](const Span &a, const Span &b) {
            const bool aDirectory = a.type() & Directory;
            const bool bDirectory = b.type() & Directory;
            if (aDirectory != bDirectory)
                return aDirectory;
            if (a.type() & b.type() & Latin1)
                return compareAscii(result.latin1Name(a), result.latin1Name(b)) < 0;
            return QAnyStringView::compare(result.name(a), result.name(b), Qt::CaseInsensitive) < 0;
        }, cancellation);
        result.cancelled = cancelled();
    }
    if (!complete || !result.error.isEmpty() || result.cancelled) {
        result.m_names.clear();
        result.m_latin1Names.clear();
        result.m_spans.clear();
    }
    return result;
}

QAnyStringView DirectoryListing::name(qsizetype index) const
{
    return name(m_spans.at(index));
}

QAnyStringView DirectoryListing::name(const Span &span) const
{
    if (span.type() & Latin1)
        return latin1Name(span);
    return unicodeName(span);
}

QLatin1StringView DirectoryListing::latin1Name(const Span &span) const
{
    const char *name = m_latin1Names.at(span.offset() / nameBlockSize).constData()
        + span.offset() % nameBlockSize;
    return QLatin1StringView(name + 1, uchar(*name));
}

QStringView DirectoryListing::unicodeName(const Span &span) const
{
    const QChar *name = m_names.at(span.offset() / nameBlockSize).constData()
        + span.offset() % nameBlockSize;
    return QStringView(name + 1, name->unicode());
}

DirectoryNameQuery::DirectoryNameQuery(const QString &needle)
    : m_needle(needle),
      m_ascii(std::all_of(needle.cbegin(), needle.cend(),
                         [](QChar ch) { return ch.unicode() <= 0x7f; })),
      m_pattern(m_ascii ? needle.toLatin1() : QByteArray()),
      m_latin1(QLatin1StringView(m_pattern), Qt::CaseInsensitive)
{
}

bool DirectoryNameQuery::contains(QLatin1StringView name) const
{
    if (m_ascii) return m_latin1.indexIn(name) >= 0;
    // The listing bounds names to 255 units; preserve Qt's Unicode folding.
    QChar unicode[255];
    for (qsizetype i = 0; i < name.size(); ++i)
        unicode[i] = QLatin1Char(name.at(i));
    return QStringView(unicode, name.size()).contains(QStringView(m_needle), Qt::CaseInsensitive);
}

bool DirectoryNameQuery::contains(QStringView name) const
{
    return name.contains(QStringView(m_needle), Qt::CaseInsensitive);
}

bool DirectoryListing::matches(qsizetype index, const DirectoryNameQuery &query,
                               bool showHidden) const
{
    const auto accepts = [&](auto view) {
        return (showHidden || !view.startsWith(QLatin1Char('.'))) && query.contains(view);
    };
    const Span &span = m_spans.at(index);
    return span.type() & Latin1 ? accepts(latin1Name(span)) : accepts(unicodeName(span));
}

qsizetype DirectoryListing::nameStorageCapacityBytes() const
{
    qsizetype bytes = 0;
    for (const QByteArray &block : m_latin1Names) bytes += block.capacity();
    for (const QString &block : m_names) bytes += block.capacity() * sizeof(QChar);
    return bytes;
}

qsizetype DirectoryListing::entryStorageBytes() const
{
    return m_spans.size() * sizeof(Span);
}

bool DirectoryListing::isDirectory(qsizetype index) const
{
    return m_spans.at(index).type() & Directory;
}

bool DirectoryListing::isLink(qsizetype index) const
{
    return m_spans.at(index).type() & Link;
}

} // namespace omafiles
