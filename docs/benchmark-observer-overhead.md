# Memory-observer perturbation

Linux [`do_task_stat`](https://raw.githubusercontent.com/torvalds/linux/master/fs/proc/array.c)
takes target-task locks and aggregates thread statistics. Reading from a separate
CPU therefore does not establish that `/proc` observation is nonintrusive.
The chosen control toggles only the independent 1 ms memory observer, preserving
the frame probe, coarse CPU sampler, workload and container limits.

One cohort contains 200 sessions: five alternating on/off pairs for each of two
apps, two configurations and five fixtures. Frozen product sources match
`10781b6`; affinity is 0/1, load threshold 4 and timeout 600 s, after a continuous
120-second quiet preflight. External work interrupted collection after slots
134 and 193; only missing slots resumed. All 200 observations are retained,
including contention and unavailable RSS. Seven final slots completed in the
last quiet window. No pair was repeated or replaced.

The declared materiality rule is a median paired increase of at least 10% and
it: paired median +130.04 ms, median on/off ratio 3.20, four positive pairs.
Its raw on values are [533.88, 189.19, 339.31, 54.94, 60.86] ms; off values are
[90.64, 59.15, 55.04, 57.17, 59.03] ms. Pair 2 has recorded external contention;
the cohort does not isolate the precise kernel mechanism or exclude host noise.
−1.06 ms, with a large positive outlier. No other metric meets the declared rule.

Decision: collect latency and memory in separate fresh sessions for both apps,
rather than assume equal observer perturbation. The memory replay preserves the
1 ms request and strict 5 ms gap/read-span/coverage checks. Its latency is not
No additional performance cohort is needed to choose this collection method.

The table contains median **paired on minus off** differences, in milliseconds
except idle CPU (percentage points). Each cell has five metric-valid pairs;
unavailable. Recorded post-run load cannot prove absence of within-run contention.
All raw observations, per-pair differences and load annotations are retained in
`/tmp/omafiles-3479/{omafiles,reference}-{on,off}.json.gz` and
`paired-analysis.json` for the tester handoff.

| App | Config | Fixture | Frame | Filter | Wheel | Settle | Idle pp | Thumbnail | Preview |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| omafiles | A | 1000 | +2.35 | +0.15 | +0.52 | -0.65 | +0.00 | — | — |
| omafiles | A | 10000 | +0.78 | -2.48 | +0.25 | -150.02 | +0.00 | — | — |
| omafiles | A | 100000 | -3.35 | -0.76 | -0.20 | -5.78 | +0.00 | — | — |
| omafiles | A | images-2000 | +0.98 | -0.79 | +0.05 | +21.60 | +0.00 | +0.93 | +1.60 |
| omafiles | A | 1000000 | +10.17 | +5.60 | +0.49 | +46.88 | +0.00 | — | — |
| omafiles | B | 1000 | +5.46 | +0.00 | +0.21 | -106.34 | +0.00 | — | — |
| omafiles | B | 10000 | -0.53 | -0.95 | -0.10 | +0.82 | +0.00 | — | — |
| omafiles | B | 100000 | +4.87 | -0.83 | +0.60 | +0.24 | +0.00 | — | — |
| omafiles | B | images-2000 | +0.47 | +0.35 | -0.04 | -3.23 | +0.00 | -1.80 | +0.84 |
| omafiles | B | 1000000 | +1.21 | +1.39 | +0.56 | -4.20 | +0.00 | — | — |
