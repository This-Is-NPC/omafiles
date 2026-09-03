OMAFILES_PROJECT_DIR = $$PWD
OMAFILES_SOURCE_ROOT = $$clean_path($$PWD/../..)
include($$PWD/../../qmake/layout.pri)

QT       += core
QT       -= gui
CONFIG   += c++17 console
CONFIG   -= app_bundle
TEMPLATE  = app
TARGET    = omafiles

DESTDIR = $$OUT_PWD/../../bin

INCLUDEPATH += $$PWD/../core
SOURCES     += main.cpp

LIBS           += -L$$OUT_PWD/../core -lomafilescore
PRE_TARGETDEPS += $$OUT_PWD/../core/libomafilescore.a
