#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QTimer>

int main(int argc, char **argv) {
    QGuiApplication application(argc, argv);
    QQmlApplicationEngine engine;
    const auto createWindow = [&] {
        engine.loadData(R"(
import QtQuick
Window {
    width: 1180; height: 720; visible: true
    Item {
        anchors.fill: parent; focus: true
        Keys.onPressed: event => {
            if (event.text === "x") rows.model = 1
            if (event.key === Qt.Key_Escape) rows.model = 40
        }
        ListView {
            id: rows; anchors.fill: parent; model: 40
            delegate: Text { required property int index; text: "row-" + index; height: 40 }
        }
    }
}
)");
        if (engine.rootObjects().isEmpty()) application.exit(2);
    };
    if (application.arguments().contains("--delayed-window")) QTimer::singleShot(0, &application, createWindow);
    else createWindow();
    return application.exec();
}
