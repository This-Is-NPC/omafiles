# GUI benchmark

`scripts/bench` measures omafiles in isolated Docker containers.
A result is evidence for this machine and method, rather than a general
claim about file managers or physical disk speed. The latest complete matrix is
in [Benchmark results](benchmark-results.md).

```sh
scripts/bench
scripts/bench --build build/bin --output /tmp/omafiles-results.json
```

Without `--build`, the command builds the repository through the disposable
validation environment. With `--build`, it copies that directory's CLI, Studio
and helper binaries into a frozen read-only snapshot. It compiles the external
Qt observer separately; no benchmark instrumentation enters product builds.
Output is JSON with raw per-run evidence and a Markdown table beside it. The
default omafiles output is a uniquely named JSON file in `/tmp`; `--output`
selects a path inside the repository or `/tmp`. Symbolic links and output files
with multiple hard links are refused. Output filenames must end in `.json` or
`.json.gz`. Existing reports at an explicitly selected path are replaced.

## Requirements and isolation

The command requires Linux, Docker, bubblewrap, Python 3, the installed pinned
`usage` tool used by validation, and the C++ compiler/Qt development packages
used by the project. The default runtime image is the already installed
`omafiles-t02:local`; `--image` selects another installed image. Images are
inspected first and never pulled, built or installed automatically. Runtime
requirements are Python, Qt Quick/plugins, libarchive, zlib, KF6WindowSystem
and supporting libraries. The current image uses Arch Linux. Host `/usr/lib`
is mounted read-only at `/hostlib` so the observer and copied binaries use the
same Qt ABI. This is a Linux host-runtime benchmark, not an independently
packaged application image.

Builds and probes run through `scripts/test-environment.py`. Every application
launch gets fresh HOME, XDG data/config/cache/state/runtime directories,
TMPDIR, CARGO_HOME, RUSTUP_HOME and mise directories/config files in its private
container `/tmp`. Qt uses offscreen/software rendering. D-Bus and audio addresses
point at nonexistent private endpoints. No host display, GPU, session bus,
system bus, thumbnail cache, Trash or desktop configuration is mounted. Root
filesystems, fixtures, binaries, observer and runtime libraries are read-only;
a per-session writable `/memory` bind under the owned `/tmp` staging directory
carries observer handshakes/evidence and is removed with that staging context;
capabilities are dropped, privilege escalation is disabled and networking is
disabled. The private writable tmpfs is not executable.

All five fixtures live in one exclusively named and labeled Docker volume:
1,000, 10,000, 100,000 and 1,000,000 empty text files, plus 2,000 valid distinct
256×256 RGB PNGs. Numeric filenames have fixed-width suffixes. An unlimited
helper populates the volume and verifies counts; the measured containers mount
it read-only. The million-file tree is never created directly in `/tmp`.

Only resources registered by this invocation and carrying its ownership label
are removed. A durable journal records ownership before creation. The outer
supervisor retries cleanup in a fresh isolated process if SIGINT/SIGTERM kills
the measurement sandbox. Partial JSON reports remain marked incomplete. A
cleanup failure returns an error and retains the ownership journal for recovery;
the command never prunes unrelated containers, volumes or images. SIGKILL of
the outer supervisor or host failure cannot execute a cleanup handler.

## A/B schedule and host load

| Configuration | CPU quota | Affinity | RAM | Swap | Network |
|---|---:|---|---:|---:|---|
| A | 1 CPU | first selected CPU | 512 MiB | none | none |
| B | 2 CPUs | both selected CPUs | 1 GiB | none | none |

Both `--cpus` and `--cpuset-cpus` are passed. Memory observation requires an
additional allowed host CPU on a physical core outside both A/B placements;
SMT siblings of measured cores are excluded. The observer runs in its own host
process outside the application cgroup. If no such CPU is available, the
harness refuses rather than putting the observer on a measured core.
By default, two permitted CPUs
from distinct physical cores are selected from Linux topology; `--cores 0,1`
selects two permitted logical CPU IDs explicitly. Pinning does not reserve those
CPUs against other host work. Cgroup quota, affinity, RAM, swap, memory events
and Docker OOM/exit state are recorded with every run.

There are five paired repetitions per fixture/configuration. Each repetition
uses two fresh containers/homes and the same frozen products: a latency session
without the independent RSS observer and a memory replay with it. Odd repetitions
run latency then memory; even repetitions reverse that order. Within each fixture the schedule
is A1, B1, B2, A2, A3, B3, B4, A4, A5, B5. Before each physical session the command waits for the host's one-minute
load average to be strictly below `--load-threshold` (default: half the number
of permitted logical CPUs). It records every wait sample, the accepted
1/5/15-minute load averages, the wait duration and post-run load. The default
`--load-timeout` is 600 seconds; exceeding it refuses that batch and cleans up.
Load average is a host-wide runnable/uninterruptible-task average, rather than
per-core utilization. A quiet entry condition cannot guarantee a quiet run.

The JSON separates the first repetition and subsequent-repetition median for
each metric, using its corresponding session. These are
page/runtime-cache observations: every application cache is fresh, the fixture
volume is reused, and host page caches are never dropped. Neither group is
described as cold disk I/O.

## Observer command delivery

The preloaded Qt observer uses [QFileSystemWatcher](https://doc.qt.io/qt-6/qfilesystemwatcher.html)
on the command directory. The session atomically replaces `command.json`; a
watch on the file itself stops after replacement. Qt's
[Linux inotify backend](https://codebrowser.dev/qt6/qtbase/src/corelib/io/qfilesystemwatcher_inotify.cpp.html)
notifies directory entry changes, so appending to the already-open GUI log does
not wake command delivery. The observer reads once after installing the watch,
then when directory entries change. It has no recurring command timer. A command
written before the window exists is delivered on the first suitable window Show
event; the startup event filter then removes itself. A command-directory watch
failure reports an error and exits with status 2.

Serial numbers deduplicate notifications. Commands still finish on rendered
frames satisfying their count, visible-label, scroll or media conditions. The
500 ms CPU quiet window and the measured idle interval keep their definitions.
Tests load the actual observer into a disposable Qt fixture through `Session.phase`
and verify reads, timer registration, atomic replacements, delayed startup and
completion conditions; their assertions concern completed work, not latency.

Results measured with different observer versions are not comparable.

## Measurements

Method 3 collects latency and RSS in separate sessions. Each session executes
the same startup, listing, idle and interaction workflow. The reported top-level
frame/filter/wheel/settle/idle/thumbnail/preview values come only from the
session marked `latency_only: true`, whose independent memory observer is
disabled. Its frame probe and coarse CPU sampler remain enabled. The nested
`memory_session` records the observed replay, including its timings, which are
diagnostic and never substituted for reported latency. Reported RSS comes only from
that replay's validated independent samples. Configuration, fixture and repeat
must match across the pair. Each physical session has its own load guard,
container exit/OOM evidence and fresh private cache/home; no two sessions overlap.
Pending first-session evidence survives interruption of the second session.

The [paired observer investigation](benchmark-observer-overhead.md) explains
why separate sessions are required.

The observer sends Qt keyboard/wheel events and examines the rendered item tree.
It does not invoke application file-operation or model methods. Both windows use
a recorded 1180×720 client size. No screenshots are taken during timing. The
method uses [Qt frame completion](https://doc.qt.io/qt-6/qquickwindow.html#frameSwapped),
[Docker quotas/affinity](https://docs.docker.com/engine/containers/resource_constraints/)
and [Linux load averages](https://man7.org/linux/man-pages/man5/proc_loadavg.5.html).

| Metric | What completes it | Limitations |
|---|---|---|
| `frame_ms` | Launch to first observed `frameSwapped` | The first frame need not contain a complete listing. |
| `filter_ms` | Exact last filename typed after `/`, then a frame with the expected result count and visible filename | omafiles filters the directory and includes a parent row. |
| `wheel_ms` | One very large downward wheel event, then a frame at the end with the last filename inside the viewport | It does not visit every intermediate row or measure sustained scrolling. |
| `settle_ms` | From launch until the validated listing has been followed by 500 ms of unchanged process-tree CPU ticks | This is observed quiescence, not an application work-completed signal. |
| `idle_cpu_percent` | CPU ticks from the settle sample to the first recorded sample at least one second later, as a percentage of one CPU | Tick quantization and observer activity affect small values. The actual elapsed interval is the denominator; CPU activity during it is recorded, including nonzero values. |
| `peak_gui_rss_mib` | Peak sampled GUI resident set throughout startup and interactions | Independent 1 ms requested sampling, with recorded gaps/read spans; approximate kernel RSS counters, not PSS. |
| `peak_tree_rss_mib` | Maximum concurrent app/descendant RSS sum from the independent observer | Distinct PIDs per snapshot; shared pages counted per process. Neither an exact instantaneous bound nor cgroup memory consumption. |
| `thumbnail_ms` | Grid shortcut to a frame with a ready visible image thumbnail | First visible thumbnail, not completion of all 2k thumbnails. Images case only. |
| `preview_ms` | Preview shortcut to a frame with a ready decoded image | Follows the thumbnail phase and may benefit from resident source data. Images case only. |

Interactions begin only after expected listing count and filename are observed
in a frame. Filter completion requires omafiles to observe the parent row plus
all filtered results. Media navigation restores the listing and selects an image
by keyboard. Preview has an independent deadline and still runs after unavailable
thumbnails. Such a preview occurs later and is not an equal end-to-end media
workflow. All observation deadlines and failed completion reasons are preserved.
Missing/failed values are `null`, never zero. A cgroup OOM kill invalidates that
run's metrics, even when the container itself survives.

This is software/offscreen responsiveness on synthetic fixtures, with observer
and polling overhead. It does not measure GPU rendering, compositor registration,
real storage/device services, production photo/video mixes or user think time.

Reports default to lossless `.json.gz` with a readable `.md` table. Explicit
`.json` output remains available for inspection. Gzip preserves every 1 ms
sample, process record, completion event, validity check and provenance hash.
It does not substitute stored peaks, summaries or certificates for
raw evidence. Corrupt or truncated gzip is refused. JSON and gzip use the same
measurement schema and identity; the container/session/observer is unchanged.

Output uses [Python's gzip format](https://docs.python.org/3/library/gzip.html),
compression level 6 and `mtime=0`, so identical documents produce identical gzip
bytes with the same Python/zlib implementation, without filename or creation-time
metadata. Standard `gzip -dc FILE.json.gz` exposes the complete JSON. An existing
measurement can be converted to compressed form without new sessions:

```sh
gzip -n -6 -c /tmp/report.json > /tmp/report.json.gz
sha256sum /tmp/report.json
gzip -dc /tmp/report.json.gz | sha256sum
```

The two uncompressed SHA-256 values must match. Compression hashes may differ
between gzip implementations; measured values and evidence must not. Keep the
readable table beside the compressed report. Compression and report writing
happen after a session ends, outside its measured interval; the existing load
guard still precedes the next run.

Each metric reports successful n, median, minimum, maximum and full range
(`spread = max − min`). Five successful observations are required for analysis.
The cached JSON summary is recomputed from validated raw observations when read.
Missing binary/frontend or runtime/observer hashes and missing or contradictory
fixture verification are rejected. Failed phases, OOM, mismatched cgroup limits
or missing/invalid accepted host-load evidence exclude the affected observations.
Viewport, visible counts, phase completion and recorded interaction latency must
agree with the measurement contract; a number alone is not completion evidence.
First-frame values must match observer events, RSS peaks must match independent
concurrent process samples, and settling/idle values must match the recorded sampling interval.
Documents marked incomplete or containing recorded report-level or cleanup errors are
refused before analysis. Zero successful samples for any metric is not a valid measurement.

Exit 0 means a complete measurement; exit 2 means an incomplete/inconclusive result, invalid input or
cleanup failure. SIGINT/SIGTERM cancellation returns 130 after supervised cleanup.

The A→B column is A/B for latency and RSS, and B−A percentage points for idle
CPU. Zero/zero ratios are undefined. RSS A/B is a memory ratio, not a speedup.
A→B doubles both the CPU quota and memory allowance, so these ratios describe
the combined configuration change and cannot isolate CPU scaling.

Use the same method and recorded environment for before/after optimization
runs. `--fixtures 100000 images-2000` restricts a diagnostic run; a partial matrix
does not establish performance for omitted cases. Unit tests exercise parsing,
load decisions, fixture integrity and resource cleanup with fakes;
they never assert a wall-clock performance threshold.

## Directory memory

The ordinary name-sorted folder listing stores a four-byte span per entry
and shared blocks of 64K ASCII or UTF-16 name units. Each name has a length
unit in its block. ASCII names use one byte per character; other names retain
their complete UTF-16 representation. Bounded blocks avoid copying a growing
folder-wide name allocation. Qt's UTF-16 comparison preserves Unicode case
matching where mixed-encoding searches have different semantics.

A complete ascending query uses an implicit row interval. Filtered or reordered
queries store four-byte indices, with the parent row inserted before other rows
so it cannot force a second allocation at the front. The payload for an
unfiltered 15-character ASCII filename is 20 bytes per entry, before capacity,
allocator overhead and concurrent queries. Storage refuses names beyond the
255-unit length capacity, 512 MiB of ASCII name blocks or 1 GiB of UTF-16 blocks.
Metadata is described on demand and cached for 128 entries.

Index updates rebuild tags and query results over the existing directory
snapshot. Filesystem notifications, an explicit refresh, storage changes and
root declaration changes request a fresh directory read. Menus and the command
palette populate their entry models while open. Menu, palette, help, permission, archive and tag sheet scaffolding is created on
first open and retained for subsequent use; controls create tooltip popups on
first hover. Hidden scroll panels suspend focus tracking. Tooltips use the native
Qt ToolTip delay (480 ms), with the same bounded, scrollable content. These storage choices do not
establish an RSS result: use the [complete A/B measurements](benchmark-memory.md) to compare
process memory and response times.

Empty source files skip native image decoding because there are no image bytes
to decode. Cached thumbnails and installed thumbnail providers remain eligible.
Nonempty sources retain Qt's content detection, including extensionless and
misnamed binary or textual image formats.

Thumbnail workers retain pixels at the icon's physical display bounds, including
the window's device pixel ratio. A resize or DPI change requests suitable pixels
again while retaining the previous image until delivery. Shared PNG cache files
keep their freedesktop tier dimensions. Provider discovery only initializes MIME
matching when a thumbnailer declares supported types.

### Allocation profiling

Heaptrack ran in a disposable image derived from the benchmark image, inside
the same CPU and memory limits. Instrumented process RSS is not a performance
measurement. Exclusive allocation stacks at the observed A/1M heap peak on
base `730c43d` accounted for 100,663,296 bytes in directory reading,
23,261,483 bytes in folder queries, 14,801,695 bytes in QML, 1,765,934 bytes
in Quick items, and 4,005,183 bytes in images. The first two categories cost
approximately 100.66 and 23.26 bytes per entry at that peak, including temporary
copies and unused capacity. The original directory spans carried QStrings,
and query sorting materialized another name per row. These are allocations
observed at one peak, not additive lifetime totals or a per-entry RSS claim.

The compact listing diagnostic retained 16.06 MB of name blocks and 4.19 MB
of span capacity for 1M 15-character ASCII names; an unfiltered ascending view
uses no separate row index. Profiling ordinary process mappings also exposed
allocator retention beyond live allocations. The process RSS peak occurred
after listing completion, when opening the filter, so storage payload alone
was insufficient to establish the target.

The implementation follows Qt's [Loader creation and focus semantics](https://doc.qt.io/qt-6/qml-qtquick-loader.html),
[QByteArray capacity contract](https://doc.qt.io/qt-6/qbytearray.html#reserve),
and [ToolTip delay](https://doc.qt.io/qt-6/qml-qtquick-controls-tooltip.html).
The installed Qt 6.11 string implementation was checked against the original
UTF-16 ordering and case-insensitive search, including mixed encodings.
[Native ToolTip](https://codebrowser.dev/qt6/qtdeclarative/src/quicktemplates/qquicktooltip.cpp.html)
uses a basic timer for its delay; a [QML Timer](https://doc.qt.io/qt-6/qml-qtqml-timer.html)
is synchronized with the animation timer. Avoiding unnecessary startup objects and animation
callbacks preserves the interaction contract while reducing their work.

### Startup tooltip events

A buffered event trace of the same frozen executable reproduced fast (785.137 ms)
and slow (1058.328 ms) image startup. Both delivered the Parent folder tooltip's
480 ms timer and a subsequent window update. The slow run recorded a CPU tick
at 543.717 ms after launch, restarting the half-second quiescence window; the
fast run's tooltip work did not cross another accounted tick. This explains the
bimodal result without treating five-run median changes as continuous slowdown.

The initial window Enter event placed the backend's stationary cursor over the
Parent folder button. No mouse movement requested help. Tooltips follow the
[Qt Widgets motion-triggered convention](https://codebrowser.dev/qt6/qtbase/src/widgets/kernel/qapplication.cpp.html#2755):
a shared attached object observes the first mouse or stylus movement in each
window, then removes its event filter. Opening a window alone does not schedule
a tooltip. Existing hover feedback and the 480 ms help delay remain available
when the pointer moves. Ten subsequent instrumented A/B runs had no late tooltip
event. Diagnostic traces add observer overhead and do not replace the official
benchmark or change its CPU-quiescence definition.

The [paired memory measurements](benchmark-memory.md) retain all medians and increases,
with the residual settling variation attributed to the shared observer.

## Directory filtering

Folder queries search the whole immutable directory listing in a worker,
including names outside the rendered window. ASCII queries prepare Qt's
[QLatin1StringMatcher](https://doc.qt.io/qt-6/qlatin1stringmatcher.html) once per
worker query. ASCII eligibility is checked once per query. The query owns the
pattern bytes for the matcher's borrowed view.
Unicode names or queries retain Qt's UTF-16 case-insensitive substring semantics;
matching does not introduce normalization or accent stripping. Displayed names
and paths retain their original spelling.

A name-only query can examine the previous accepted result when it extends the
previous nonempty query under the same listing, hidden-file setting, sort
direction and parent-row context. Canceled or partially completed work supplies
no reuse context. Broadening, replacement, clearing, metadata criteria, storage
refresh or a changed directory requires the corresponding full query. A compact
unfiltered range has no materialized candidate vector to reuse. Descending
candidate order is preserved, and the parent row remains unique.

This retains one accepted result, without a query history or a duplicate folded
name arena. A second 15-byte name for each of 1M entries would alone cost about
14.3 MiB before capacity and indexing. Prepared query state avoids that retained
per-entry payload. Work counters in tests distinguish cold scans from safe
incremental reuse; semaphore-controlled cancellation tests observe actual queued
work and accepted publication. These design checks do not establish a latency
or RSS result; performance conclusions require the recorded A/B measurements.

The [paired filtering measurements](benchmark-filter.md) retain all A/B medians,
raw samples, positive changes, and the reverse-order 1M startup countercheck.
They show the cold-query gain separately from the work-count evidence for
incremental narrowing.

The [paired preview measurements](benchmark-preview.md) preserve every metric
and raw A/B sample, including positive deltas and identical-executable variation.

## Directory enumeration and startup work

The shared directory visitor passes names as callback-lifetime string views.
ASCII names borrow the directory reader's bytes as Latin1; other names retain
strict UTF-8 validation and expose decoded UTF16. Callers that retain a name
materialize an owning string during the callback. The immutable Studio listing
copies ASCII bytes directly into bounded name blocks and keeps four-byte entry
spans. It performs metadata reads only for links and unknown directory types;
case-insensitive sorting orders directories before files, and workers check
cancellation before publishing. Two ASCII names compare through an ASCII case
fold, which gives Qt's case-insensitive order without its general Unicode path;
any other pair uses Qt's comparison.

This avoids allocating and decoding an owning string for every ASCII entry.
The callback's borrowed lifetime is explicit: retaining the view after the
callback returns is invalid. Tests verify the byte alias, encoding, retained
copies, leading BOM, invalid UTF-8, early termination and descriptor ownership.
Startup comparisons require the complete A/B matrix with exclusive host access;
listing stage profiles identify work and do not substitute for that comparison.

The index prepares its owned data and configuration directories before registering
source watches. Initialization therefore does not generate parent-directory
notifications that invalidate an in-flight folder listing. Existing index and
root-file creation and atomic replacement remain watched. Native registration
tests cover fresh and existing state directories without timing an idle interval.

The [paired startup measurements](benchmark-startup.md) retain every official
metric and raw sample, the unsuccessful initial candidate, startup phase
attribution and the reverse-order images countercheck. They distinguish the
settle gain from helper/frame variation and sampled transient RSS.

The [parallel directory analysis](benchmark-directory-parallel.md) distinguishes
complete-listing frames from placeholders, measures serial work and records the
Amdahl-based scaling criterion alongside the A/B performance evidence.

## High-resolution process-tree memory

The frame/CPU-quiescence loop still polls every 20 ms. Memory observation uses
one external host process requesting a snapshot every 1 ms on a separate physical
core. It is armed before application launch;
the session publishes its application PID and launch clock. The observer maps
the PID from the container namespace, follows descendants through every thread,
and counts each live PID once. It records PID/start-time identity, current RSS,
GUI PID, snapshot read span and elapsed time. The session stops observation and
receives final evidence before terminating the app. Startup handshakes precede
the launch clock, and final JSON transfer follows interaction timing. Observer
CPU and memory are outside the application's quota, memory limit and tick sum;
shared host caches and memory bandwidth can still cause indirect interference.

The observer suspends [Python's cyclic garbage collector](https://docs.python.org/3.14/library/gc.html) for the capture and
restores its previous state on both success and failure. Its retained evidence
consists of acyclic lists, dictionaries and scalar values; reference counting
still releases temporary objects. This avoids periodic scans of the growing
sample history during a timed read. In the R58 investigation, a controlled
24,000-snapshot process/thread-tree capture recorded a 5.43 ms GC pause within
a 6.01 ms read; the corresponding GC-suspended capture had a 0.64 ms maximum.
These are diagnostic observations, not application performance results.

The observer was validated with synthetic app/launcher controls capturing
a short helper allocation absent from the coarse samples. These validate the
instrument; they do not publish a replacement performance baseline or guarantee
that future scheduling delays will remain below the rejection bound.

RSS reads already use `/proc/PID/stat`; namespace discovery alone reads `status`.
`statm` uses the same approximate resident counters, so changing files does not
provide exact RSS. Every snapshot still discovers descendants through every
thread's `children` file, rather than caching a tree that could omit a new short
helper. A native observer or reusable descriptors would be justified if the
remaining per-read work exceeded the budget; removing the demonstrated cyclic
GC pause retains the simpler implementation. The observer stays on an unmeasured
physical core with normal scheduling. It does not request real-time priority,
change host affinity policy, or assume affinity guarantees a deadline.

The 1 ms request and 5 ms rejection bound are retained. Five milliseconds is a
temporal-resolution budget, four times finer than the old 20 ms observations,
chosen to bound gaps around short helper overlap. It is not a guarantee of
capturing an arbitrarily short peak. Accepting the observed 8–10 ms outliers by
raising the threshold would weaken that bound rather than fix capture cost.
Read spans and actual inter-sample gaps remain independently checked; external
scheduling delays can still invalidate a run, which remains recorded.

`memory_session.memory.samples` contains the concurrent snapshots used for both
RSS maxima. Its `max_gap_ms` and `max_span_ms` are recomputed during validation.
Missing evidence, an observer error, duplicate PIDs, contradictory totals,
incorrect placement, a launch-discovery delay above 5 ms, missing final coverage,
or a gap/read span above 5 ms make memory unavailable with
a reason. There is no fallback to the coarse samples or a sum of process high
water marks. Failed/incomplete rounds retain their observations and reasons.
The native synthetic validation holds a 64 MiB child allocation until its peak
is observed, then releases it before the coarse samples; this checks completed
work without depending on a sleep deadline. App and launcher/GUI process shapes
use the same observer path in both limited configurations.

The Linux kernel's [RSS counters](https://man7.org/linux/man-pages/man5/proc_pid_stat.5.html)
are approximate; neither a 1 ms request nor a recorded 5 ms bound captures every
possible shorter peak. Sequential reads form a concurrent process snapshot,
not an atomic system-wide memory reading. Summing
[VmHWM](https://man7.org/linux/man-pages/man5/proc_pid_status.5.html) would combine
peaks that occurred at different times and lose exited helpers.
[cgroup-v2 memory.peak](https://docs.kernel.org/admin-guide/cgroup-v2.html)
tracks charged anonymous, file-cache and kernel memory, rather than RSS.
Current `memory.stat` anon/file/kernel counters cannot reconstruct the
components at that historical peak. They and `memory.current`/`memory.peak`
remain separately labeled diagnostics; none is substituted for process RSS.

Method 3 records the observer source hash and CPU in the benchmark identity
and validates the paired collection contract. Supplementary drivers use
the same `measured_session` lifecycle and writable `/memory` control mount;
the standalone session requires `--memory-control`. A direct container start
with only the frame observer cannot produce RSS observations.

The observer cost investigation also follows the Linux documentation for
[`statm` resident counters](https://man7.org/linux/man-pages/man5/proc_pid_statm.5.html),
[per-thread process information](https://man7.org/linux/man-pages/man5/proc_pid_task.5.html)
and [scheduler policies and privileges](https://man7.org/linux/man-pages/man7/sched.7.html).


## Memory-observer perturbation diagnostics

`--latency-only` disables the independent 1 ms RSS observer for a diagnostic
session. It preserves the same frame probe, coarse CPU samples, interactions,
container limits and startup handshake. RSS is intentionally unavailable;
coarse RSS is never promoted to a peak. The report identifies the diagnostic
purpose and observer mode. Normal runs pair these unobserved latency
sessions with separate mandatory high-resolution memory replays and retain
the 5 ms rejection bound.

The paired observer comparison uses the same frozen products, five fixtures
and A/B, with five alternating observer-on/off pairs per case. It retains every
run and compares frame, filter, wheel, settle, idle CPU, thumbnail and preview
measurements. Only the independent memory observer changes. It requires an
exclusive machine and the normal load guard; no performance claim is made from
implementation or unit tests alone.
