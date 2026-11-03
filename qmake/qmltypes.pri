# Qt's metatypes for every module in QT, passed to qmltyperegistrar. Qt 6.12
# installs them as <module>_metatypes.json, without the build type that
# qmltypes.prf looks for; without them the generated .qmltypes declares Qt's
# own types (QQuickItem, int, QVariantList) as opaque and qmllint loses them.
# Include after QT and TARGET are final.
CONFIG += qmltypes

# qmltypes.prf hands the registrar <target>_metatypes.json, which moc collects
# during the build, and qmake warns "Failure to find" about an input that does
# not exist yet. It is seeded empty and dated 1970: every moc output is newer,
# so the collect rule still writes it.
omafiles_metatypes_json = $$OUT_PWD/$$lower($$basename(TARGET))_metatypes.json
!exists($$omafiles_metatypes_json): system(touch -d @0 $$shell_quote($$omafiles_metatypes_json))

omafiles_qt_modules = $$replace(QT, -private$, '')
omafiles_qt_modules = $$resolve_depends(omafiles_qt_modules, "QT.", ".depends" ".run_depends")
omafiles_qt_modules = $$unique($$list($$replace(omafiles_qt_modules, _private$, "")))
for(module, omafiles_qt_modules) {
    metatypes = $$[QT_INSTALL_ARCHDATA]/metatypes/$$lower($$eval(QT.$${module}.module))_metatypes.json
    exists($$metatypes): QML_FOREIGN_METATYPES += $$metatypes
}
# The registrar's flags come from this file; a build made without them reruns.
qmltyperegistrar_compiler.depends += $$PWD/qmltypes.pri
