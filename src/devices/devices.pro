OMAFILES_PROJECT_DIR = $$PWD
OMAFILES_SOURCE_ROOT = $$clean_path($$PWD/../..)
include($$PWD/../../qmake/layout.pri)
QT =
CONFIG += console c++17 link_pkgconfig
CONFIG -= app_bundle
TEMPLATE = app
TARGET = omafiles-devices
DESTDIR = $$OUT_PWD/../../bin
PKGCONFIG += gio-2.0 gio-unix-2.0
SOURCES += main.cpp
