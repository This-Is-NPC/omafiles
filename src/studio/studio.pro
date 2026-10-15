OMAFILES_PROJECT_DIR = $$PWD
OMAFILES_SOURCE_ROOT = $$clean_path($$PWD/../..)
include($$PWD/../../qmake/layout.pri)

QT       += core gui qml quick dbus concurrent
CONFIG   += qtquickcompiler c++17 qmltypes

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

HEADERS += PointerActivity.h

HEADERS += DirectoryListing.h  DefaultManager.h FileManager.h FileMetadata.h Theme.h IndexModel.h Workspace.h GraphItem.h FolderModel.h EntryWatcher.h Clipboard.h FileTransfer.h Verb.h Views.h
SOURCES += DirectoryListing.cpp  DefaultManager.cpp FileManager.cpp main.cpp Theme.cpp IndexModel.cpp Workspace.cpp GraphItem.cpp FolderModel.cpp EntryWatcher.cpp \
           Clipboard.cpp FileTransfer.cpp Verb.cpp Views.cpp
RESOURCES += resources.qrc

LIBS           += -L$$OUT_PWD/../core -lomafilescore
PRE_TARGETDEPS += $$OUT_PWD/../core/libomafilescore.a

HEADERS += Thumbnail.h
SOURCES += Thumbnail.cpp
HEADERS += Preview.h HelperProcess.h ../preview/Protocol.h
SOURCES += Preview.cpp HelperProcess.cpp

LIBS += -lz

HEADERS += UndoJournal.h
SOURCES += UndoJournal.cpp

include($$PWD/../../qmake/qmlcache.pri)

HEADERS += TrashModel.h
SOURCES += TrashModel.cpp

HEADERS += ArchiveJob.h
SOURCES += ArchiveJob.cpp

HEADERS += Devices.h
SOURCES += Devices.cpp

CONFIG += link_pkgconfig
PKGCONFIG += KF6WindowSystem
HEADERS += Portal.h
SOURCES += Portal.cpp

HEADERS += DirectoryWork.h
SOURCES += DirectoryWork.cpp
