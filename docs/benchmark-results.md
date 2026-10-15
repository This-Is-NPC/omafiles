# Benchmark results

Medians of five fresh sessions per cell, with the minimum and maximum in
parentheses, measured by [`scripts/bench`](benchmark.md) with method 3. Each
session is a new container: latency comes from the unobserved session and RSS
from its separate memory replay.

| | |
|---|---|
| Product | the studio of `2d8e182` without its status receipt colour fix |
| Date | 2026-10-15, 06:57–07:08 UTC |
| CPU | AMD Ryzen AI 5 340, CPUs 0 (A) and 0–1 (B); memory observer on CPU 2 |
| Kernel | 7.2.5-3-omarchy, x86_64 |
| Host load | 1.7–4.0 one-minute average on the other CPUs, median 2.5 |

```sh
scripts/bench --cores 0,1 --load-threshold 4 --load-timeout 600
```

| Fixture | Metric | A: 1 CPU, 512 MB | B: 2 CPU, 1 GB |
|---|---|---:|---:|
| 1000 | first complete frame (ms) | 56.5 (56.4–111.4) | 54.3 (52.4–60.2) |
| 1000 | filter (ms) | 6.1 (5.1–10.2) | 6.1 (5.1–10.6) |
| 1000 | wheel scroll (ms) | 8.2 (7.4–8.5) | 8.4 (7.9–8.9) |
| 1000 | GUI peak RSS (MiB) | 77.5 (77.1–77.8) | 77.4 (77.2–77.5) |
| 1000 | process-tree peak RSS (MiB) | 86.4 (85.8–86.8) | 86.2 (86.0–86.4) |
| 1000 | settle (ms) | 677.6 (597.0–848.4) | 761.2 (597.7–762.9) |
| 1000 | idle CPU (%) | 0.0 (0.0–0.0) | 0.0 (0.0–0.0) |
| 10000 | first complete frame (ms) | 55.7 (53.7–57.4) | 53.0 (51.7–54.9) |
| 10000 | filter (ms) | 5.1 (4.8–8.6) | 5.4 (5.0–9.3) |
| 10000 | wheel scroll (ms) | 8.0 (7.7–8.7) | 8.3 (8.1–8.5) |
| 10000 | GUI peak RSS (MiB) | 77.6 (77.5–77.7) | 77.7 (77.4–77.9) |
| 10000 | process-tree peak RSS (MiB) | 86.4 (86.4–86.6) | 86.6 (86.3–86.9) |
| 10000 | settle (ms) | 598.1 (596.6–761.6) | 599.1 (595.9–760.9) |
| 10000 | idle CPU (%) | 0.0 (0.0–0.0) | 0.0 (0.0–0.0) |
| 100000 | first complete frame (ms) | 57.2 (53.9–57.4) | 55.0 (52.6–57.6) |
| 100000 | filter (ms) | 5.8 (4.7–9.3) | 10.2 (7.3–10.5) |
| 100000 | wheel scroll (ms) | 7.9 (7.8–8.0) | 8.1 (7.7–8.5) |
| 100000 | GUI peak RSS (MiB) | 79.4 (79.4–79.6) | 79.3 (79.3–79.5) |
| 100000 | process-tree peak RSS (MiB) | 88.4 (88.3–136.8) | 88.2 (88.1–88.4) |
| 100000 | settle (ms) | 741.7 (598.0–761.5) | 739.7 (596.3–741.8) |
| 100000 | idle CPU (%) | 0.0 (0.0–0.0) | 0.0 (0.0–0.0) |
| 1000000 | first complete frame (ms) | 57.1 (54.9–57.3) | 54.0 (52.9–54.4) |
| 1000000 | filter (ms) | 11.4 (11.2–16.5) | 10.9 (10.2–11.8) |
| 1000000 | wheel scroll (ms) | 8.0 (7.9–8.1) | 8.4 (8.2–8.6) |
| 1000000 | GUI peak RSS (MiB) | 96.5 (96.4–96.6) | 96.6 (96.2–96.9) |
| 1000000 | process-tree peak RSS (MiB) | 105.5 (105.3–137.3) | 105.5 (105.0–105.8) |
| 1000000 | settle (ms) | 763.5 (762.6–1265.8) | 762.2 (702.1–785.5) |
| 1000000 | idle CPU (%) | 0.0 (0.0–0.0) | 0.0 (0.0–0.0) |
| images-2000 | first complete frame (ms) | 54.7 (54.0–56.4) | 53.0 (52.7–53.9) |
| images-2000 | filter (ms) | 8.6 (4.7–9.3) | 5.5 (5.1–9.4) |
| images-2000 | wheel scroll (ms) | 7.9 (7.5–8.1) | 7.7 (7.2–8.6) |
| images-2000 | GUI peak RSS (MiB) | 85.3 (85.1–85.5) | 86.2 (85.9–86.3) |
| images-2000 | process-tree peak RSS (MiB) | 119.9 (119.5–120.0) | 121.1 (119.3–122.3) |
| images-2000 | settle (ms) | 761.8 (760.4–864.9) | 760.3 (760.0–762.9) |
| images-2000 | idle CPU (%) | 0.0 (0.0–0.0) | 0.0 (0.0–0.0) |
| images-2000 | thumbnails (ms) | 168.2 (168.0–169.4) | 168.0 (167.4–169.4) |
| images-2000 | preview (ms) | 36.5 (34.8–117.8) | 25.7 (25.3–27.4) |

Thumbnails and preview apply only to the images fixture. Settle includes the
500 ms quiet window that defines it. Its two levels, near 600 and 760 ms in
small folders, come from whether the first thumbnail pass lands inside that
window. The A→B ratios and per-run evidence remain in the report the command
writes beside its Markdown table.
