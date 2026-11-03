# Benchmark results

Medians of five fresh sessions per cell, with the minimum and maximum in
parentheses, measured by [`scripts/bench`](benchmark.md) with method 4.
Latency and RSS use separate fresh sessions: latency comes from the session
without the memory observer, and RSS from its separate memory replay, whose
timings are diagnostic only.

| | |
|---|---|
| Product | `162a1d8` |
| Date | 2026-11-03, 18:29–18:43 UTC |
| CPU | AMD Ryzen AI 5 340 w/ Radeon 840M, CPUs 0 (A) and 0–1 (B); memory observer on CPU 2 |
| Kernel | 7.2.8-9-omarchy, x86_64 |
| Host load | 1.83–5.98 one-minute host-wide average when each of the 100 sessions started, median 3.63 |

```sh
scripts/bench --output /tmp/omafiles-bench/head-result.json.gz
```

The defaults chose CPUs 0 and 1, a load threshold of 6.0 and the memory
observer on CPU 2.

| Fixture | Metric | A: 1 CPU, 512 MB | B: 2 CPU, 1 GB |
|---|---|---:|---:|
| 1000 | first complete frame (ms) | 62.9 (59.5–76.4) | 59.9 (57.0–133.3) |
| 1000 | filter (ms) | 9.4 (6.5–16.5) | 6.1 (5.0–7.1) |
| 1000 | wheel scroll (ms) | 8.2 (7.6–9.0) | 7.9 (7.1–8.4) |
| 1000 | GUI peak RSS (MiB) | 78.9 (78.7–78.9) | 78.9 (78.5–79.1) |
| 1000 | process-tree peak RSS (MiB) | 87.8 (87.6–87.9) | 87.8 (87.5–88.1) |
| 1000 | settle (ms) | 623.2 (597.5–785.3) | 686.7 (595.1–761.9) |
| 1000 | idle CPU (%) | 0.0 (0.0–0.0) | 0.0 (0.0–0.0) |
| 10000 | first complete frame (ms) | 61.4 (59.4–117.4) | 57.3 (56.3–58.0) |
| 10000 | filter (ms) | 8.5 (5.4–15.4) | 7.0 (5.8–11.3) |
| 10000 | wheel scroll (ms) | 8.3 (7.5–8.8) | 7.9 (7.6–9.1) |
| 10000 | GUI peak RSS (MiB) | 79.0 (78.6–79.5) | 79.1 (79.0–79.2) |
| 10000 | process-tree peak RSS (MiB) | 88.0 (87.5–88.4) | 88.1 (87.9–88.1) |
| 10000 | settle (ms) | 764.6 (599.6–831.4) | 761.0 (595.2–762.5) |
| 10000 | idle CPU (%) | 0.0 (0.0–0.0) | 0.0 (0.0–0.0) |
| 100000 | first complete frame (ms) | 75.1 (65.8–107.9) | 69.4 (59.4–107.7) |
| 100000 | filter (ms) | 9.7 (9.5–12.3) | 9.7 (6.1–16.2) |
| 100000 | wheel scroll (ms) | 8.1 (7.8–8.5) | 7.7 (7.5–9.0) |
| 100000 | GUI peak RSS (MiB) | 80.7 (80.5–81.0) | 81.0 (80.7–81.2) |
| 100000 | process-tree peak RSS (MiB) | 89.8 (89.6–142.1) | 90.0 (89.5–142.2) |
| 100000 | settle (ms) | 619.2 (617.6–806.1) | 779.5 (682.8–785.6) |
| 100000 | idle CPU (%) | 0.0 (0.0–0.0) | 0.0 (0.0–0.0) |
| 1000000 | first complete frame (ms) | 69.9 (67.5–71.5) | 60.6 (59.1–64.2) |
| 1000000 | filter (ms) | 14.3 (11.6–16.1) | 11.7 (9.9–18.6) |
| 1000000 | wheel scroll (ms) | 8.0 (7.2–8.6) | 8.0 (7.7–9.7) |
| 1000000 | GUI peak RSS (MiB) | 98.1 (97.9–98.3) | 98.1 (97.9–98.4) |
| 1000000 | process-tree peak RSS (MiB) | 107.0 (106.9–142.7) | 107.0 (107.0–107.5) |
| 1000000 | settle (ms) | 802.5 (782.6–803.6) | 761.7 (724.7–868.3) |
| 1000000 | idle CPU (%) | 0.0 (0.0–0.0) | 0.0 (0.0–0.0) |
| images-2000 | first complete frame (ms) | 62.8 (60.9–119.6) | 59.3 (58.2–79.4) |
| images-2000 | filter (ms) | 14.5 (5.4–22.0) | 7.3 (6.0–10.3) |
| images-2000 | wheel scroll (ms) | 8.6 (7.1–9.5) | 7.7 (6.9–8.6) |
| images-2000 | GUI peak RSS (MiB) | 87.4 (87.2–87.5) | 87.8 (87.5–88.0) |
| images-2000 | process-tree peak RSS (MiB) | 123.6 (123.4–125.1) | 125.0 (114.2–125.9) |
| images-2000 | settle (ms) | 783.9 (781.2–965.5) | 780.2 (761.7–805.8) |
| images-2000 | idle CPU (%) | 0.0 (0.0–0.0) | 0.0 (0.0–0.0) |
| images-2000 | thumbnails (ms) | 170.6 (169.4–177.0) | 171.4 (171.2–172.1) |
| images-2000 | all visible thumbnails (ms) | 235.4 (230.8–339.2) | 206.7 (205.3–214.0) |
| images-2000 | preview (ms) | 33.9 (31.7–62.3) | 18.7 (16.3–23.1) |

Thumbnails, all visible thumbnails and preview apply only to the images
fixture. Thumbnails ends at the first ready thumbnail; all visible thumbnails
ends, after the same key press, when every file cell in the grid viewport
shows one. Settle includes the 500 ms quiet window that defines it. Its two
levels, near 600 and 780 ms, come from whether the first thumbnail pass lands
inside that window. The A→B ratios and per-run evidence remain in the report
the command writes beside its Markdown table.

Since `01ae599`, measured on this machine with the same method, the median
filter on 1,000,000 files fell from 27.3 to 14.3 ms on A and from 22.9 to
11.7 ms on B; preview fell from 52.0 to 33.9 ms on A and from 35.4 to 18.7 ms
on B; the first complete frame on 1,000,000 files fell from 79.9 to 69.9 ms on
A and from 78.9 to 60.6 ms on B.
