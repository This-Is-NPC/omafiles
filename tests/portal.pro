OMAFILES_PROJECT_DIR = $$PWD
OMAFILES_SOURCE_ROOT = $$clean_path($$PWD/..)
include($$PWD/../qmake/layout.pri)

QT += core gui dbus concurrent testlib
CONFIG += testcase console c++17 link_pkgconfig
CONFIG -= app_bundle
TEMPLATE = app
PKGCONFIG += KF6WindowSystem
TARGET = tst_portal
SOURCES += $$PWD/tst_portal.cpp $$PWD/../src/studio/Portal.cpp $$files($$PWD/../src/core/*.cpp)
HEADERS += $$PWD/../src/studio/Portal.h
INCLUDEPATH += $$PWD/../src/core $$PWD/../src/studio
