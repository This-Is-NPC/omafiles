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
    /// The last press came from a pointer rather than a key: the focus ring
    /// stays hidden until a key is pressed.
    Q_PROPERTY(bool pointing READ pointing NOTIFY pointingChanged)

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
    bool pointing() const { return m_pointing; }

signals:
    void observedChanged();
    void pointingChanged();

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::MouseMove:
        case QEvent::TabletMove:
            if (!m_observed) {
                m_observed = true;
                emit observedChanged();
            }
            break;
        case QEvent::MouseButtonPress:
        case QEvent::TabletPress:
        case QEvent::TouchBegin:
            setPointing(true);
            break;
        case QEvent::KeyPress:
            setPointing(false);
            break;
        default:
            break;
        }
        return false;
    }

private:
    void setPointing(bool pointing)
    {
        if (m_pointing == pointing) return;
        m_pointing = pointing;
        emit pointingChanged();
    }

    bool m_observed = false;
    bool m_pointing = false;
};

} // namespace omafiles

QML_DECLARE_TYPEINFO(omafiles::PointerActivity, QML_HAS_ATTACHED_PROPERTIES)
