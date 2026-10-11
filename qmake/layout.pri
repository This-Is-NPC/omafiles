include($$PWD/version.pri)

# Keep generated compiler and Qt files inside the shadow build.
OMAFILES_OUTPUT_DIR = $$clean_path($$OUT_PWD)
OMAFILES_PROJECT_DIR = $$clean_path($$OMAFILES_PROJECT_DIR)
OMAFILES_SOURCE_ROOT = $$clean_path($$OMAFILES_SOURCE_ROOT)

equals(OMAFILES_OUTPUT_DIR, $$OMAFILES_SOURCE_ROOT)|equals(OMAFILES_OUTPUT_DIR, $$OMAFILES_PROJECT_DIR) {
    error("Building in the source tree is not supported. Use mise run build.")
}

OBJECTS_DIR = $$OUT_PWD/.qmake/obj
MOC_DIR = $$OUT_PWD/.qmake/moc
RCC_DIR = $$OUT_PWD/.qmake/rcc
UI_DIR = $$OUT_PWD/.qmake/ui

# Qt's headers as system headers, which is what CMake makes of an imported
# target: a warning inside one is Qt's, and the build reports only this code's.
QMAKE_CXXFLAGS += -isystem $$[QT_INSTALL_HEADERS]
