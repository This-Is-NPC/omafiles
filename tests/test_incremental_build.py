#!/usr/bin/env python3
"""Exercise the build entry point with real incremental Qt Quick resources."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class IncrementalBuildTest(unittest.TestCase):
    def test_add_remove_and_rename_qml_without_cleaning(self):
        with tempfile.TemporaryDirectory(prefix="omafiles-incremental-") as temporary:
            root = Path(temporary)
            env = os.environ.copy()
            for variable in ("HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME",
                             "XDG_STATE_HOME", "XDG_RUNTIME_DIR", "TMPDIR"):
                directory = root / variable
                directory.mkdir(mode=0o700)
                env[variable] = str(directory)
            env.update(QT_QPA_PLATFORM="offscreen", QT_QPA_PLATFORMTHEME="",
                       QML_DISABLE_DISK_CACHE="1",
                       DBUS_SESSION_BUS_ADDRESS="unix:path=" + str(root / "no-bus"))
            scripts = root / "scripts"
            scripts.mkdir()
            shutil.copy2(ROOT / "scripts/build.sh", scripts / "build.sh")
            (scripts / "deps.sh").write_text("#!/bin/sh\nexit 0\n")
            (scripts / "deps.sh").chmod(0o755)
            (root / "qmake").mkdir()
            shutil.copy2(ROOT / "qmake/qmlcache.pri", root / "qmake/qmlcache.pri")
            studio = root / "src/studio"
            studio.mkdir(parents=True)
            (root / "omafiles.pro").write_text("TEMPLATE = subdirs\nSUBDIRS = src/studio\n")
            (studio / "main.cpp").write_text('''#include <QCoreApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QScopedPointer>
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl(QString::fromUtf8(argv[1])));
    if (QString::fromUtf8(argv[2]) == "missing") return component.isError() ? 0 : 1;
    QScopedPointer<QObject> object(component.create());
    return object && object->property("marker").toString() == QString::fromUtf8(argv[2]) ? 0 : 1;
}
''')
            (studio / "studio.pro").write_text('''QT += qml
CONFIG += qtquickcompiler console c++17
TARGET = omafiles-studio
DESTDIR = ../../bin
OBJECTS_DIR = .qmake/obj
SOURCES += $$PWD/main.cpp
RESOURCES += $$PWD/resources.qrc
include(../../qmake/qmlcache.pri)
''')
            tests = root / "tests"
            tests.mkdir()
            (tests / "studio.pro").write_text('''include(../src/studio/studio.pro)
TARGET = tst_studio
DESTDIR = .
''')
            test_build = root / "build-tests/studio"
            test_build.mkdir(parents=True)

            def run(command, cwd=root):
                result = subprocess.run(command, cwd=cwd, env=env, text=True,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                self.assertEqual(result.returncode, 0, f"{command}\n{result.stdout}")

            def resources(names, prefix="qml", aliases=None):
                aliases = aliases or {}
                (studio / "resources.qrc").write_text(f'<RCC><qresource prefix="/{prefix}">'
                    + "".join(f'<file alias="{aliases.get(name, name)}.qml">{name}.qml</file>' for name in names)
                    + "</qresource></RCC>\n")

            def build():
                run([str(scripts / "build.sh")])
                run(["qmake6", str(tests / "studio.pro")], test_build)
                run(["make", "-j2"], test_build)

            def check(name, marker, prefix="qml"):
                for binary in (root / "build/bin/omafiles-studio", test_build / "tst_studio"):
                    run([str(binary), f"qrc:/{prefix}/{name}.qml", marker])

            for name in ("Base", "Added"):
                (studio / f"{name}.qml").write_text(
                    f'import QtQml\nQtObject {{ property string marker: "{name}" }}\n')
            resources(["Base"])
            build()
            check("Base", "Base")
            objects = [root / "build/src/studio/.qmake/obj/main.o",
                       test_build / ".qmake/obj/main.o"]
            stamps = [path.stat().st_mtime_ns for path in objects]
            resources(["Base", "Added"])
            build()
            check("Added", "Added")
            build()
            check("Added", "Added")
            (studio / "Added.qml").rename(studio / "Renamed.qml")
            resources(["Base", "Renamed"])
            build()
            check("Renamed", "Added")
            check("Added", "missing")
            resources(["Base", "Renamed"], "renamed", {"Renamed": "Alias"})
            build()
            check("Alias", "Added", "renamed")
            check("Renamed", "missing")
            resources(["Base", "Renamed"])
            build()
            check("Renamed", "Added")
            resources(["Base"])
            (studio / "Renamed.qml").unlink()
            build()
            check("Renamed", "missing")
            build()
            check("Base", "Base")
            self.assertEqual(stamps, [path.stat().st_mtime_ns for path in objects],
                             "Unchanged C++ objects must survive incremental resource updates")


if __name__ == "__main__":
    unittest.main()
