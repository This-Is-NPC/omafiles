OMAFILES_PROJECT_DIR = $$PWD
OMAFILES_SOURCE_ROOT = $$clean_path($$PWD/..)
include($$PWD/../qmake/layout.pri)

QT += core gui quick testlib
CONFIG += testcase console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = tst_benchmark_probe
SOURCES += $$PWD/tst_benchmark_probe.cpp
INCLUDEPATH += $$PWD/../scripts/benchmark
LIBS += -ldl
