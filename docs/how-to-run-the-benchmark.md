# How to run the benchmark

**The question:** How do I measure omafiles performance? This page is enough on its own.

## Before you start

The benchmark measures how fast omafiles opens and lists folders of different sizes, in isolated Docker containers. Results are evidence for your machine and method, not a general claim. It requires Linux, Docker, bubblewrap, Python 3, the pinned `usage` tool, and the C++ compiler/Qt development packages used by the project.

## Run the benchmark

From the omafiles source directory:

```sh
scripts/bench
```

This builds omafiles, creates test fixtures (1,000, 10,000, 100,000 and 1,000,000 empty files, plus 2,000 test images), and measures startup, filtering, scrolling, thumbnail/preview timing, settle time and idle CPU, each on two CPU configurations. The result is a `.json.gz` file with raw data and a Markdown table beside it, written to a uniquely named path in `/tmp`.

With options:

```sh
scripts/bench --build build/bin --output /tmp/omafiles-results.json
```

- `--build <dir>` — Use prebuilt binaries from this directory instead of building
- `--output <path>` — Write results to this path (must end in `.json` or `.json.gz`; must be inside the repository or `/tmp`)
- `--image <name>` — Use a different, already-installed Docker image (default: `omafiles-t02:local`); images are never pulled or built automatically
- `--cores 0,1` — Pin to two specific permitted CPU IDs (default: auto-select two distinct physical cores from topology)
- `--load-threshold 2.0` — Wait for host load average below this value (default: half the permitted CPU count)
- `--load-timeout 600` — Seconds to wait for the load threshold (default: 600)
- `--fixtures 100000 images-2000` — Restrict the run to specific fixtures (default: all five — `1000`, `10000`, `100000`, `images-2000`, `1000000`); a partial matrix does not establish performance for omitted cases
- `--latency-only` — Disable the independent RSS observer for a diagnostic session; no memory metrics are produced

## Understanding the results

The output file contains:

- **Raw per-run data** — JSON with CPU, memory, filesystem and operation details for each measurement
- **Markdown table** — Human-readable results showing median latency and memory for each operation and configuration
- **Configuration notes** — CPU quota, affinity, memory limits and other constraints for reproducibility

The table shows:

- **frame_ms** — Time to launch and render the first frame
- **filter_ms** — Time to filter a large folder by typing a filename
- **wheel_ms** — Time to scroll a large folder
- **settle_ms** — Time from launch until the listing stays quiet for 500 ms
- **idle_cpu_percent** — CPU activity while the window is idle
- **peak_gui_rss_mib** — Peak resident memory of the GUI process alone
- **peak_tree_rss_mib** — Peak resident memory summed across the app and its descendant processes
- **thumbnail_ms** — Time to show the first ready thumbnail in the images fixture
- **preview_ms** — Time to show a decoded preview in the images fixture

Results are shown separately for the first run and the median of subsequent runs, on both single-CPU (A) and dual-CPU (B) configurations. See [GUI benchmark](benchmark.md) for exactly what completes each metric and its limitations.

## What can go wrong

- **Docker image inspection fails**: The `omafiles-t02:local` image (or the one named with `--image`) is not installed; it is only inspected, never pulled or built. Check `docker images` and install the image first.
- **`High-resolution memory measurement needs an allowed physical core outside A/B`**: The system does not have enough uncontested physical CPU cores for independent memory observation. Use `--cores` to choose which CPUs the A/B configurations use, freeing another physical core, or disable other processes.
- **`Host load N did not fall below THRESHOLD; no batch started`**: The system is too busy. The benchmark waits for the one-minute load average to drop below `--load-threshold`, then refuses that batch after `--load-timeout` seconds. Wait for other processes to finish, or raise `--load-threshold`.
- **`Container failed: exit CODE; OOMKilled=True; ...`**: The container was killed, often OOM on the million-file fixture. Try increasing host memory, or restrict the run with `--fixtures`.
- **Cleanup failed**: The benchmark could not remove a Docker resource it created; the error is logged and the ownership journal path is printed so cleanup can be retried. You can also clean up manually: `docker volume ls | grep omafiles-bench-` lists the volumes; `docker volume rm <name>` removes one.

## See also

[GUI benchmark](benchmark.md) describes the measurement method, isolation, CPU configuration and observer protocol.
[Benchmark results](benchmark-results.md) shows the latest complete matrix of measurements.
