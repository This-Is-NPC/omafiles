OMAFILES_PROJECT_DIR = $$PWD
OMAFILES_SOURCE_ROOT = $$clean_path($$PWD/..)
include($$PWD/../qmake/layout.pri)

QT       += core testlib
QT       -= gui
CONFIG   += testcase c++17 console
CONFIG   -= app_bundle
TEMPLATE  = app
TARGET    = tst_omafiles

INCLUDEPATH += $$PWD/../src/core

SOURCES += \
    tst_store.cpp \
    ../src/core/Tag.cpp \
    ../src/core/Record.cpp \
    ../src/core/Store.cpp \
    ../src/core/Xattr.cpp \
    ../src/core/Paths.cpp \
    ../src/core/Roots.cpp

HEADERS += \
    ../src/core/Tag.h \
    ../src/core/Record.h \
    ../src/core/Store.h \
    ../src/core/Xattr.h \
    ../src/core/Paths.h \
    ../src/core/Roots.h
