# Memory optimization measurements

These historical tables use method 1. Their 20 ms sampled RSS values can miss
short helper peaks; they are not evidence of exact peak memory or comparable
to the corrected [method 2 memory observer](benchmark.md#high-resolution-process-tree-memory).

Official 3349 paired measurements, iteration 4.



|---|---|---:|---:|---:|---:|---:|
| A/1000 | frame_ms | 73.580 | 55.442 | -18.138 | 232.738 | -177.296 |
| A/1000 | filter_ms | 9.000 | 9.006 | +0.006 | 39.160 | -30.154 |
| A/1000 | wheel_ms | 7.576 | 7.846 | +0.270 | 28.117 | -20.271 |
| A/1000 | peak_gui_rss_mib | 99.344 | 77.562 | -21.781 | 98.980 | -21.418 |
| A/1000 | peak_tree_rss_mib | 114.398 | 92.512 | -21.887 | 105.152 | -12.641 |
| A/1000 | settle_ms | 788.844 | 763.755 | -25.089 | 914.435 | -150.680 |
| A/1000 | idle_cpu_percent | 0.000 | 0.000 | +0.000 | 0.982 | -0.982 |
| B/1000 | frame_ms | 69.838 | 54.427 | -15.411 | 216.853 | -162.426 |
| B/1000 | filter_ms | 8.770 | 7.322 | -1.448 | 38.632 | -31.310 |
| B/1000 | wheel_ms | 7.971 | 7.784 | -0.187 | 27.721 | -19.937 |
| B/1000 | peak_gui_rss_mib | 99.172 | 77.621 | -21.551 | 101.605 | -23.984 |
| B/1000 | peak_tree_rss_mib | 114.133 | 92.590 | -21.543 | 107.832 | -15.242 |
| B/1000 | settle_ms | 783.968 | 749.886 | -34.083 | 764.383 | -14.498 |
| B/1000 | idle_cpu_percent | 0.993 | 0.981 | -0.011 | 0.985 | -0.003 |
| A/10000 | frame_ms | 76.052 | 59.080 | -16.972 | 234.070 | -174.989 |
| A/10000 | filter_ms | 8.786 | 8.068 | -0.719 | 39.477 | -31.409 |
| A/10000 | wheel_ms | 8.233 | 7.757 | -0.475 | 26.674 | -18.917 |
| A/10000 | peak_gui_rss_mib | 100.156 | 77.844 | -22.312 | 98.879 | -21.035 |
| A/10000 | peak_tree_rss_mib | 115.078 | 92.816 | -22.262 | 105.562 | -12.746 |
| A/10000 | settle_ms | 802.576 | 996.688 | +194.112 | 792.334 | +204.354 |
| A/10000 | idle_cpu_percent | 0.000 | 0.000 | +0.000 | 0.994 | -0.994 |
| B/10000 | frame_ms | 71.211 | 55.464 | -15.746 | 218.930 | -163.466 |
| B/10000 | filter_ms | 9.055 | 8.747 | -0.308 | 38.897 | -30.150 |
| B/10000 | wheel_ms | 8.511 | 8.124 | -0.388 | 27.121 | -18.997 |
| B/10000 | peak_gui_rss_mib | 100.008 | 77.586 | -22.422 | 101.633 | -24.047 |
| B/10000 | peak_tree_rss_mib | 114.961 | 92.520 | -22.441 | 107.973 | -15.453 |
| B/10000 | settle_ms | 923.958 | 766.986 | -156.972 | 787.525 | -20.539 |
| B/10000 | idle_cpu_percent | 0.000 | 0.981 | +0.981 | 0.984 | -0.004 |
| A/100000 | frame_ms | 100.608 | 65.217 | -35.392 | 235.909 | -170.692 |
| A/100000 | filter_ms | 21.979 | 12.359 | -9.620 | 39.614 | -27.255 |
| A/100000 | wheel_ms | 8.808 | 8.049 | -0.758 | 28.240 | -20.191 |
| A/100000 | peak_gui_rss_mib | 104.156 | 79.559 | -24.598 | 98.820 | -19.262 |
| A/100000 | peak_tree_rss_mib | 119.148 | 94.508 | -24.641 | 107.816 | -13.309 |
| A/100000 | settle_ms | 852.065 | 751.356 | -100.709 | 860.891 | -109.535 |
| A/100000 | idle_cpu_percent | 0.000 | 0.000 | +0.000 | 0.992 | -0.992 |
| B/100000 | frame_ms | 83.113 | 59.578 | -23.535 | 220.266 | -160.688 |
| B/100000 | filter_ms | 18.601 | 10.442 | -8.159 | 39.370 | -28.928 |
| B/100000 | wheel_ms | 7.996 | 8.254 | +0.259 | 27.449 | -19.195 |
| B/100000 | peak_gui_rss_mib | 106.551 | 79.559 | -26.992 | 101.582 | -22.023 |
| B/100000 | peak_tree_rss_mib | 121.441 | 94.516 | -26.926 | 110.387 | -15.871 |
| B/100000 | settle_ms | 842.132 | 660.276 | -181.856 | 774.150 | -113.875 |
| B/100000 | idle_cpu_percent | 0.000 | 0.987 | +0.987 | 0.997 | -0.010 |
| A/images-2000 | frame_ms | 72.879 | 60.441 | -12.437 | 236.917 | -176.475 |
| A/images-2000 | filter_ms | 8.924 | 11.182 | +2.258 | 41.249 | -30.067 |
| A/images-2000 | wheel_ms | 8.549 | 8.167 | -0.382 | 27.830 | -19.663 |
| A/images-2000 | peak_gui_rss_mib | 119.613 | 99.961 | -19.652 | 108.773 | -8.812 |
| A/images-2000 | peak_tree_rss_mib | 175.441 | 160.832 | -14.609 | 120.363 | +40.469 |
| A/images-2000 | settle_ms | 782.510 | 830.880 | +48.370 | 993.034 | -162.155 |
| A/images-2000 | idle_cpu_percent | 0.000 | 0.000 | +0.000 | 0.992 | -0.992 |
| A/images-2000 | thumbnail_ms | 173.252 | 171.654 | -1.598 | unavailable | unranked |
| A/images-2000 | preview_ms | 117.057 | 116.356 | -0.701 | 58.731 | +57.625 |
| B/images-2000 | frame_ms | 71.961 | 59.050 | -12.911 | 214.593 | -155.543 |
| B/images-2000 | filter_ms | 7.641 | 9.892 | +2.252 | 40.699 | -30.806 |
| B/images-2000 | wheel_ms | 7.933 | 7.426 | -0.507 | 28.531 | -21.104 |
| B/images-2000 | peak_gui_rss_mib | 118.273 | 99.176 | -19.098 | 110.652 | -11.477 |
| B/images-2000 | peak_tree_rss_mib | 171.512 | 155.703 | -15.809 | 117.387 | +38.316 |
| B/images-2000 | settle_ms | 1066.641 | 767.074 | -299.567 | 1074.824 | -307.750 |
| B/images-2000 | idle_cpu_percent | 0.000 | 0.982 | +0.982 | 0.983 | -0.001 |
| B/images-2000 | thumbnail_ms | 166.002 | 164.814 | -1.188 | unavailable | unranked |
| B/images-2000 | preview_ms | 59.384 | 64.618 | +5.234 | 42.151 | +22.467 |
| A/1000000 | frame_ms | 159.705 | 170.235 | +10.529 | 228.525 | -58.290 |
| A/1000000 | filter_ms | 86.162 | 42.536 | -43.626 | 38.750 | +3.786 |
| A/1000000 | wheel_ms | 8.017 | 7.437 | -0.579 | 27.694 | -20.257 |
| A/1000000 | peak_gui_rss_mib | 191.270 | 96.695 | -94.574 | 98.875 | -2.180 |
| A/1000000 | peak_tree_rss_mib | 206.145 | 111.773 | -94.371 | 130.977 | -19.203 |
| A/1000000 | settle_ms | 2035.473 | 1221.976 | -813.497 | 1136.559 | +85.416 |
| A/1000000 | idle_cpu_percent | 0.000 | 0.000 | +0.000 | 0.000 | +0.000 |
| B/1000000 | frame_ms | 150.220 | 140.951 | -9.268 | 214.632 | -73.680 |
| B/1000000 | filter_ms | 84.536 | 37.887 | -46.648 | 38.393 | -0.505 |
| B/1000000 | wheel_ms | 7.698 | 7.712 | +0.014 | 27.512 | -19.800 |
| B/1000000 | peak_gui_rss_mib | 161.004 | 96.930 | -64.074 | 101.598 | -4.668 |
| B/1000000 | peak_tree_rss_mib | 175.926 | 111.879 | -64.047 | 133.410 | -21.531 |
| B/1000000 | settle_ms | 1378.763 | 1068.129 | -310.634 | 919.819 | +148.310 |
| B/1000000 | idle_cpu_percent | 0.992 | 0.000 | -0.992 | 0.991 | -0.991 |

## Settling interpretation


A buffered trace of the final executable found no tooltip events in ten A/B runs and no late application timer, thumbnail, provider, animation or window update after about 200 ms. In a slow run, the observer polling callback crossed an accounted CPU tick at 708.545 ms after launch: process CPU increased from 99.981932 to 100.020104 ms, while main-thread CPU increased from 68.942566 to 68.979034 ms. Between 250 and 1400 ms, all 171 recorded callbacks were observer polls or event-loop observations.

