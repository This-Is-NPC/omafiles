OMAFILES_PROJECT_DIR = $$PWD
OMAFILES_SOURCE_ROOT = $$clean_path($$PWD/../..)
include($$PWD/../../qmake/layout.pri)
QT += core gui pdf multimedia
CONFIG += c++17 link_pkgconfig
PKGCONFIG += libarchive
TEMPLATE = app
TARGET = omafiles-preview-codecs
DESTDIR = $$OUT_PWD/../../bin
SOURCES += main.cpp
HEADERS += ../preview/Output.h ../preview/Protocol.h
