# The one place the version is written, moved by release-please.
#
# The binaries read it from a generated header rather than a define: qmake
# rewrites a changed define into the Makefile, but make rebuilds nothing,
# because no source changed, and an incremental build keeps the old version.
# omafiles.usage.kdl feeds `usage` and stays a literal, so a test asserts that
# every copy and `omafiles --version` say the same thing.
OMAFILES_VERSION = 0.2.0 # x-release-please-version
VERSION = $$OMAFILES_VERSION

OMAFILES_VERSION_HEADER = $$OUT_PWD/omafiles_version.h
omafiles_version_line = "$${LITERAL_HASH}define OMAFILES_VERSION \"$$OMAFILES_VERSION\""
omafiles_version_had = $$cat($$OMAFILES_VERSION_HEADER, lines)
!equals(omafiles_version_had, $$omafiles_version_line) {
    write_file($$OMAFILES_VERSION_HEADER, omafiles_version_line)|error("cannot write $$OMAFILES_VERSION_HEADER")
}
INCLUDEPATH += $$OUT_PWD
