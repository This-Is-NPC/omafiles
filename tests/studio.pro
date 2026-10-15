# The studio's keyboard, tested without a screen.
#
# A second test binary rather than more cases in tst_omafiles: this one needs
# QtQuick, a QML engine and the studio's resources, and the store tests should
# not have to drag a window toolkit in behind them to check a jsonl file.
OMAFILES_PROJECT_DIR = $$PWD
OMAFILES_SOURCE_ROOT = $$clean_path($$PWD/..)
include($$PWD/../qmake/layout.pri)

QT       += core gui qml quick qmltest testlib dbus concurrent
CONFIG   += qtquickcompiler testcase c++17 console qmltypes

# The same module name and version the studio registers, or the test binary and
# the shipped one would disagree about what `import omafiles` means.
QML_IMPORT_NAME = omafiles
QML_IMPORT_MAJOR_VERSION = 1
CONFIG   -= app_bundle
TEMPLATE  = app
TARGET    = tst_studio

INCLUDEPATH += $$PWD/../src/core $$PWD/../src/studio

HEADERS += ../src/studio/PointerActivity.h

SOURCES += \
    tst_studio.cpp \
    ../src/studio/DirectoryListing.cpp \
    ../src/studio/DefaultManager.cpp \
    ../src/studio/FileManager.cpp \
    ../src/studio/Theme.cpp \
    ../src/studio/IndexModel.cpp \
    ../src/studio/Workspace.cpp \
    ../src/studio/GraphItem.cpp \
    ../src/studio/FolderModel.cpp \
    ../src/studio/EntryWatcher.cpp \
    ../src/studio/Clipboard.cpp \
    ../src/studio/FileTransfer.cpp \
    ../src/studio/Verb.cpp \
    ../src/studio/Views.cpp \
    ../src/core/View.cpp \
    ../src/core/Tag.cpp \
    ../src/core/Record.cpp \
    ../src/core/Store.cpp \
    ../src/core/Xattr.cpp \
    ../src/core/Paths.cpp \
    ../src/core/Roots.cpp \
    ../src/core/Names.cpp \
    ../src/core/Open.cpp \
    ../src/core/Recent.cpp \
    ../src/core/Glob.cpp

HEADERS += ../src/studio/DirectoryListing.h \
    ../src/studio/DefaultManager.h \
    ../src/studio/FileManager.h \
    ../src/studio/FileMetadata.h \
    ../src/studio/Theme.h \
    ../src/studio/IndexModel.h \
    ../src/studio/Workspace.h \
    ../src/studio/GraphItem.h \
    ../src/studio/FolderModel.h \
    ../src/studio/EntryWatcher.h \
    ../src/studio/Clipboard.h \
    ../src/studio/FileTransfer.h \
    ../src/studio/Verb.h \
    ../src/studio/Views.h

# The same qrc the studio ships, so the test drives the QML that runs.
RESOURCES += ../src/studio/resources.qrc

HEADERS += ../src/studio/Thumbnail.h
SOURCES += ../src/studio/Thumbnail.cpp
HEADERS += ../src/studio/Preview.h ../src/studio/HelperProcess.h ../src/preview/Protocol.h
SOURCES += ../src/studio/Preview.cpp ../src/studio/HelperProcess.cpp

LIBS += -lz

HEADERS += ../src/studio/UndoJournal.h
SOURCES += ../src/studio/UndoJournal.cpp

include($$PWD/../qmake/qmlcache.pri)

HEADERS += ../src/studio/TrashModel.h
SOURCES += ../src/studio/TrashModel.cpp
HEADERS += ../src/core/Trash.h
SOURCES += ../src/core/Trash.cpp

CONFIG += link_pkgconfig
PKGCONFIG += libarchive
HEADERS += ../src/studio/ArchiveJob.h
SOURCES += ../src/studio/ArchiveJob.cpp

HEADERS += ../src/studio/Devices.h
SOURCES += ../src/studio/Devices.cpp

CONFIG += link_pkgconfig
PKGCONFIG += KF6WindowSystem
HEADERS += ../src/studio/Portal.h
SOURCES += ../src/studio/Portal.cpp

HEADERS += ../src/core/CpuBudget.h ../src/studio/DirectoryWork.h
SOURCES += ../src/core/CpuBudget.cpp ../src/studio/DirectoryWork.cpp
