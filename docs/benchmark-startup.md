# Directory startup measurements on `306ceb9`

These historical tables use method 1. Their 20 ms sampled RSS values can miss
short helper peaks; they are not evidence of exact peak memory or comparable
to the corrected [method 2 memory observer](benchmark.md#high-resolution-process-tree-memory).

The directory reader borrows ASCII names from `dirent` and copies them directly
into the immutable listing's compact name blocks. Other names retain strict
UTF-8 validation and use a decoded string view. The index prepares its own data
and configuration directories before watching sources, avoiding initialization
notifications that cancel an in-flight directory scan. External index and root
creation/replacement remain observed.

## Method and identities

Before is the clean `306ceb9840cc823d17ab2e393a19c8f0bbfc212c` archive; after is
that source plus the directory visitor and startup-watch changes. Both product
build directories started empty, with private HOME/XDG/toolchain directories and
two build jobs. Products were frozen before measurement. No builds or tests ran
alongside either matrix. The before matrix ran immediately before the after
matrix. Each contains all 50 observations: five repetitions of every fixture in
A (one CPU, 512 MiB) and B (two CPUs, 1 GiB), with swap capped at memory, no
network, and `--cores 0,1 --load-threshold 4 --load-timeout 600`.

`96a709119350b8be1e721141c2ad428862b7d3b3e439e91c766fd199248917ff`.
The observer, harness and baseline were not changed. Both matrices completed
with zero OOM kills and no cleanup errors. Exit 1 reports the harness's full
baseline comparison; it does not mean observations were lost. In particular,

| Product | Before SHA256 | After SHA256 |
|---|---|---|
| omafiles | `67114fb15d4983e1bb9a5bccaec7673f120b0352a860e06cb143df60bdb82c4a` | `62e33bbf32ed8b1e9f1324010c3236de61de6037871f6d2f12c91060e792b28b` |
| omafiles-studio | `485f1ee0a906ce6dde9df0087c04e5a1991573c9dc1ee1fce39ea553a14ccea5` | `c60840e7982d103ce11c9eb11d94a1e39cf59e0749e6ebe9c194445b36ca67b2` |
| omafiles-preview | `1b91259e95c579f99296ca20f993ad5aad6e2e435cf1e8580065bb8acbca845c` | `1b91259e95c579f99296ca20f993ad5aad6e2e435cf1e8580065bb8acbca845c` |
| omafiles-archive | `057996d291b85813f712256dc23416da400393f7185c1811e51d1ca38ae5a584` | `057996d291b85813f712256dc23416da400393f7185c1811e51d1ca38ae5a584` |
| omafiles-devices | `e717ee71f972be80ccd02c90712c18e66c03648cf0ad1ceebbcad27e3bf060d6` | `e717ee71f972be80ccd02c90712c18e66c03648cf0ad1ceebbcad27e3bf060d6` |

## Result and limits

B/10k settle rises 55.674 ms in overlapping fast/slow samples; B/images preview
rises 8.097 ms and sampled tree RSS rises 17.379 MiB in the forward pair.
These increases were investigated rather than omitted.

An unchanged-binary, uninstrumented images-only countercheck runs in reverse
order (after, then before), five repetitions in A and B with the same flags.
B preview reverses to 29.279 ms after versus 35.463 before. A preview increases
from 48.831 to 54.042 ms in this countercheck, while the complete forward pair
has the opposite direction. Both modes occur within the same binary; these
samples do not establish a repeatable latency regression or formal statistical
equivalence. The complete forward matrix remains the official comparison.

Twelve interleaved diagnostic B sessions on the same frozen binaries use a
native preload outside the product and harness. All phases use CLOCK_MONOTONIC.
Before preview spans 26.358–37.079 ms and after spans 26.875–34.910 ms. Comparing
fast and slow runs of the *same* before binary, request-to-helper-main increases
from 14.702 to 21.553 ms, while packet-to-frame increases from 5.912 to 8.263 ms.
The image decode itself takes about 0.3 ms. Helper main CPU work is approximately
5.5–6.0 ms in both entrants. This locates variation in process launch/scheduling,
packet delivery and frame phase, rather than added directory-reader work.
Instrumentation can perturb phase alignment; these are attribution observations,
not replacement performance measurements.

Helper `getrusage(RUSAGE_SELF).ru_maxrss` is 31.965–32.270 MiB before and
31.609–31.766 MiB after in those diagnostic sessions. In one before session the
sampled tree peak is only 106.602 MiB despite the helper reaching 31.965 MiB;
another before session samples 124.754 MiB. Approximately 20 ms sampling can
miss a short-lived helper peak, and instantaneous summed RSS counts shared pages
more than once. Helper high-water values are not additive concurrent tree peaks.
Together with lower GUI RSS and the reverse-order samples, this supplies no
evidence of increased retained product memory. Sampled positive deltas remain
in both tables; the sampling interval is unchanged.

An initial complete candidate matrix missed B/1M settle (946.202 ms versus
3352, comment 197770, and the original JSON artifacts. Native paired traces then
identified two scan starts about 60 ms apart after initialization notifications
on XDG parent directories; the second candidate prepares owned directories
before installing watches. The first matrix's B/1M frame increase was also
retained: the unchanged before executable measures 91.285 ms there and 132.301
ms in this pair. Native registration of the 1M directory watch itself takes
79–98 ms on the GUI thread in both entrants, before the directory reader starts.
This shared cost is not removed by the optimization.

## Work and validation

Tests assert borrowed byte aliases, encoding, retained copies, strict UTF-8,
leading BOM, invalid names, early termination, descriptor ownership, mixed-name
ordering, cancellation and bounded compact storage. Native watch registrations
cover fresh, data-ready, config-ready and both-ready state, requiring the owned
directories and excluding unrelated XDG parents. Existing fresh-index and
external-root creation/replacement tests remain enabled. Assertions observe
completed work and registrations, without timing an idle interval.

| Mutation | Result |
|---|---|
| Decode ASCII into owning UTF16 | Both Store data rows reject encoding/alias |
| Copy ASCII bytes before visiting | Both Store data rows reject alias |
| Bypass strict UTF-8 validation | Both Store data rows reject names |
| Ignore visitor early termination | Both Store data rows reject visit count |
| Decode ASCII again in the Studio listing | Storage byte/capacity assertions fail |
| Corrupt the compact name copy | Name/link correspondence and file existence fail |
| Restore initial reload before directory creation | Three watch-state rows fail |
| Prepare only the data directory | Two watch-state rows fail |
| Prepare only the configuration directory | Four focused cases fail |

The restored focused watch suite passes nine cases. The final normal-environment
gate exits 0: Studio 955 passed, zero failed, one intended skip; Store 70;
Portal 20; observer 8; Python, incremental build, usage, QML, screenshots and CLI
checks pass. A preceding gate failed in `refusesDropsIntoVirtualViews`, leaving a
conflict sheet and causing 579 subsequent failures. That unresolved intermittent
failure is tracked separately in task 3450, with historical pre-optimization
sightings. A clean-base full Studio run passes 951 cases with one intended skip;
the unchanged candidate's repeated full gate passes. No drop-test repair or
weakened assertion is included here.

Research: [Qt string-view lifetime and encodings](https://doc.qt.io/qt-6/qanystringview.html),
[borrowed byte arrays](https://doc.qt.io/qt-6/qbytearray.html#fromRawData), and
[filesystem watch registration and notifications](https://doc.qt.io/qt-6/qfilesystemwatcher.html).

## Complete official forward pair

A/B, five repetitions; --cores 0,1 --load-threshold 4 --load-timeout 600. One matrix at a time. No observations discarded. JSON retains full precision; table displays three decimals.

### frame_ms

|---|---:|---:|---:|---:|---|
| A/1000 | 54.936 | 57.159 | 236.044 | 2.223 | 5/5/5 |
| B/1000 | 52.957 | 53.878 | 215.903 | 0.921 | 5/5/5 |
| A/10000 | 58.941 | 58.063 | 233.677 | -0.878 | 5/5/5 |
| B/10000 | 55.910 | 55.251 | 222.939 | -0.659 | 5/5/5 |
| A/100000 | 68.660 | 69.856 | 233.658 | 1.197 | 5/5/5 |
| B/100000 | 65.095 | 64.843 | 229.754 | -0.252 | 5/5/5 |
| A/images-2000 | 57.138 | 56.640 | 242.577 | -0.498 | 5/5/5 |
| B/images-2000 | 55.886 | 53.161 | 218.218 | -2.726 | 5/5/5 |
| A/1000000 | 137.762 | 138.113 | 236.659 | 0.352 | 5/5/5 |
| B/1000000 | 132.301 | 134.369 | 219.601 | 2.068 | 5/5/5 |

|---|---|---|---|
| A/1000 | [55.818, 54.260, 54.274, 54.936, 55.037] | [55.055, 55.057, 57.159, 60.099, 60.103] | [241.319, 258.042, 228.546, 229.842, 236.044] |
| B/1000 | [52.924, 53.901, 53.120, 51.936, 52.957] | [53.878, 53.216, 54.028, 54.646, 52.794] | [217.797, 215.903, 217.908, 214.254, 215.878] |
| A/10000 | [59.717, 58.941, 76.642, 58.842, 58.735] | [58.063, 57.600, 58.004, 58.421, 58.874] | [229.412, 230.324, 233.677, 254.652, 248.915] |
| B/10000 | [56.014, 63.551, 55.240, 55.729, 55.910] | [55.251, 54.396, 53.523, 56.926, 55.930] | [216.514, 278.927, 222.939, 227.085, 222.652] |
| A/100000 | [69.942, 68.660, 68.489, 69.656, 67.801] | [70.310, 69.856, 68.829, 69.528, 70.226] | [237.924, 232.945, 253.451, 233.658, 231.410] |
| B/100000 | [64.274, 65.095, 65.965, 65.637, 63.212] | [64.843, 63.786, 66.613, 64.213, 87.415] | [226.569, 232.099, 264.977, 223.278, 229.754] |
| A/images-2000 | [59.673, 92.029, 56.424, 57.138, 55.399] | [60.115, 54.522, 57.088, 54.993, 56.640] | [236.292, 242.934, 242.741, 232.523, 242.577] |
| B/images-2000 | [56.071, 52.445, 55.886, 61.013, 54.981] | [53.161, 53.161, 53.363, 54.055, 52.448] | [218.218, 222.688, 214.914, 214.693, 218.412] |
| A/1000000 | [140.603, 136.461, 138.798, 137.339, 137.762] | [138.113, 137.943, 138.162, 137.274, 140.909] | [236.659, 242.733, 228.272, 241.073, 233.317] |
| B/1000000 | [132.301, 134.303, 134.645, 131.850, 131.159] | [139.325, 133.754, 138.170, 133.920, 134.369] | [213.760, 220.153, 268.790, 219.601, 218.300] |

### filter_ms

|---|---:|---:|---:|---:|---|
| A/1000 | 7.113 | 5.984 | 38.462 | -1.130 | 5/5/5 |
| B/1000 | 6.986 | 5.805 | 38.838 | -1.181 | 5/5/5 |
| A/10000 | 7.255 | 5.945 | 38.917 | -1.310 | 5/5/5 |
| B/10000 | 7.793 | 5.243 | 40.213 | -2.550 | 5/5/5 |
| A/100000 | 7.307 | 7.891 | 40.369 | 0.584 | 5/5/5 |
| B/100000 | 8.661 | 9.788 | 40.746 | 1.127 | 5/5/5 |
| A/images-2000 | 7.651 | 7.010 | 40.075 | -0.641 | 5/5/5 |
| B/images-2000 | 8.131 | 5.937 | 41.474 | -2.194 | 5/5/5 |
| A/1000000 | 16.160 | 12.039 | 39.347 | -4.121 | 5/5/5 |
| B/1000000 | 13.280 | 12.969 | 38.718 | -0.310 | 5/5/5 |

|---|---|---|---|
| A/1000 | [7.113, 7.313, 7.002, 8.462, 6.902] | [8.684, 5.381, 8.319, 5.561, 5.984] | [39.498, 38.936, 37.633, 38.269, 38.462] |
| B/1000 | [6.587, 6.381, 9.098, 7.144, 6.986] | [5.805, 5.264, 5.283, 8.693, 5.995] | [38.838, 40.581, 37.434, 40.262, 38.646] |
| A/10000 | [7.107, 11.077, 6.358, 7.255, 7.458] | [6.000, 5.344, 5.945, 5.850, 7.445] | [38.368, 37.602, 38.917, 40.173, 62.833] |
| B/10000 | [7.793, 8.729, 7.991, 6.554, 6.946] | [5.243, 9.525, 5.263, 4.197, 5.147] | [37.602, 37.909, 40.905, 41.283, 40.213] |
| A/100000 | [9.644, 6.033, 7.307, 5.933, 7.654] | [6.125, 4.642, 10.204, 9.275, 7.891] | [43.005, 43.456, 40.369, 40.117, 39.144] |
| B/100000 | [8.813, 9.159, 7.758, 8.661, 8.090] | [9.999, 6.366, 9.788, 12.508, 9.464] | [40.746, 38.929, 41.521, 38.387, 41.163] |
| A/images-2000 | [7.407, 11.715, 7.651, 7.280, 8.950] | [7.391, 4.955, 5.872, 7.318, 7.010] | [40.075, 40.717, 65.825, 39.277, 39.746] |
| B/images-2000 | [8.957, 7.229, 8.131, 7.239, 8.842] | [6.542, 5.908, 5.678, 8.824, 5.937] | [41.792, 41.474, 74.992, 39.724, 41.279] |
| A/1000000 | [15.960, 16.444, 16.315, 12.647, 16.160] | [11.954, 12.039, 11.895, 13.480, 14.073] | [39.452, 40.217, 38.861, 39.347, 38.543] |
| B/1000000 | [13.280, 16.942, 11.191, 12.260, 16.662] | [10.894, 11.500, 12.969, 13.295, 15.837] | [37.508, 40.339, 38.718, 40.430, 37.418] |

### wheel_ms

|---|---:|---:|---:|---:|---|
| A/1000 | 8.107 | 7.769 | 27.868 | -0.338 | 5/5/5 |
| B/1000 | 8.459 | 7.814 | 27.608 | -0.645 | 5/5/5 |
| A/10000 | 8.071 | 8.114 | 28.043 | 0.043 | 5/5/5 |
| B/10000 | 8.210 | 7.979 | 27.562 | -0.230 | 5/5/5 |
| A/100000 | 7.886 | 7.621 | 27.553 | -0.265 | 5/5/5 |
| B/100000 | 8.403 | 8.410 | 27.908 | 0.007 | 5/5/5 |
| A/images-2000 | 7.654 | 8.072 | 28.287 | 0.418 | 5/5/5 |
| B/images-2000 | 8.111 | 7.900 | 27.671 | -0.211 | 5/5/5 |
| A/1000000 | 7.613 | 8.341 | 27.407 | 0.728 | 5/5/5 |
| B/1000000 | 8.290 | 7.977 | 26.986 | -0.312 | 5/5/5 |

|---|---|---|---|
| A/1000 | [8.107, 8.148, 8.085, 6.783, 8.322] | [7.769, 7.414, 7.987, 8.326, 7.755] | [27.445, 27.526, 28.169, 27.868, 28.746] |
| B/1000 | [8.432, 8.459, 8.769, 7.768, 8.524] | [7.028, 8.434, 7.733, 7.814, 8.506] | [28.441, 27.957, 27.195, 26.188, 27.608] |
| A/10000 | [7.972, 8.101, 8.071, 8.516, 7.833] | [8.219, 7.958, 7.578, 8.114, 8.153] | [29.714, 28.043, 27.592, 27.598, 29.969] |
| B/10000 | [8.210, 7.545, 6.998, 8.614, 8.344] | [8.578, 7.647, 7.979, 7.884, 8.747] | [27.054, 27.651, 27.674, 27.268, 27.562] |
| A/100000 | [7.975, 7.654, 8.302, 7.703, 7.886] | [7.859, 7.621, 7.437, 7.624, 7.605] | [29.264, 27.908, 26.957, 27.014, 27.553] |
| B/100000 | [8.706, 7.995, 7.744, 8.441, 8.403] | [7.837, 8.534, 8.410, 9.657, 7.322] | [26.773, 28.408, 27.842, 27.908, 28.367] |
| A/images-2000 | [8.271, 8.132, 7.654, 7.114, 7.578] | [8.107, 8.007, 8.072, 7.625, 8.099] | [28.287, 26.111, 29.610, 29.126, 27.735] |
| B/images-2000 | [8.111, 8.808, 7.911, 8.279, 7.607] | [7.669, 8.956, 7.914, 7.739, 7.900] | [27.671, 27.186, 32.325, 27.043, 28.479] |
| A/1000000 | [7.493, 7.613, 7.562, 8.154, 8.302] | [8.341, 8.091, 9.080, 8.394, 7.318] | [26.665, 28.943, 26.924, 28.952, 27.407] |
| B/1000000 | [8.290, 8.269, 8.449, 8.900, 7.913] | [7.977, 7.947, 8.029, 8.087, 7.843] | [26.474, 27.800, 27.752, 26.986, 25.816] |

### peak_gui_rss_mib

|---|---:|---:|---:|---:|---|
| A/1000 | 77.945 | 77.367 | 99.207 | -0.578 | 5/5/5 |
| B/1000 | 77.609 | 77.148 | 101.977 | -0.461 | 5/5/5 |
| A/10000 | 78.039 | 77.730 | 98.984 | -0.309 | 5/5/5 |
| B/10000 | 77.871 | 77.410 | 101.652 | -0.461 | 5/5/5 |
| A/100000 | 79.852 | 79.266 | 98.957 | -0.586 | 5/5/5 |
| B/100000 | 79.734 | 79.215 | 101.922 | -0.520 | 5/5/5 |
| A/images-2000 | 85.973 | 85.688 | 109.520 | -0.285 | 5/5/5 |
| B/images-2000 | 85.906 | 85.281 | 113.199 | -0.625 | 5/5/5 |
| A/1000000 | 96.988 | 96.680 | 98.949 | -0.309 | 5/5/5 |
| B/1000000 | 97.137 | 96.367 | 101.734 | -0.770 | 5/5/5 |

|---|---|---|---|
| A/1000 | [77.652, 77.945, 78.137, 78.008, 77.773] | [77.496, 77.148, 77.484, 77.367, 77.152] | [99.207, 98.914, 99.367, 99.254, 98.977] |
| B/1000 | [77.539, 77.609, 77.559, 77.762, 77.707] | [77.102, 77.008, 77.328, 77.352, 77.148] | [101.723, 102.082, 102.008, 101.977, 101.422] |
| A/10000 | [78.039, 77.508, 78.082, 77.746, 78.133] | [77.730, 77.496, 77.738, 77.699, 77.750] | [99.133, 98.957, 99.121, 98.961, 98.984] |
| B/10000 | [77.781, 77.680, 78.219, 77.871, 78.051] | [77.316, 77.668, 77.258, 77.586, 77.410] | [101.414, 101.652, 101.465, 101.859, 101.688] |
| A/100000 | [79.852, 79.938, 79.688, 79.879, 79.664] | [79.820, 79.328, 79.086, 79.039, 79.266] | [99.445, 98.957, 98.793, 99.125, 98.625] |
| B/100000 | [79.648, 80.016, 79.559, 79.742, 79.734] | [79.312, 79.215, 78.965, 78.965, 79.520] | [101.984, 102.125, 101.406, 101.922, 101.215] |
| A/images-2000 | [86.281, 85.973, 85.895, 86.219, 85.668] | [85.785, 85.812, 85.449, 85.477, 85.688] | [109.441, 110.008, 109.520, 109.293, 109.559] |
| B/images-2000 | [85.973, 85.656, 85.906, 85.551, 86.090] | [85.281, 85.371, 85.594, 85.203, 85.078] | [111.039, 113.199, 115.496, 115.156, 112.559] |
| A/1000000 | [96.824, 97.051, 96.934, 96.988, 97.246] | [96.703, 96.680, 96.656, 96.559, 96.727] | [98.949, 98.902, 98.938, 98.973, 99.285] |
| B/1000000 | [96.984, 97.137, 97.160, 97.270, 96.867] | [96.422, 96.367, 96.684, 96.324, 96.242] | [101.645, 101.719, 101.734, 101.836, 101.910] |

### peak_tree_rss_mib

|---|---:|---:|---:|---:|---|
| A/1000 | 92.883 | 92.352 | 105.492 | -0.531 | 5/5/5 |
| B/1000 | 92.617 | 92.070 | 107.770 | -0.547 | 5/5/5 |
| A/10000 | 92.969 | 92.695 | 105.535 | -0.273 | 5/5/5 |
| B/10000 | 92.930 | 92.359 | 107.809 | -0.570 | 5/5/5 |
| A/100000 | 94.820 | 94.258 | 108.012 | -0.562 | 5/5/5 |
| B/100000 | 94.691 | 94.160 | 110.617 | -0.531 | 5/5/5 |
| A/images-2000 | 129.074 | 129.102 | 119.461 | 0.027 | 5/5/5 |
| B/images-2000 | 101.098 | 118.477 | 121.449 | 17.379 | 5/5/5 |
| A/1000000 | 111.867 | 111.656 | 131.184 | -0.211 | 5/5/5 |
| B/1000000 | 112.113 | 111.332 | 133.746 | -0.781 | 5/5/5 |

|---|---|---|---|
| A/1000 | [92.559, 92.883, 93.145, 92.918, 92.770] | [92.352, 92.145, 92.402, 92.375, 92.215] | [105.492, 104.863, 105.672, 105.566, 105.457] |
| B/1000 | [92.398, 92.617, 92.555, 92.660, 92.715] | [92.066, 91.977, 92.312, 92.402, 92.070] | [107.574, 108.258, 107.770, 108.129, 107.766] |
| A/10000 | [92.969, 92.449, 93.078, 92.758, 93.039] | [92.668, 92.426, 92.711, 92.699, 92.695] | [105.500, 105.535, 105.473, 107.672, 105.723] |
| B/10000 | [92.805, 92.555, 93.184, 92.930, 93.059] | [92.176, 92.660, 92.148, 92.559, 92.359] | [107.746, 107.676, 107.809, 108.695, 108.195] |
| A/100000 | [94.898, 94.820, 94.613, 94.863, 94.656] | [94.805, 94.293, 94.059, 93.922, 94.258] | [108.320, 108.012, 107.742, 108.016, 107.387] |
| B/100000 | [94.691, 94.938, 94.531, 94.711, 94.652] | [94.160, 94.176, 93.758, 93.914, 94.457] | [110.617, 111.129, 110.398, 110.668, 110.137] |
| A/images-2000 | [125.551, 129.465, 129.074, 131.426, 128.758] | [129.102, 129.273, 129.855, 129.023, 129.027] | [119.461, 120.164, 117.824, 115.824, 120.621] |
| B/images-2000 | [100.988, 100.594, 124.750, 124.992, 101.098] | [118.285, 118.004, 118.996, 125.137, 118.477] | [117.406, 119.715, 121.809, 121.449, 122.020] |
| A/1000000 | [111.734, 112.047, 111.828, 111.867, 112.137] | [111.688, 111.590, 111.656, 111.641, 111.664] | [131.113, 131.305, 131.184, 131.039, 131.566] |
| B/1000000 | [111.898, 112.129, 112.113, 112.203, 111.910] | [111.348, 111.227, 111.566, 111.301, 111.332] | [133.746, 133.371, 133.801, 133.691, 133.965] |

### settle_ms

|---|---:|---:|---:|---:|---|
| A/1000 | 629.372 | 606.565 | 1258.379 | -22.807 | 5/5/5 |
| B/1000 | 646.233 | 632.121 | 750.436 | -14.112 | 5/5/5 |
| A/10000 | 674.257 | 586.075 | 791.972 | -88.182 | 5/5/5 |
| B/10000 | 681.353 | 737.027 | 781.826 | 55.674 | 5/5/5 |
| A/100000 | 757.492 | 631.135 | 811.425 | -126.357 | 5/5/5 |
| B/100000 | 751.469 | 753.139 | 801.963 | 1.669 | 5/5/5 |
| A/images-2000 | 777.505 | 772.405 | 900.593 | -5.100 | 5/5/5 |
| B/images-2000 | 753.025 | 753.166 | 849.821 | 0.141 | 5/5/5 |
| A/1000000 | 997.406 | 897.103 | 962.577 | -100.303 | 5/5/5 |
| B/1000000 | 1021.615 | 899.570 | 934.083 | -122.045 | 5/5/5 |

|---|---|---|---|
| A/1000 | [634.360, 626.505, 652.312, 627.520, 629.372] | [754.817, 584.300, 587.627, 756.089, 606.565] | [1265.078, 811.869, 774.461, 1258.379, 1261.553] |
| B/1000 | [751.516, 731.744, 644.268, 625.393, 646.233] | [752.929, 737.180, 586.890, 602.695, 632.121] | [750.115, 1242.110, 765.543, 750.436, 745.668] |
| A/10000 | [645.935, 634.339, 791.146, 674.257, 771.677] | [583.800, 759.122, 586.075, 594.008, 585.821] | [789.501, 773.114, 791.972, 811.508, 809.250] |
| B/10000 | [751.646, 753.978, 628.899, 681.353, 627.440] | [756.164, 754.977, 587.486, 737.027, 649.134] | [838.430, 892.296, 748.889, 767.398, 781.826] |
| A/100000 | [756.775, 756.347, 759.697, 757.806, 757.492] | [631.135, 629.942, 613.423, 644.409, 753.663] | [814.201, 802.886, 831.950, 796.109, 811.425] |
| B/100000 | [651.169, 650.425, 751.469, 755.970, 770.045] | [753.139, 624.393, 759.859, 767.028, 631.286] | [803.902, 801.963, 826.287, 783.565, 789.390] |
| A/images-2000 | [777.505, 823.032, 777.896, 761.564, 769.563] | [781.658, 782.548, 756.434, 772.405, 771.549] | [775.553, 1274.418, 900.593, 884.794, 971.607] |
| B/images-2000 | [753.025, 751.537, 750.084, 788.235, 764.303] | [753.166, 750.706, 757.441, 752.310, 768.734] | [849.821, 1257.563, 747.988, 1243.239, 751.464] |
| A/1000000 | [1029.021, 975.784, 998.368, 997.406, 969.438] | [888.853, 908.444, 897.103, 891.295, 898.007] | [944.046, 1281.222, 935.969, 962.577, 1260.723] |
| B/1000000 | [1021.615, 1020.990, 1032.614, 1023.537, 994.213] | [927.266, 873.222, 899.570, 872.650, 900.329] | [975.356, 914.172, 975.881, 934.083, 926.859] |

### idle_cpu_percent

|---|---:|---:|---:|---:|---|
| A/1000 | 0.000 | 0.000 | 0.000 | 0.000 | 5/5/5 |
| B/1000 | 0.000 | 0.000 | 0.000 | 0.000 | 5/5/5 |
| A/10000 | 0.000 | 0.000 | 0.000 | 0.000 | 5/5/5 |
| B/10000 | 0.000 | 0.000 | 0.000 | 0.000 | 5/5/5 |
| A/100000 | 0.000 | 0.000 | 0.000 | 0.000 | 5/5/5 |
| B/100000 | 0.000 | 0.000 | 0.000 | 0.000 | 5/5/5 |
| A/images-2000 | 0.000 | 0.000 | 0.000 | 0.000 | 5/5/5 |
| B/images-2000 | 0.000 | 0.000 | 0.000 | 0.000 | 5/5/5 |
| A/1000000 | 0.000 | 0.000 | 0.000 | 0.000 | 5/5/5 |
| B/1000000 | 0.000 | 0.000 | 0.000 | 0.000 | 5/5/5 |

|---|---|---|---|
| A/1000 | [0.000, 0.000, 0.000, 0.000, 0.000] | [0.000, 0.000, 0.000, 0.000, 0.000] | [0.000, 0.000, 0.000, 0.994, 0.000] |
| B/1000 | [0.000, 0.000, 0.000, 0.000, 0.000] | [0.000, 0.000, 0.000, 0.000, 0.000] | [0.000, 0.000, 0.000, 0.000, 0.000] |
| A/10000 | [0.000, 0.000, 0.000, 0.000, 0.000] | [0.000, 0.000, 0.000, 0.000, 0.000] | [0.000, 0.000, 0.000, 0.000, 0.000] |
| B/10000 | [0.000, 0.000, 0.000, 0.000, 0.000] | [0.000, 0.000, 0.000, 0.000, 0.000] | [0.000, 0.000, 0.000, 0.000, 0.000] |
| A/100000 | [0.000, 0.000, 0.000, 0.000, 0.000] | [0.000, 0.000, 0.000, 0.000, 0.000] | [0.000, 0.000, 0.000, 0.000, 0.000] |
| B/100000 | [0.000, 0.000, 0.000, 0.000, 0.000] | [0.000, 0.000, 0.000, 0.000, 0.000] | [0.000, 0.000, 0.000, 0.000, 0.000] |
| A/images-2000 | [0.000, 0.000, 0.000, 0.000, 0.000] | [0.000, 0.000, 0.000, 0.000, 0.000] | [0.000, 0.000, 0.000, 0.000, 0.000] |
| B/images-2000 | [0.000, 0.000, 0.000, 0.000, 0.000] | [0.000, 0.000, 0.000, 0.000, 0.000] | [0.000, 0.000, 0.000, 0.000, 0.000] |
| A/1000000 | [0.000, 0.000, 0.000, 0.000, 0.000] | [0.000, 0.000, 0.000, 0.000, 0.000] | [0.000, 0.000, 0.000, 0.000, 0.000] |
| B/1000000 | [0.000, 0.000, 0.000, 0.000, 0.000] | [0.000, 0.000, 0.000, 0.000, 0.000] | [0.995, 0.996, 0.000, 0.000, 0.000] |

### thumbnail_ms

|---|---:|---:|---:|---:|---|
| A/images-2000 | 168.634 | 169.147 | n/a | 0.513 | 5/5/0 |
| B/images-2000 | 168.552 | 167.400 | n/a | -1.152 | 5/5/0 |

|---|---|---|---|
| A/images-2000 | [168.634, 169.961, 168.154, 167.647, 169.773] | [171.701, 167.140, 166.776, 170.915, 169.147] | [n/a, n/a, n/a, n/a, n/a] |
| B/images-2000 | [167.641, 168.882, 168.924, 164.823, 168.552] | [169.131, 167.400, 168.335, 165.642, 166.223] | [n/a, n/a, n/a, n/a, n/a] |

### preview_ms

|---|---:|---:|---:|---:|---|
| A/images-2000 | 50.451 | 45.761 | 57.266 | -4.690 | 5/5/5 |
| B/images-2000 | 25.384 | 33.481 | 42.369 | 8.097 | 5/5/5 |

|---|---|---|---|
| A/images-2000 | [73.342, 70.629, 49.877, 49.285, 50.451] | [47.958, 45.199, 46.672, 45.169, 45.761] | [71.436, 65.951, 56.484, 57.266, 55.796] |
| B/images-2000 | [25.384, 24.687, 28.700, 25.906, 24.328] | [33.481, 35.903, 32.890, 26.143, 33.734] | [47.851, 40.871, 83.631, 42.369, 38.839] |

## Reverse-order images countercheck

These supplementary observations do not replace the complete forward pair.

### frame_ms

| Case | Before median | After median | Before raw | After raw |
|---|---:|---:|---|---|
| A/images-2000 | 57.639 | 57.540 | [59.173, 57.639, 58.357, 55.966, 55.548] | [57.540, 55.633, 55.100, 57.593, 58.994] |
| B/images-2000 | 54.124 | 55.898 | [54.914, 53.793, 54.124, 55.664, 53.030] | [55.898, 59.288, 54.971, 53.046, 57.876] |

### filter_ms

| Case | Before median | After median | Before raw | After raw |
|---|---:|---:|---|---|
| A/images-2000 | 7.784 | 5.584 | [7.151, 6.318, 8.500, 8.890, 7.784] | [5.584, 5.590, 5.218, 5.477, 9.922] |
| B/images-2000 | 7.744 | 5.968 | [7.713, 7.136, 9.756, 7.744, 9.092] | [8.687, 5.968, 5.807, 5.345, 10.557] |

### wheel_ms

| Case | Before median | After median | Before raw | After raw |
|---|---:|---:|---|---|
| A/images-2000 | 8.052 | 7.664 | [8.433, 8.373, 7.569, 8.052, 7.684] | [8.150, 7.664, 7.468, 8.381, 7.148] |
| B/images-2000 | 7.758 | 7.846 | [7.962, 7.523, 8.727, 7.758, 7.572] | [7.846, 7.947, 7.631, 8.805, 6.867] |

### peak_gui_rss_mib

| Case | Before median | After median | Before raw | After raw |
|---|---:|---:|---|---|
| A/images-2000 | 86.090 | 85.371 | [86.082, 85.746, 86.133, 86.090, 86.203] | [85.758, 85.773, 85.371, 85.355, 85.301] |
| B/images-2000 | 86.031 | 85.254 | [86.070, 86.031, 86.094, 85.977, 85.727] | [85.664, 85.441, 85.051, 85.172, 85.254] |

### peak_tree_rss_mib

| Case | Before median | After median | Before raw | After raw |
|---|---:|---:|---|---|
| A/images-2000 | 129.164 | 124.844 | [129.328, 124.039, 125.641, 130.203, 129.164] | [122.609, 129.301, 124.844, 129.152, 123.273] |
| B/images-2000 | 108.977 | 111.391 | [121.484, 119.219, 108.051, 108.977, 100.594] | [100.691, 123.953, 111.391, 104.172, 118.883] |

### settle_ms

| Case | Before median | After median | Before raw | After raw |
|---|---:|---:|---|---|
| A/images-2000 | 780.942 | 768.448 | [790.733, 786.651, 780.942, 767.959, 756.674] | [790.648, 751.162, 760.513, 768.448, 783.261] |
| B/images-2000 | 752.879 | 750.878 | [752.879, 752.747, 766.761, 749.849, 767.417] | [766.635, 750.878, 748.481, 749.498, 752.292] |

### idle_cpu_percent

| Case | Before median | After median | Before raw | After raw |
|---|---:|---:|---|---|
| A/images-2000 | 0.000 | 0.000 | [0.000, 0.000, 0.000, 0.000, 0.000] | [0.000, 0.000, 0.000, 0.000, 0.000] |
| B/images-2000 | 0.000 | 0.000 | [0.000, 0.000, 0.000, 0.000, 0.000] | [0.000, 0.000, 0.000, 0.000, 0.000] |

### thumbnail_ms

| Case | Before median | After median | Before raw | After raw |
|---|---:|---:|---|---|
| A/images-2000 | 170.561 | 170.461 | [170.561, 171.706, 171.317, 169.449, 170.451] | [189.538, 169.607, 170.731, 167.190, 170.461] |
| B/images-2000 | 169.260 | 168.619 | [170.209, 169.243, 168.965, 171.000, 169.260] | [167.888, 168.619, 168.846, 168.533, 169.188] |

### preview_ms

| Case | Before median | After median | Before raw | After raw |
|---|---:|---:|---|---|
| A/images-2000 | 48.831 | 54.042 | [48.831, 57.268, 49.036, 45.215, 48.343] | [55.093, 47.246, 54.042, 47.584, 54.060] |
| B/images-2000 | 35.463 | 29.279 | [35.463, 37.570, 34.307, 36.913, 24.371] | [21.807, 27.751, 38.180, 38.321, 29.279] |
