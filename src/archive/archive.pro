OMAFILES_PROJECT_DIR = $$PWD
OMAFILES_SOURCE_ROOT = $$clean_path($$PWD/../..)
include($$PWD/../../qmake/layout.pri)
QT = core
CONFIG += console c++17 link_pkgconfig
CONFIG -= app_bundle
TEMPLATE = app
TARGET = omafiles-archive
DESTDIR = $$OUT_PWD/../../bin
PKGCONFIG += libarchive
SOURCES += main.cpp ArchiveWorker.cpp
HEADERS += ArchiveWorker.h
