#pragma once

#include "../core/Paths.h"

#include <QAnyStringView>
#include <QByteArray>
#include <QStringView>
#include <QVector>

#include <atomic>
#include <memory>

namespace omafiles {

class DirectoryWork;

class DirectoryNameQuery
{
public:
    explicit DirectoryNameQuery(const QString &needle);
    DirectoryNameQuery(const DirectoryNameQuery &) = delete;
    DirectoryNameQuery &operator=(const DirectoryNameQuery &) = delete;
private:
    friend class DirectoryListing;
    bool contains(QLatin1StringView name) const;
    bool contains(QStringView name) const;
    QString m_needle;
    bool m_ascii;
    /// The ASCII needle in lower case.
    QByteArray m_pattern;
};

class DirectoryListing
{
public:
    static DirectoryListing read(const QString &directory,
                                 const std::shared_ptr<std::atomic_bool> &cancellation = {},
                                 const std::shared_ptr<const DirectoryWork> &work = {});

    qsizetype size() const { return m_spans.size(); }
    QAnyStringView name(qsizetype index) const;
    bool matches(qsizetype index, const DirectoryNameQuery &query, bool showHidden) const;
    /// Starts loading the name of `index` into the cache.
    void prefetch(qsizetype index) const
    {
        const quint32 offset = m_spans[index].offset();
        if (m_spans[index].type() & Latin1)
            __builtin_prefetch(m_latin1Names[offset / nameBlockSize].constData() + offset % nameBlockSize);
        else
            __builtin_prefetch(m_names[offset / nameBlockSize].constData() + offset % nameBlockSize);
    }
    bool isDirectory(qsizetype index) const;
    bool isLink(qsizetype index) const;
    qsizetype nameStorageCapacityBytes() const;
    qsizetype entryStorageBytes() const;

    static constexpr qsizetype nameBlockSize = 64 * 1024;

    QString directory;
    QString error;
    QStringList unnameable;
    PathState state = PathState::Present;
    bool cancelled = false;
    qsizetype metadataReads = 0;
    bool hasHiddenNames = false;

private:
    enum Type : quint32 { Directory = 1, Link = 2, Latin1 = 4 };
    struct Span {
        quint32 offsetAndType;

        quint32 offset() const { return offsetAndType & 0x1fffffff; }
        quint32 type() const { return offsetAndType >> 29; }
    };
    static_assert(sizeof(Span) == 4);
    QAnyStringView name(const Span &span) const;
    QLatin1StringView latin1Name(const Span &span) const;
    QStringView unicodeName(const Span &span) const;

    QVector<QString> m_names;
    QVector<QByteArray> m_latin1Names;
    QVector<Span> m_spans;
};

} // namespace omafiles
