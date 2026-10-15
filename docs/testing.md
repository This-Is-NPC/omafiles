# Asynchronous studio tests

Studio tests wait for completed work, not an estimate of how long a worker takes.
Qt's [test guidance](https://doc.qt.io/qt-6/qttest-best-practices.html#avoid-fixed-timeouts)
recommends `QTRY_VERIFY`, `QTRY_COMPARE`, or a checked signal wait for asynchronous results.
Their timeouts bound a failed test; elapsed time is not evidence of success.

The Studio suite sets QtTest's public
[`defaultTryTimeout`](https://doc.qt.io/qt-6/qtest.html#defaultTryTimeout-var)
to 30 seconds in `initTestCase()` (Qt 6.11 or later). All `QTRY_*` calls use
that shared failure guard. The observed loaded-machine playback acknowledgement
took 5.2 seconds; 30 seconds leaves scheduling headroom without adding a delay
to a successful test. Window exposure, semaphore handshakes and the private
D-Bus completion signal use the same guard. A missed completion still fails;
the whole-function QtTest watchdog also remains enabled.

The deadline audit covers every `QTRY_*` and `QTest::qWaitFor*` call in `tests/`:

| Wait family | Completion evidence | Failure guard |
| --- | --- | --- |
| Folder and Trash listing, watchers, metadata | Published rows, loading state, MIME and selected entry | 30 seconds |
| Index mutations, file operations, undo and conflicts | Finished signal, receipt and expected filesystem/model state | 30 seconds |
| Thumbnail queue, decoding and invalidation | Runner completion, readiness, painted pixels and GUI heartbeat | 30 seconds |
| Preview decoding, playback, seeking and cancellation | Decoded packet, acknowledged state, positive PID and process exit | 30 seconds |
| UI focus, layout, input and window exposure | Expected focus/control/model state or exposed window | 30 seconds |
| Worker/GUI and private D-Bus handshakes | Semaphore acquisition or finished signal | 30 seconds |

Store tests and Python tests contain no Qt conditional waits. QProcess waits
use 30 seconds, with 60 seconds for the nested test subprocess. Gesture/animation waits and the Store
lock test's lower-bound measurement exercise their respective contracts, not
background-work completion. Product helper memory, output, CPU and execution
budgets remain separate from these test guards.

`waitsForDelayedPlaybackAcknowledgement` uses a helper that acknowledges playback
after six seconds. The assertion waits for the actual acknowledgement and checks
the resulting state; it does not assert elapsed time. This controlled delay
exercises a completion later than QtTest's five-second default. The thumbnail
memory fixture can produce a valid image while its child holds more than the
resident-memory budget. The test requires rejection and no persistent failure
marker, so it checks resource enforcement without a wall-clock upper bound.

The shared `settle()` barrier waits for the active folder to finish loading and for
its focused entry to have a MIME type and match the published model entry. `focusRow()` also checks the
requested name before callers capture metadata. This covers listing, filtering,
sorting, navigation, marks, permissions, tags, details and accessibility reads.
`FolderModel::entryAt()` and `thumbnailFor()` return their results synchronously;
standalone model tests wait for `loading == false` before reading the published rows.
Tests of the loading state itself deliberately inspect it before completion.

The suite keeps one isolated MIME data directory alive for its entire lifetime.
Qt caches MIME providers and checks directory changes periodically; deleting a
provider directory between tests can invalidate subsequent MIME lookups. Thumbnail
fixtures isolate cache and thumbnailer descriptors without replacing that shared,
read-only MIME directory. See Qt's [MIME provider implementation](https://github.com/qt/qtbase/blob/v6.11.2/src/corelib/mimetypes/qmimedatabase.cpp).

Thumbnail tests wait for readiness, the expected painted color, runner completion,
and GUI heartbeat events. A failed thumbnail decode is observed through its completion
signal before checking that it has no image. Cancellation checks finish before fixture teardown;
a subsequent completed request proves the queue has progressed. Preview tests wait
for decoded packets, playback state and seek acknowledgements. A helper PID file is
ready when it contains a positive PID, not merely when it exists. A completed audio
packet declares paused playback; reopening a covered preview preserves that state.
UI playback and focus tests use a scripted helper whose acknowledged state persists
until the next command. They do not race a short clip's natural end against rendering
or input handling. Decoder integration tests exercise the real helper separately
and record the playback acknowledgement signal, which remains evidence even when
the clip has already ended before the next assertion.

Gesture durations simulate press-and-hold input. Hover timer checks, screenshot
animation settling and explicit helper resource/deadline contracts have distinct
purposes; they do not establish completion of directory or decoder work.

To exercise scheduling pressure, run the gate with CPU workers in a private scratch
area, record that they remain alive throughout the run, and stop only those workers
in a cleanup handler. Use fresh HOME, XDG and temporary directories, isolated D-Bus
and audio, software Qt rendering and the suite's launcher recorders. Keep builds and
fixtures under the project's temporary storage budget.

## Incremental QML builds

`scripts/build.sh` runs `qmake6 -r` for the `SUBDIRS` project before `make`.
Recursive generation refreshes each subproject's resource-derived object list.
Qt's `qtquickcompiler` reads the QRC during qmake to enumerate compiled QML,
while the generated loader depends on that QRC during make. Both must describe
the same resources when files are added, removed or renamed. The shared
`qmake/qmlcache.pri` also makes compiled QML units depend on the original QRC:
resource aliases and prefixes determine their generated C++ symbols. See Qt's
[SUBDIRS guidance](https://wiki.qt.io/SUBDIRS_-_handling_dependencies#Makefiles)
and [QML compilation documentation](https://doc.qt.io/qt-6/qmldiskcache.html).
The Studio test project runs qmake directly before its build as well.

`python3 tests/test_incremental_build.py` is part of the gate. It drives the
build entry point on a small real Qt Quick project, then adds, renames and removes
a QML resource without cleaning either shadow build. Both application and test
executables must link and load the expected compiled components; removed URLs
must be absent. Alias and prefix changes must update compiled symbols and URLs.
Repeated builds must succeed and preserve unchanged C++ objects.
The fixture and its HOME, XDG directories and QML cache live in temporary storage
and are removed on completion. Dependency-package checks run separately in the
gate; the small fixture substitutes an empty dependency-check script.


## Device services

`tests/device_bus.py` runs the shipped GIO helper with the real GVfs UDisks2
monitor on two private D-Bus buses. Its UDisks2 ObjectManager, block device,
filesystem and drive are fake. A test-only preload supplies a synthetic mount
table to that private GVfs process; it creates no block device and performs no
kernel mount. Phone volumes come from a fake GIO remote-volume monitor on the
same private session bus. HOME, XDG directories and runtime files are temporary.

The suite requires `dbus-daemon`, `gvfs`, `udisks2`'s interface XML and
`python-gobject`, alongside the normal C++ build tools. It never connects to the
machine's system or session bus. Client and window tests use a scripted helper
and the normal launcher recorder, so they cannot mount a disk or launch an
external viewer. All waits assert completed work or observed signals.


The device-task comparison used 100,000 empty files in an owned Docker volume,
three alternating runs per version, one CPU, 1 GiB RAM, no swap and no network.
Against commit `1ff7de7`, median first frame was 94.217 → 85.704 ms and wheel
latency 8.222 → 8.663 ms. Median peak GUI RSS was 96.613 → 97.434 MiB; the
process tree including the GIO helper was 96.613 → 111.820 MiB. Idle CPU was
0% in both versions; no OOM event occurred. The measured first frame improved; wheel latency increased by 0.441 ms.
Device discovery is deferred until after the first frame. The volume was removed after measuring.

## Validation environment

Run `scripts/check.sh` directly from the normal shell, as the Git hooks do.
The gate, standalone `scripts/test.sh` and documentation screenshot entry point
create disposable HOME, XDG, Cargo, Rustup and mise directories under `/tmp`
through `scripts/test-environment.py`. They replace the inherited session bus
with a private `dbus-daemon`, disconnect the system bus and audio, use software
offscreen rendering, and remove their scratch state on
success, failure or interruption. Scripts invoked inside an existing validation
run reuse its environment. Explicit runner invocations, including each C++
suite, create a fresh environment of their own.

The runner uses [dbus-run-session](https://dbus.freedesktop.org/doc/dbus-run-session.1.html)
inside the write-confined process group. Each invocation gets a fresh Unix socket
under its temporary home tree and a configuration with no host service directories
or systemd activation. It works when the caller has no session bus, an empty
address or an unusable address. Daemon startup failure stops validation before
the requested command runs; environments that prohibit Unix sockets must permit
the private test bus. Success, failure and interruption stop the owned daemon
with its child processes. Tests needing special activation or fake device services
can start additional private buses without using the real desktop bus.

The runner reads the exact `usage` version from the repository's `mise.toml`
and copies that already installed executable into temporary storage. Both
`scripts/usage-check.sh` and `scripts/usage-gen.sh` execute this copy directly;
they never invoke mise or merge its global or project tool configuration.
Missing pinned `usage` is an explicit refusal, with no installation or PATH
fallback. Install required tools separately before validation. The runner also
sets `MISE_NO_CONFIG=1` and `MISE_AUTO_INSTALL=false` for child processes.
Validation uses the system executable search path, excluding inherited mise
shims and user toolchain binaries. Portal tests retain their temporary-HOME guard
and use private D-Bus connections. The real portal configuration is untouched.

`tests/test_test_environment.py` checks inherited-state isolation, fresh runs,
exit-status propagation, usable distinct private buses, startup refusal, cleanup
after success, failure and interruption, and preservation of caller files.
The full gate also exercises the scripts from the hook entry point, including
with `DBUS_SESSION_BUS_ADDRESS` and `DBUS_SYSTEM_BUS_ADDRESS` unset.

Bubblewrap (`bwrap`) is required for validation. The runner exposes the host
filesystem read-only and grants writable mounts only to this repository and
`/tmp`. Private `/dev` and `/proc` provide process facilities and discard sinks;
they do not expose writable host devices. Namespace setup failure stops the
run. Tests verify creation, truncation, deletion, metadata changes, links,
renames and child-process inheritance, including an end-to-end runner check
using a fake caller directory. All fixtures stay under `/tmp`.

Every agent build, binary, gate or mise invocation must set fresh `HOME`,
`XDG_*_HOME`, `XDG_RUNTIME_DIR`, `TMPDIR`, `CARGO_HOME`, `RUSTUP_HOME` and the
`MISE_*` data/config/cache/state directories under `/tmp`. Use
`/usr/bin/python3 scripts/test-environment.py COMMAND ...` for isolated builds
and agent probes. Do not run repair, install or toolchain-update commands against
the user's real homes. Agent command runners use non-login shells
(`exec_command` with `login=false`) so shell startup hooks cannot resolve tools
before isolation. Invoke validation scripts directly from the normal
environment; wrapping them in `mise run` would let mise evaluate configuration
before the script can establish isolation.

The [mise exec documentation](https://mise.jdx.dev/cli/exec) explains that naming
one tool still loads other configured tools. Direct pinned execution avoids
that configuration evaluation entirely. The filesystem boundary follows
[bubblewrap's read-only bind-mount model](https://github.com/containers/bubblewrap).

## Status lifetime measurements

The status-lifetime revision was compared with the preceding portal snapshot
using 100,000 empty files in an owned Docker volume. Three alternating runs per
version used one CPU, 1 GiB RAM, no swap or network, and a read-only fixture.
Median first frame was 118.151 / 110.529 ms before/after; wheel response was
9.275 / 8.928 ms; filtering was 25.497 / 26.229 ms. Peak GUI RSS was
97.609 / 98.422 MiB and total process-tree RSS was 112.723 / 113.180 MiB.
Idle CPU was 0% for both versions, with no OOM events. The owned volume was
removed after measurement. Receipt invalidation adds no per-file listing work;
it compares existing index snapshots and checks the current directory only
when publishing a receipt or receiving its directory-change notification.

## Root refresh measurements

Index loading includes the queued watcher debounce and the presence worker.
Studio's shared barrier checks it again after queued events, then waits for the
listing and focused metadata. Touch helpers wait for [Qt Quick polish](https://doc.qt.io/qt-6/qquicktest.html#qWaitForPolish-1)
before calculating scene coordinates, and verify the captured delegate is still
alive afterwards. A deferred layout can move a control; a model refresh can
destroy it. Pointer injection observes layout completion rather than a fixed delay. A target
retired during that barrier returns failure without injecting input; ordinary
callers assert successful preparation.
The movement regression keeps its target alive, moves it during pending polish,
and checks activation at the new position for touch and pen. A receiver at the old
position detects stale input, and a window observer checks polish precedes presses.
A separate retirement regression deletes the target during pending polish and
asserts refusal with no injected press. These are distinct proofs of layout
completion and lifetime; observing eventual deletion alone does not prove either
input ordering or refusal.

The root-declaration refresh revision was compared with commit `594ecbc` using
100,000 empty files in an owned Docker volume, three alternating runs per
version, one CPU, 1 GiB RAM, no swap or network and a read-only fixture.
Before/after medians were 89.550/87.185 ms for the first frame,
8.295/7.838 ms for wheel response and 18.219/19.203 ms for filtering.
Peak GUI RSS was 98.117/98.055 MiB and process-tree RSS was
112.898/113.062 MiB. Idle CPU was 0% in both versions, with no OOM events.
The owned volume was removed after measurement. Declaration watching adds no
per-file listing work; presence and root-availability checks run in a canceled
and replaced background job on startup and refresh.

## Preparing focus before keyboard assertions

Set up keyboard focus after the current row has been published and pending
Qt Quick layout has completed. Moving the pointer can deliver queued events;
perform that movement before the shared listing/metadata completion barrier and
[Qt Quick polish barrier](https://doc.qt.io/qt-6/qquicktest.html#qWaitForPolish-1).
Then verify the target is visible and enabled, assign focus once, and assert
both active focus and its visible outline. Do not reassign focus after the key
under test: that would hide a routing defect.

A refresh clears the details while its worker runs. Permission controls become
unavailable, so their normal focus traversal can move focus to the next enabled
control. Re-enabling a permission control does not itself request focus. A test
that focuses it before pending publication can therefore lose its setup during
pointer movement or polish, even though the eventual row is valid. Waiting
longer for that focus to return does not establish the missing preparation.

The sidebar routing matrix covers both an already published row and an actual
refresh queued before preparation, at both widths, with fitting/overflowing
panels, short tooltips and every navigation key. Signal observation proves the
refresh started and completed. The same cursor, focus, outline, scrolling,
permission preservation and left/right-control assertions apply to both cases.
CPU contention checks pin the test and two owned busy workers to one core;
workers are reaped before the full gate. The focus-before-publication mutation
must fail without relaxing any assertion or adding a fixed delay.

## Keyboard input and scene readiness

Studio's shared keyboard helper waits for [Qt Quick polish](https://doc.qt.io/qt-6/qquicktest.html#qWaitForPolish-1)
before injecting either character or enum key events. A viewport's height and
model count can already match their expected values while ListView layout is
pending. The barrier observes completion of that scene work; it does not change
focus, reset selection, lengthen deadlines or wait for an arbitrary duration.

The native-navigation matrix covers menus and the command palette with both
fitting and overflowing viewports, using direct and pending-polish resizes.
Page Down must use the completed viewport geometry. End, Home, Down, Up and
page navigation retain exact index assertions, and fitting text inputs retain
native caret movement and selection. Character navigation also runs with a
pending delegate-width change. An input observer checks that pending scene work
completes before key presses, separately from the navigation assertions.


## Result editors and modified text navigation

The tag editor and command palette are exercised through their public opening
commands at 1180 and 420 pixels wide. Tags come from a temporary index with 32
matching names; the palette uses its actual command results. Both viewports
overflow, and native wheel events establish a nonzero scroll position before
each selection gesture. A nonzero result row prevents boundary clamping from
hiding accidental navigation.

Each editor is compared with an installed-Qt `TextInput` reference inside the
same overlay focus scope, with both a caret and an existing selection. Shift,
Ctrl+Shift, Ctrl, Alt and Meta arrows, pages and document keys must retain native
caret/selection behavior, result selection, viewport positions and text focus.
Plain arrows/pages, palette Ctrl+Home/End and Ctrl+N/P are positive controls.
Independent mutations remove the modified-navigation guard from each editor.
The tests must reject the corresponding result movement while the other editor
continues to pass. No real launcher or desktop state is used.


## Directory read counters and asynchronous storage refresh

Directory read counters include started attempts, including scans cancelled by
a newer query. The index-only cache test checks exact counter equality after
index publication and one extra read after an explicit folder reload.

Storage refresh also publishes an asynchronous index result. A separate test
controls the real workers' completion notifications, with both computations
finished before choosing the publication order. Publishing presence before the
pending directory result cancels that result and starts a fresh scan with the
new index snapshot: two started reads. Publishing the directory result first
makes its completed listing available to the subsequent index-only query:
one started read. Both cases require one storage notification, one presence
publication and the same final rows. A cancelled result cannot reset the model
or finish the replacement query. No counter tolerance or fixed delay is used.

The test-only barrier holds new worker signals until their native futures have
finished, then releases each completion exactly once. Subsequent workers use
normal signal delivery. See Qt's [future completion notifications](https://doc.qt.io/qt-6/qfuturewatcher.html).

## Native drag-and-drop injection

Synthetic drag helpers receive the visual target and a point relative to its
size. They wait for the window's Qt Quick polish before reading dimensions and
mapping that point to scene coordinates. A `QPointer` guards targets retired
during the wait; a detached target cannot receive a drop through another window.
The enter/drop events use the completed geometry for accepted and rejected drops.

The regression uses a real QML `Row` with two native drop receivers. Increasing
the first receiver's width schedules a reflow while the second receiver retains
its pending position. Mapping before polish can land on the expanded neighbor,
which deliberately has the opposite acceptance policy. Tests require the exact
receiver, target position and local drop coordinates, for acceptance and refusal.
A separate pending-polish retirement case requires no event on the neighbor.
Moving the polish wait after mapping, or removing it, must fail these controls.
There is no fixed layout delay or weakened virtual-view refusal assertion.

See [Qt Quick polish completion](https://doc.qt.io/qt-6/qquicktest.html#qWaitForPolish-1),
[item coordinate mapping](https://doc.qt.io/qt-6/qquickitem.html#mapToScene), and
[Qt's drag delivery and scene hit testing](https://github.com/qt/qtdeclarative/blob/v6.11.2/src/quick/util/qquickdeliveryagent.cpp).

A real-window regression grows the folder tab adjacent to a virtual tab before
the strip's Row reflows. It checks that the stale virtual-tab point overlaps the
valid folder target, while the completed point does not. The destination contains
an existing same-named file: a misrouted drop exposes the original conflict-sheet
signature. The helper must reject the virtual drop, emit no transfer request,
leave the window unblocked and preserve the destination bytes. The fixture
closes an unexpected conflict through the sheet's public cancel action, then
waits for pending `FileTransfer` planning watchers to deliver their results and
retire, the operation to become idle and the sheet to disappear before retiring
its temporary destination. The cleanup uses `QTest::qWaitFor`, whose event loop
continues after an assertion failure; `QTRY` stops waiting when a test is resolved.
Tab geometry and the original folder are restored before cleanup reports failure.

Failure containment is checked by running the stale-coordinate helper mutation
followed by ordinary clipboard and native-navigation cases in the same process.
Only the intended coordinate/refusal control may fail: subsequent cases must
initialize and pass, with the unchanged blocked-window guard. Restoring the
cancel-only cleanup must expose the blocked-window cascade in that sequence.

A separate native-drop control holds the real transfer-planning worker in the
thread pool, so cleanup begins before the index operation starts and with no conflict sheet.
An event releases the worker during cleanup. The resulting real conflict must
be cancelled, the source and destination bytes preserved, and the window idle
and unblocked before the temporary destination retires. The worker and pool
configuration are restored even when a test assertion fails.
