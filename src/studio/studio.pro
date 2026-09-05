OMAFILES_PROJECT_DIR = $$PWD
OMAFILES_SOURCE_ROOT = $$clean_path($$PWD/../..)
include($$PWD/../../qmake/layout.pri)

QT       += core gui qml quick
CONFIG   += c++17 qmltypes

# So the QML types this ships are registered by the build rather than by a line
# in main(), and so `qmllint` can see them: a type registered in code is
# invisible to it, and every use of one in Main.qml read as a failed import.
QML_IMPORT_NAME = omafiles
QML_IMPORT_MAJOR_VERSION = 1
CONFIG   -= app_bundle
TEMPLATE  = app
TARGET    = omafiles-studio

DESTDIR = $$OUT_PWD/../../bin

INCLUDEPATH += $$PWD/../core

HEADERS += Theme.h IndexModel.h Workspace.h GraphItem.h FolderModel.h Clipboard.h Verb.h
SOURCES += main.cpp Theme.cpp IndexModel.cpp Workspace.cpp GraphItem.cpp FolderModel.cpp \
           Clipboard.cpp Verb.cpp
RESOURCES += resources.qrc

LIBS           += -L$$OUT_PWD/../core -lomafilescore
PRE_TARGETDEPS += $$OUT_PWD/../core/libomafilescore.a
