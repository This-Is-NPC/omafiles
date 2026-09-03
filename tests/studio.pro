# The studio's keyboard, tested without a screen.
#
# A second test binary rather than more cases in tst_omafiles: this one needs
# QtQuick, a QML engine and the studio's resources, and the store tests should
# not have to drag a window toolkit in behind them to check a jsonl file.
OMAFILES_PROJECT_DIR = $$PWD
OMAFILES_SOURCE_ROOT = $$clean_path($$PWD/..)
include($$PWD/../qmake/layout.pri)

QT       += core gui qml quick testlib
CONFIG   += testcase c++17 console qmltypes

# The same module name and version the studio registers, or the test binary and
# the shipped one would disagree about what `import omafiles` means.
QML_IMPORT_NAME = omafiles
QML_IMPORT_MAJOR_VERSION = 1
CONFIG   -= app_bundle
TEMPLATE  = app
TARGET    = tst_studio

INCLUDEPATH += $$PWD/../src/core $$PWD/../src/studio

SOURCES += \
    tst_studio.cpp \
    ../src/studio/Theme.cpp \
    ../src/studio/IndexModel.cpp \
    ../src/studio/Workspace.cpp \
    ../src/studio/GraphItem.cpp \
    ../src/studio/FolderModel.cpp \
    ../src/core/Tag.cpp \
    ../src/core/Record.cpp \
    ../src/core/Store.cpp \
    ../src/core/Xattr.cpp \
    ../src/core/Paths.cpp \
    ../src/core/Open.cpp

HEADERS += \
    ../src/studio/Theme.h \
    ../src/studio/IndexModel.h \
    ../src/studio/Workspace.h \
    ../src/studio/GraphItem.h \
    ../src/studio/FolderModel.h

# The same qrc the studio ships, so the test drives the QML that runs.
RESOURCES += ../src/studio/resources.qrc
