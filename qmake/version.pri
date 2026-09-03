# The one place the version is written.
#
# It used to be five: three in the CLI's main.cpp alone, one in the studio's,
# and one in omafiles.usage.kdl. Four of those are C++ and can share a define;
# the fifth feeds `usage` and has to stay a literal, so a test asserts that
# `omafiles --version` and the declaration still say the same thing.
OMAFILES_VERSION = 0.1.0
DEFINES += OMAFILES_VERSION=\\\"$$OMAFILES_VERSION\\\"
VERSION = $$OMAFILES_VERSION
