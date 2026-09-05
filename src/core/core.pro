# The model, and nothing else. No window, no argument parsing.
OMAFILES_PROJECT_DIR = $$PWD
OMAFILES_SOURCE_ROOT = $$clean_path($$PWD/../..)
include($$PWD/../../qmake/layout.pri)

QT       += core
QT       -= gui
CONFIG   += c++17 staticlib
CONFIG   -= app_bundle
TEMPLATE  = lib
TARGET    = omafilescore

HEADERS += \
    Tag.h \
    Record.h \
    Store.h \
    Xattr.h \
    Paths.h \
    Roots.h \
    Open.h

SOURCES += \
    Tag.cpp \
    Record.cpp \
    Store.cpp \
    Xattr.cpp \
    Paths.cpp \
    Roots.cpp \
    Open.cpp
