# Parallel directory work and Amdahl limits

These historical tables use method 1. Their 20 ms sampled RSS values can miss
short helper peaks; they are not evidence of exact peak memory or comparable
to the corrected [method 2 memory observer](benchmark.md#high-resolution-process-tree-memory).

The directory worker budget is the intersection of process affinity and visible
ancestor cgroup CPU quotas, capped at four workers. A uses one CPU and 512 MiB;
B uses two CPUs and 1 GiB. Both configurations use `--cores 0,1`,
`--load-threshold 4`, `--load-timeout 600`, five official repetitions, and the

Private parallel workers take disjoint CPU slots from that allowed budget and
apply Linux thread affinity. Slots are released when work returns or throws;
concurrent folder queries share the same limit. The GUI and global pool keep
their affinity. A private worker can retain its last CPU while idle; each new
chunk takes its own slot. If a live cpuset change rejects placement, the kernel
still controls where the thread can run. See
[sched_setaffinity](https://man7.org/linux/man-pages/man2/sched_setaffinity.2.html).

Filtering uses chunks of at least 65,536 entries; sorting uses 32,768. Traces of
100k showed a roughly 0.6 ms serial scan and pool tasks sometimes executing in
sequence on one thread, so splitting that small scan adds dispatch work without
a reliable gain. The parallel filter threshold is 131,072 candidates (two full chunks). Below
that threshold, the background query scans directly without asking the executor
to plan partitions. A 1M scan uses the allowed parallel workers. One CPU uses
the direct scan without partition planning, affinity calls or pool tasks.
`partitionPasses()` counts executor partition requests so tests can verify this
work boundary without a timing assertion.

Directory enumeration uses the shared `visitDirectory` reader. The immutable
packed snapshot is sorted and filtered in contiguous worker chunks. Each scan
accumulates in a worker-local result and moves that result into its shared slot
once; counters and vector headers are not repeatedly written in adjacent slots.
The examined count comes from each range length, excluding the parent sentinel;
it does not require a counter write for every filename. Packed names reuse one
mutable block reference and one destination pointer for each append.
Pending query edits share one GUI notification until the query publishes. A
directory reset still notifies immediately; each query property keeps its own
notifier. This avoids repeated QML binding work during a burst of filter input,
following the [Qt property guidance](https://doc.qt.io/qt-6/properties.html).
Already ordered neighboring chunks require only a boundary comparison. Other
chunks use the standard in-place merge. One-worker and small-input queries use
the same work directly, without a pool task or merge buffer.

## What the scaling target measures

`frame_ms` can be a placeholder frame. The complete-listing frame is derived
from the unchanged observer timestamps:

```text
first_complete_frame_ms = listing.complete.elapsed_ms
                        + first_frame.ms - first_frame.elapsed_ms
```

This includes process startup and GUI work before enumeration, worker execution,
model publication and rendering. The internal listing duration is reported
separately and cannot replace the complete-frame metric. Filter latency includes
key delivery, query dispatch, worker execution, model publication and a validated
frame. Qt documents a fallback update-request delay of at most five milliseconds
in [QWindow::requestUpdate](https://doc.qt.io/qt-6/qwindow.html#requestUpdate).
The measurement uses the default frame scheduling policy.

## Measured serial fraction

A diagnostic copy timestamps startup, directory enumeration, chunk sorts and
merges, scans, publication and the first frame. Five paired frozen/instrumented
runs in each A/B configuration cover 100k and 1M entries. These stage profiles
explain the official measurements; they are not official performance results.

Enumeration includes `readdir`/`getdents`, name validation and packing.
Per-thread `getrusage` deltas separate user and system CPU for that phase; they
are not isolated syscall latency. Regular-file fixtures need zero link or
`DT_UNKNOWN` metadata fallback reads. Short CPU-accounting intervals have
accounting granularity; the critical-path model uses wall-clock timestamps.

For a conservative upper bound, the entire worker interval is treated as
perfectly parallelizable, including enumeration, allocation and merging. Only
time outside it remains serial. This favors parallelism: enumeration can contain
GUI overlap and one-CPU scheduling stalls, so its whole wall time cannot be
classified as an immutable serial cost. With serial fraction `s`, the ideal
speedup on two CPUs is:

```text
S2 = 1 / (s + (1 - s) / 2)
```

The reference A traces produced the following medians. Fractions and ceilings
are computed per run before taking their medians, rather than dividing unrelated
median stage durations.

| Fixture | Metric | Total (ms) | Minimum serial fraction | Ideal two-CPU ceiling | Acceptance: 90% of ceiling |
| --- | --- | ---: | ---: | ---: | ---: |
| 100k | Complete listing frame | 101.848 | 68.14% | 1.190x | 1.071x |
| 100k | Filter | 9.585 | 92.37% | 1.040x | 0.940x |
| 1M | Complete listing frame | 368.993 | 35.70% | 1.474x | 1.327x |
| 1M | Filter | 17.120 | 57.58% | 1.269x | 1.142x |

A second, more optimistic calculation assigns every positive paired
instrumented-minus-frozen timing difference to instrumentation in the serial
part and removes it. Its largest ceilings across five pairs are respectively
1.193x, 1.114x, 1.480x and 1.481x. These are limits for the measured serial
architecture, not a claim that startup, watch registration or frame scheduling
could never be redesigned.

Acceptance requires at least 90% of the measured ceiling for each scaling metric,
positive deltas require paired controls and raw distributions. A fast internal
sort or a favorable five-run frame-phase median alone cannot establish acceptance.

## Final matched controls

The final cohort contains twenty matched before/after pairs per A/B fixture,
400 sessions in total, with alternating order on the same fixture volume.
All sessions completed without errors or OOM; owned containers and the volume
were removed. Starting one-minute load stayed below 1.952; each run used the
unchanged load guard (threshold 4, timeout 600 seconds). The 120-second preflight
ranged from 0.323 to 1.010. No observations are excluded.

Both products originate from clean `a2b6161` archives. The clean-before cold
build is reused unchanged from the preceding cohort. Studio SHA256:

- Before: `7b7696a9e994d602f99db4894aecc6594bc083bfca04e5d4fb53881ecc9259b7`.
- After: `3df45b304f47edf55f20ece1db298499053e128cac73e28feb8ef114527b1ccc`.

Hashes were verified at cohort startup; products are mounted read-only in the
sessions. CLI, archive, device and preview helpers have identical before/after
`96a709119350b8be1e721141c2ad428862b7d3b3e439e91c766fd199248917ff`.
The measured source patch SHA256 is
`e345664559d46355bd79433b971a6ab99313699727a055af8f50143776814a77`;
adding this results documentation does not change the measured code.

The 100k filter's explicit acceptance floor is 0.94x. Ratios of medians remain
the primary scaling statistic; per-pair ratios expose phase sensitivity and
are reported without substituting a favorable statistic. The final cohort
meets both complete-frame targets. Its filter ratios do not meet their floors.
There is no strictly positive descriptive paired median-change interval for
any A metric, although positive point estimates remain in the full tables.
These intervals do not establish equivalence under arbitrary host correlation.
The tiny 1M frame margin is retained without rounding up.

### Acceptance decision and filter scaling shortfall

The filter scaling criterion was **not met**. In the final twenty-pair cohort,
100k filtering took 5.556602 ms in A and 6.0527325 ms in B: 0.918032x A/B,
below the 0.94x floor (88.30% of the reference ceiling). Both configurations
execute the same serial background scan below 131,072 candidates, without
executor partition planning or private-pool tasks. The median of paired A/B
ratios was 0.991975x. Remaining input, publication and frame-phase variation
must not be described as a pool-coordination cost.

At 1M, A improved from 16.4946925 to 11.231020 ms (31.91% lower latency),
while B improved from 12.366367 to 10.116992 ms (18.19% lower). The stronger
improvement in A reduces the A/B speedup: the final ratio of medians is
1.110115x, below the 1.142308x floor (87.46% of the reference ceiling).
Both absolute filter latencies improved; that does not turn the unmet scaling
38.718028 ms in B, above both omafiles results.

The user accepted this result on 2026-10-14, with the shortfall documented,
rather than requiring another favorable median. Complete-frame scaling reached
99.49% (100k) and 90.06% (1M) of the reference ceilings. The final paired
controls found no strictly positive A regression interval; all point estimates
and raw distributions remain reported. Acceptance does not change the filter
rows marked `NOT MET`, the benchmark, its baseline or its primary statistic.

The 100k filter requests no executor partitions in either configuration, as
verified by work-count tests and mutations. Its measured latency includes
input, dispatch, publication and rendering, including the observed frame-phase
modes. The cutoff removes executor planning; it does not establish that the
remaining end-to-end phase variation is pool coordination.


| Fixture | Metric | After A median | After B median | A/B | 90% target | Fraction of ceiling | Median pair A/B | Pair interval | Result |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |
| 100000 | first_complete_frame_ms | 100.8209 | 85.1945 | 1.1834x | 1.0706x | 99.49% | 1.1916x | 1.1614 .. 1.2067 | PASS |
| 100000 | filter_ms | 5.5566 | 6.0527 | 0.9180x | 0.9400x | 88.30% | 0.9920x | 0.7665 .. 1.4482 | NOT MET |
| 1000000 | first_complete_frame_ms | 380.5572 | 286.6886 | 1.3274x | 1.3265x | 90.06% | 1.3329x | 1.3192 .. 1.3412 | PASS |
| 1000000 | filter_ms | 11.2310 | 10.1170 | 1.1101x | 1.1423x | 87.46% | 1.1267x | 1.0990 .. 1.5219 | NOT MET |

### Every metric against the baseline

| --- | --- | --- | ---: | ---: | ---: | ---: |
| A | 1000 | frame_ms | 236.044 | 55.680 | 55.679 | -0.00% |
| A | 1000 | listing_work_ms | 48.198 | 18.610 | 18.451 | -0.85% |
| A | 1000 | first_complete_frame_ms | 263.813 | 69.417 | 69.581 | +0.24% |
| A | 1000 | filter_ms | 38.462 | 5.358 | 8.580 | +60.14% |
| A | 1000 | wheel_ms | 27.868 | 7.924 | 7.826 | -1.24% |
| A | 1000 | peak_gui_rss_mib | 99.207 | 77.357 | 77.369 | +0.02% |
| A | 1000 | peak_tree_rss_mib | 105.492 | 92.334 | 92.357 | +0.03% |
| A | 1000 | settle_ms | 1258.379 | 606.984 | 597.331 | -1.59% |
| A | 1000 | idle_cpu_percent | 0.000 | 0.000 | 0.000 | — |
| A | 1000 | thumbnail_ms | — | — | — | — |
| A | 1000 | preview_ms | — | — | — | — |
| A | 10000 | frame_ms | 233.677 | 58.102 | 58.028 | -0.13% |
| A | 10000 | listing_work_ms | 49.096 | 19.640 | 19.803 | +0.83% |
| A | 10000 | first_complete_frame_ms | 263.325 | 71.547 | 71.754 | +0.29% |
| A | 10000 | filter_ms | 38.917 | 5.150 | 7.775 | +50.97% |
| A | 10000 | wheel_ms | 28.043 | 7.720 | 7.759 | +0.52% |
| A | 10000 | peak_gui_rss_mib | 98.984 | 77.457 | 77.582 | +0.16% |
| A | 10000 | peak_tree_rss_mib | 105.535 | 92.471 | 92.562 | +0.10% |
| A | 10000 | settle_ms | 791.972 | 715.905 | 597.266 | -16.57% |
| A | 10000 | idle_cpu_percent | 0.000 | 0.000 | 0.000 | — |
| A | 10000 | thumbnail_ms | — | — | — | — |
| A | 10000 | preview_ms | — | — | — | — |
| A | 100000 | frame_ms | 233.658 | 68.350 | 67.980 | -0.54% |
| A | 100000 | listing_work_ms | 65.633 | 41.194 | 41.085 | -0.27% |
| A | 100000 | first_complete_frame_ms | 279.966 | 101.572 | 100.821 | -0.74% |
| A | 100000 | filter_ms | 40.369 | 5.382 | 5.557 | +3.25% |
| A | 100000 | wheel_ms | 27.553 | 7.829 | 7.780 | -0.62% |
| A | 100000 | peak_gui_rss_mib | 98.957 | 79.318 | 79.453 | +0.17% |
| A | 100000 | peak_tree_rss_mib | 108.012 | 94.309 | 94.467 | +0.17% |
| A | 100000 | settle_ms | 811.425 | 625.740 | 656.981 | +4.99% |
| A | 100000 | idle_cpu_percent | 0.000 | 0.000 | 0.000 | — |
| A | 100000 | thumbnail_ms | — | — | — | — |
| A | 100000 | preview_ms | — | — | — | — |
| A | images-2000 | frame_ms | 242.577 | 55.631 | 55.265 | -0.66% |
| A | images-2000 | listing_work_ms | 50.084 | 18.452 | 18.383 | -0.37% |
| A | images-2000 | first_complete_frame_ms | 272.808 | 69.534 | 68.905 | -0.90% |
| A | images-2000 | filter_ms | 40.075 | 5.172 | 8.461 | +63.60% |
| A | images-2000 | wheel_ms | 28.287 | 7.975 | 7.864 | -1.39% |
| A | images-2000 | peak_gui_rss_mib | 109.520 | 85.367 | 85.430 | +0.07% |
| A | images-2000 | peak_tree_rss_mib | 119.461 | 106.916 | 106.775 | -0.13% |
| A | images-2000 | settle_ms | 900.593 | 768.290 | 762.104 | -0.81% |
| A | images-2000 | idle_cpu_percent | 0.000 | 0.000 | 0.000 | — |
| A | images-2000 | thumbnail_ms | — | 169.294 | 169.089 | -0.12% |
| A | images-2000 | preview_ms | 57.266 | 36.737 | 36.896 | +0.43% |
| A | 1000000 | frame_ms | 236.659 | 138.200 | 138.672 | +0.34% |
| A | 1000000 | listing_work_ms | 219.543 | 248.891 | 249.918 | +0.41% |
| A | 1000000 | first_complete_frame_ms | 436.336 | 379.168 | 380.557 | +0.37% |
| A | 1000000 | filter_ms | 39.347 | 16.495 | 11.231 | -31.91% |
| A | 1000000 | wheel_ms | 27.407 | 7.882 | 7.934 | +0.67% |
| A | 1000000 | peak_gui_rss_mib | 98.949 | 96.453 | 96.574 | +0.13% |
| A | 1000000 | peak_tree_rss_mib | 131.184 | 111.439 | 111.547 | +0.10% |
| A | 1000000 | settle_ms | 962.577 | 900.403 | 900.039 | -0.04% |
| A | 1000000 | idle_cpu_percent | 0.000 | 0.000 | 0.000 | — |
| A | 1000000 | thumbnail_ms | — | — | — | — |
| A | 1000000 | preview_ms | — | — | — | — |
| B | 1000 | frame_ms | 215.903 | 53.244 | 53.334 | +0.17% |
| B | 1000 | listing_work_ms | 30.524 | 12.725 | 12.791 | +0.52% |
| B | 1000 | first_complete_frame_ms | 237.802 | 62.601 | 62.580 | -0.03% |
| B | 1000 | filter_ms | 38.838 | 8.988 | 7.066 | -21.39% |
| B | 1000 | wheel_ms | 27.608 | 7.936 | 7.884 | -0.66% |
| B | 1000 | peak_gui_rss_mib | 101.977 | 77.459 | 77.418 | -0.05% |
| B | 1000 | peak_tree_rss_mib | 107.770 | 92.410 | 92.406 | -0.00% |
| B | 1000 | settle_ms | 750.436 | 594.862 | 706.513 | +18.77% |
| B | 1000 | idle_cpu_percent | 0.000 | 0.000 | 0.000 | — |
| B | 1000 | thumbnail_ms | — | — | — | — |
| B | 1000 | preview_ms | — | — | — | — |
| B | 10000 | frame_ms | 222.939 | 54.645 | 54.870 | +0.41% |
| B | 10000 | listing_work_ms | 32.709 | 12.904 | 13.047 | +1.11% |
| B | 10000 | first_complete_frame_ms | 245.426 | 63.715 | 64.705 | +1.55% |
| B | 10000 | filter_ms | 40.213 | 5.694 | 7.121 | +25.06% |
| B | 10000 | wheel_ms | 27.562 | 7.962 | 7.971 | +0.12% |
| B | 10000 | peak_gui_rss_mib | 101.652 | 77.568 | 77.648 | +0.10% |
| B | 10000 | peak_tree_rss_mib | 107.809 | 92.570 | 92.572 | +0.00% |
| B | 10000 | settle_ms | 781.826 | 596.592 | 595.177 | -0.24% |
| B | 10000 | idle_cpu_percent | 0.000 | 0.000 | 0.000 | — |
| B | 10000 | thumbnail_ms | — | — | — | — |
| B | 10000 | preview_ms | — | — | — | — |
| B | 100000 | frame_ms | 229.754 | 63.935 | 64.114 | +0.28% |
| B | 100000 | listing_work_ms | 47.695 | 30.560 | 25.724 | -15.83% |
| B | 100000 | first_complete_frame_ms | 268.567 | 90.118 | 85.195 | -5.46% |
| B | 100000 | filter_ms | 40.746 | 8.742 | 6.053 | -30.77% |
| B | 100000 | wheel_ms | 27.908 | 7.692 | 8.101 | +5.31% |
| B | 100000 | peak_gui_rss_mib | 101.922 | 79.295 | 79.420 | +0.16% |
| B | 100000 | peak_tree_rss_mib | 110.617 | 94.232 | 94.369 | +0.15% |
| B | 100000 | settle_ms | 801.963 | 687.743 | 758.010 | +10.22% |
| B | 100000 | idle_cpu_percent | 0.000 | 0.000 | 0.000 | — |
| B | 100000 | thumbnail_ms | — | — | — | — |
| B | 100000 | preview_ms | — | — | — | — |
| B | images-2000 | frame_ms | 218.218 | 53.782 | 53.616 | -0.31% |
| B | images-2000 | listing_work_ms | 30.636 | 12.806 | 12.922 | +0.91% |
| B | images-2000 | first_complete_frame_ms | 242.333 | 62.978 | 62.922 | -0.09% |
| B | images-2000 | filter_ms | 41.474 | 6.260 | 5.785 | -7.59% |
| B | images-2000 | wheel_ms | 27.671 | 8.303 | 8.053 | -3.02% |
| B | images-2000 | peak_gui_rss_mib | 113.199 | 86.002 | 86.031 | +0.03% |
| B | images-2000 | peak_tree_rss_mib | 121.449 | 116.848 | 117.744 | +0.77% |
| B | images-2000 | settle_ms | 849.821 | 757.900 | 758.970 | +0.14% |
| B | images-2000 | idle_cpu_percent | 0.000 | 0.000 | 0.000 | — |
| B | images-2000 | thumbnail_ms | — | 168.729 | 168.004 | -0.43% |
| B | images-2000 | preview_ms | 42.369 | 33.195 | 32.926 | -0.81% |
| B | 1000000 | frame_ms | 219.601 | 133.807 | 133.649 | -0.12% |
| B | 1000000 | listing_work_ms | 195.501 | 234.645 | 157.570 | -32.85% |
| B | 1000000 | first_complete_frame_ms | 407.528 | 364.924 | 286.689 | -21.44% |
| B | 1000000 | filter_ms | 38.718 | 12.366 | 10.117 | -18.19% |
| B | 1000000 | wheel_ms | 26.986 | 7.842 | 8.100 | +3.29% |
| B | 1000000 | peak_gui_rss_mib | 101.734 | 96.496 | 96.631 | +0.14% |
| B | 1000000 | peak_tree_rss_mib | 133.746 | 111.484 | 111.582 | +0.09% |
| B | 1000000 | settle_ms | 934.083 | 881.385 | 820.908 | -6.86% |
| B | 1000000 | idle_cpu_percent | 0.000 | 0.000 | 0.000 | — |
| B | 1000000 | thumbnail_ms | — | — | — | — |
| B | 1000000 | preview_ms | — | — | — | — |


The internal 1M listing duration is also reported: an end-to-end win does not
paired intervals and load records are attached to task 3354 in Omakiten.
