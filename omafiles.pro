# One core, two front ends. `core` is a static library with the store and no
# opinion about how it is driven; `cli` and `studio` are both thin things on
# top of it.
OMAFILES_PROJECT_DIR = $$PWD
OMAFILES_SOURCE_ROOT = $$PWD
include($$PWD/qmake/layout.pri)

TEMPLATE = subdirs
CONFIG  += ordered

SUBDIRS = \
    src/core \
    src/cli \
    src/studio

core.subdir   = src/core
cli.subdir    = src/cli
cli.depends   = core
studio.subdir = src/studio
studio.depends = core
