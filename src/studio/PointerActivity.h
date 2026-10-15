#pragma once

#include <QEvent>
#include <QObject>
#include <QWindow>
#include <QtQml/qqml.h>

namespace omafiles {

class PointerActivity : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("PointerActivity is attached to a window")
    QML_ATTACHED(PointerActivity)
    Q_PROPERTY(bool observed READ observed NOTIFY observedChanged)

public:
    explicit PointerActivity(QObject *parent = nullptr) : QObject(parent)
    {
        if (qobject_cast<QWindow *>(parent)) parent->installEventFilter(this);
    }

    static PointerActivity *qmlAttachedProperties(QObject *object)
    {
        return new PointerActivity(object);
    }

    bool observed() const { return m_observed; }

signals:
    void observedChanged();

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() == QEvent::MouseMove || event->type() == QEvent::TabletMove) {
            m_observed = true;
            object->removeEventFilter(this);
            emit observedChanged();
        }
        return false;
    }

private:
    bool m_observed = false;
};

} // namespace omafiles

QML_DECLARE_TYPEINFO(omafiles::PointerActivity, QML_HAS_ATTACHED_PROPERTIES)
