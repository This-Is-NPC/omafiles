# Benchmark results

Medians of five fresh sessions per cell, with the minimum and maximum in
parentheses, measured by [`scripts/bench`](benchmark.md) with method 4.
Latency and RSS use separate fresh sessions: latency comes from the session
without the memory observer, and RSS from its separate memory replay, whose
timings are diagnostic only.

| | |
|---|---|
| Product | `e5484bd` |
| Date | 2026-11-05, 22:12–22:25 UTC |
| CPU | AMD Ryzen AI 5 340 w/ Radeon 840M, CPUs 0 (A) and 0–1 (B); memory observer on CPU 2 |
| Kernel | 7.2.8-9-omarchy, x86_64 |
| Host load | 2.13–5.96 one-minute host-wide average when each of the 100 sessions started, median 3.44 |

```sh
scripts/bench --build build/bin --output /tmp/omafiles-bench/head-result.json.gz
```

The defaults chose CPUs 0 and 1, a load threshold of 6.0 and the memory
observer on CPU 2.

| Fixture | Metric | A: 1 CPU, 512 MB | B: 2 CPU, 1 GB |
|---|---|---:|---:|
| 1000 | first complete frame (ms) | 72.7 (72.0–84.5) | 70.6 (68.4–71.2) |
| 1000 | filter (ms) | 7.1 (5.7–9.5) | 6.7 (5.6–8.5) |
| 1000 | wheel scroll (ms) | 8.4 (7.6–11.2) | 7.9 (7.5–8.3) |
| 1000 | GUI peak RSS (MiB) | 81.7 (81.4–81.9) | 81.6 (81.4–81.8) |
| 1000 | process-tree peak RSS (MiB) | 90.6 (90.4–90.8) | 90.5 (90.3–90.8) |
| 1000 | settle (ms) | 619.4 (615.3–782.5) | 618.3 (615.6–783.2) |
| 1000 | idle CPU (%) | 0.0 (0.0–0.0) | 0.0 (0.0–0.0) |
| 10000 | first complete frame (ms) | 72.7 (69.6–73.8) | 71.3 (66.8–103.3) |
| 10000 | filter (ms) | 7.4 (6.0–8.1) | 6.3 (5.9–7.7) |
| 10000 | wheel scroll (ms) | 8.6 (8.1–9.2) | 8.9 (7.9–9.3) |
| 10000 | GUI peak RSS (MiB) | 81.7 (81.6–82.0) | 82.0 (81.8–82.0), 4 of 5 |
| 10000 | process-tree peak RSS (MiB) | 90.7 (90.5–91.0) | 90.9 (90.6–91.0), 4 of 5 |
| 10000 | settle (ms) | 782.2 (620.1–785.8) | 781.2 (597.6–807.7) |
| 10000 | idle CPU (%) | 0.0 (0.0–0.0) | 0.0 (0.0–0.0) |
| 100000 | first complete frame (ms) | 95.1 (78.8–147.6) | 79.3 (72.5–161.3) |
| 100000 | filter (ms) | 14.0 (9.9–15.1) | 6.2 (6.0–13.7) |
| 100000 | wheel scroll (ms) | 8.2 (7.8–9.9) | 8.2 (7.7–11.0) |
| 100000 | GUI peak RSS (MiB) | 83.9 (83.7–84.2), 3 of 5 | 83.9 (83.7–84.0), 4 of 5 |
| 100000 | process-tree peak RSS (MiB) | 92.8 (92.7–93.0), 3 of 5 | 148.6 (92.8–149.3), 4 of 5 |
| 100000 | settle (ms) | 807.7 (707.2–823.9) | 789.8 (782.1–903.9) |
| 100000 | idle CPU (%) | 0.0 (0.0–0.0) | 0.0 (0.0–0.0) |
| 1000000 | first complete frame (ms) | 88.0 (83.1–137.8) | 92.3 (70.0–96.0) |
| 1000000 | filter (ms) | 17.0 (12.4–27.4) | 18.9 (10.5–21.7) |
| 1000000 | wheel scroll (ms) | 8.6 (7.8–9.7) | 9.5 (7.3–9.9) |
| 1000000 | GUI peak RSS (MiB) | 100.7 (100.4–101.0), 3 of 5 | 100.7 (100.6–101.2), 4 of 5 |
| 1000000 | process-tree peak RSS (MiB) | 109.9 (109.4–151.4), 3 of 5 | 109.7 (109.6–110.2), 4 of 5 |
| 1000000 | settle (ms) | 828.0 (823.3–912.2) | 848.3 (763.0–867.5) |
| 1000000 | idle CPU (%) | 0.0 (0.0–0.0) | 0.0 (0.0–0.0) |
| images-2000 | first complete frame (ms) | 72.3 (70.1–139.3) | 68.8 (68.4–74.7) |
| images-2000 | filter (ms) | 7.6 (6.0–9.6) | 7.2 (6.0–13.8) |
| images-2000 | wheel scroll (ms) | 8.4 (7.6–8.5) | 8.3 (7.9–9.7) |
| images-2000 | GUI peak RSS (MiB) | 91.5 (91.3–91.7), 4 of 5 | 92.1 (91.9–92.3) |
| images-2000 | process-tree peak RSS (MiB) | 118.0 (117.9–118.5), 4 of 5 | 122.4 (118.6–123.4) |
| images-2000 | settle (ms) | 803.8 (779.8–898.6) | 780.3 (780.1–801.1) |
| images-2000 | idle CPU (%) | 0.0 (0.0–0.0) | 0.0 (0.0–0.0) |
| images-2000 | thumbnails (ms) | 183.9 (181.6–184.2) | 183.1 (181.3–185.3) |
| images-2000 | all visible thumbnails (ms) | 258.1 (253.5–335.4) | 233.9 (222.1–270.4) |
| images-2000 | preview (ms) | 32.1 (30.1–51.8) | 28.0 (15.9–30.5) |

Thumbnails, all visible thumbnails and preview apply only to the images
fixture. Thumbnails ends at the first ready thumbnail; all visible thumbnails
ends, after the same key press, when every file cell in the grid viewport
shows one. Settle includes the 500 ms quiet window that defines it. Its two
levels, near 600 and 780 ms, come from whether the first thumbnail pass lands
inside that window. The A→B ratios and per-run evidence remain in the report
the command writes beside its Markdown table.

A cell marked "N of 5" kept N sessions: in the others the memory observer
missed its 5 ms resolution budget while the host was busy, and the command
reports that session's RSS as missing rather than guessing it. The command
therefore marks the whole report incomplete.

The 0.2.2 release was measured on this machine with the same method just
before this run (21:43–22:12 UTC, host load median 4.30). Against it, the
window this release adds (tab strip, toolbar, places, tag bars, the details
dock and the tag, size and date columns) costs memory and first-frame time:

- GUI peak RSS rose by 2.6 to 4.0 MiB in every cell (1,000,000 files on A:
  98.1 to 100.7 MiB).
- The first complete frame on 1,000,000 files went from
  75.6 to 88.0 ms on A and from 71.3 to 92.3 ms on B; on
  100,000 files from 81.2 to 95.1 ms on A.
- Filtering 1,000,000 files went from 16.3 to 17.0 ms on A and from
  11.9 to 18.9 ms on B.
- Preview went from 34.6 to 32.1 ms on A and from
  16.2 to 28.0 ms on B.

Filtering 1,000 to 100,000 files on B got faster (9.5 to 6.2 ms on
100,000). Wheel scroll moved by less than 1 ms except on 1,000,000 files on
B (7.5 to 9.5 ms). Settle medians moved between its two levels, and idle CPU
stayed at 0.
