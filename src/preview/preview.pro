OMAFILES_PROJECT_DIR = $$PWD
OMAFILES_SOURCE_ROOT = $$clean_path($$PWD/../..)
include($$PWD/../../qmake/layout.pri)
QT += core gui
CONFIG += c++17
TEMPLATE = app
TARGET = omafiles-preview
DESTDIR = $$OUT_PWD/../../bin
SOURCES += main.cpp
HEADERS += Output.h Protocol.h
