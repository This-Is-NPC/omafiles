# Paired preview measurements

These historical tables use method 1. Their 20 ms sampled RSS values can miss
short helper peaks; they are not evidence of exact peak memory or comparable
to the corrected [method 2 memory observer](benchmark.md#high-resolution-process-tree-memory).


## Medians


| --- | --- | ---: | ---: | ---: | ---: | ---: |
| A/1000 | frame_ms | 57.829 | 59.830 | 236.044 | 2.001 | -176.214 |
| A/1000 | filter_ms | 9.068 | 7.915 | 38.462 | -1.153 | -30.546 |
| A/1000 | wheel_ms | 7.656 | 7.547 | 27.868 | -0.110 | -20.322 |
| A/1000 | peak_gui_rss_mib | 77.750 | 77.773 | 99.207 | 0.023 | -21.434 |
| A/1000 | peak_tree_rss_mib | 92.691 | 92.750 | 105.492 | 0.059 | -12.742 |
| A/1000 | settle_ms | 658.597 | 657.160 | 1258.379 | -1.437 | -601.219 |
| A/1000 | idle_cpu_percent | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |
| B/1000 | frame_ms | 55.430 | 61.698 | 215.903 | 6.268 | -154.205 |
| B/1000 | filter_ms | 9.631 | 6.934 | 38.838 | -2.697 | -31.904 |
| B/1000 | wheel_ms | 7.557 | 7.719 | 27.608 | 0.162 | -19.890 |
| B/1000 | peak_gui_rss_mib | 77.434 | 77.633 | 101.977 | 0.199 | -24.344 |
| B/1000 | peak_tree_rss_mib | 92.395 | 92.613 | 107.770 | 0.219 | -15.156 |
| B/1000 | settle_ms | 758.219 | 654.373 | 750.436 | -103.846 | -96.064 |
| B/1000 | idle_cpu_percent | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |
| A/10000 | frame_ms | 62.706 | 68.081 | 233.677 | 5.374 | -165.596 |
| A/10000 | filter_ms | 7.359 | 8.647 | 38.917 | 1.288 | -30.271 |
| A/10000 | wheel_ms | 7.685 | 7.750 | 28.043 | 0.066 | -20.293 |
| A/10000 | peak_gui_rss_mib | 77.930 | 77.863 | 98.984 | -0.066 | -21.121 |
| A/10000 | peak_tree_rss_mib | 92.879 | 92.977 | 105.535 | 0.098 | -12.559 |
| A/10000 | settle_ms | 659.957 | 777.107 | 791.972 | 117.150 | -14.866 |
| A/10000 | idle_cpu_percent | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |
| B/10000 | frame_ms | 58.222 | 58.476 | 222.939 | 0.254 | -164.464 |
| B/10000 | filter_ms | 8.982 | 8.775 | 40.213 | -0.207 | -31.438 |
| B/10000 | wheel_ms | 7.640 | 7.559 | 27.562 | -0.081 | -20.003 |
| B/10000 | peak_gui_rss_mib | 77.848 | 77.926 | 101.652 | 0.078 | -23.727 |
| B/10000 | peak_tree_rss_mib | 92.914 | 92.859 | 107.809 | -0.055 | -14.949 |
| B/10000 | settle_ms | 658.265 | 757.571 | 781.826 | 99.306 | -24.255 |
| B/10000 | idle_cpu_percent | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |
| A/100000 | frame_ms | 64.316 | 68.454 | 233.658 | 4.138 | -165.204 |
| A/100000 | filter_ms | 5.808 | 8.849 | 40.369 | 3.041 | -31.521 |
| A/100000 | wheel_ms | 7.486 | 7.655 | 27.553 | 0.170 | -19.898 |
| A/100000 | peak_gui_rss_mib | 79.691 | 79.617 | 98.957 | -0.074 | -19.340 |
| A/100000 | peak_tree_rss_mib | 94.688 | 94.570 | 108.012 | -0.117 | -13.441 |
| A/100000 | settle_ms | 659.399 | 704.597 | 811.425 | 45.198 | -106.829 |
| A/100000 | idle_cpu_percent | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |
| B/100000 | frame_ms | 61.711 | 63.685 | 229.754 | 1.975 | -166.069 |
| B/100000 | filter_ms | 9.139 | 9.612 | 40.746 | 0.473 | -31.134 |
| B/100000 | wheel_ms | 8.065 | 7.738 | 27.908 | -0.328 | -20.170 |
| B/100000 | peak_gui_rss_mib | 79.520 | 79.531 | 101.922 | 0.012 | -22.391 |
| B/100000 | peak_tree_rss_mib | 94.449 | 94.477 | 110.617 | 0.027 | -16.141 |
| B/100000 | settle_ms | 637.496 | 755.482 | 801.963 | 117.986 | -46.481 |
| B/100000 | idle_cpu_percent | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |
| A/images-2000 | frame_ms | 57.613 | 59.393 | 242.577 | 1.779 | -183.184 |
| A/images-2000 | filter_ms | 8.781 | 8.547 | 40.075 | -0.234 | -31.528 |
| A/images-2000 | wheel_ms | 7.659 | 7.653 | 28.287 | -0.006 | -20.634 |
| A/images-2000 | peak_gui_rss_mib | 97.379 | 85.828 | 109.520 | -11.551 | -23.691 |
| A/images-2000 | peak_tree_rss_mib | 158.195 | 124.977 | 119.461 | -33.219 | 5.516 |
| A/images-2000 | settle_ms | 781.020 | 779.840 | 900.593 | -1.180 | -120.753 |
| A/images-2000 | idle_cpu_percent | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |
| A/images-2000 | thumbnail_ms | 169.175 | 169.579 | unavailable | 0.404 | unavailable |
| A/images-2000 | preview_ms | 123.834 | 51.719 | 57.266 | -72.115 | -5.547 |
| B/images-2000 | frame_ms | 57.159 | 58.015 | 218.218 | 0.856 | -160.203 |
| B/images-2000 | filter_ms | 9.194 | 8.240 | 41.474 | -0.953 | -33.233 |
| B/images-2000 | wheel_ms | 7.584 | 7.469 | 27.671 | -0.116 | -20.202 |
| B/images-2000 | peak_gui_rss_mib | 97.520 | 85.848 | 113.199 | -11.672 | -27.352 |
| B/images-2000 | peak_tree_rss_mib | 156.059 | 116.980 | 121.449 | -39.078 | -4.469 |
| B/images-2000 | settle_ms | 758.313 | 761.197 | 849.821 | 2.884 | -88.623 |
| B/images-2000 | idle_cpu_percent | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |
| B/images-2000 | thumbnail_ms | 168.570 | 168.800 | unavailable | 0.231 | unavailable |
| B/images-2000 | preview_ms | 65.220 | 33.126 | 42.369 | -32.095 | -9.243 |
| A/1000000 | frame_ms | 119.003 | 118.768 | 236.659 | -0.235 | -117.891 |
| A/1000000 | filter_ms | 16.261 | 16.856 | 39.347 | 0.595 | -22.491 |
| A/1000000 | wheel_ms | 7.445 | 7.521 | 27.407 | 0.076 | -19.886 |
| A/1000000 | peak_gui_rss_mib | 96.770 | 96.684 | 98.949 | -0.086 | -2.266 |
| A/1000000 | peak_tree_rss_mib | 111.738 | 111.641 | 131.184 | -0.098 | -19.543 |
| A/1000000 | settle_ms | 985.999 | 1028.996 | 962.577 | 42.997 | 66.419 |
| A/1000000 | idle_cpu_percent | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |
| B/1000000 | frame_ms | 119.745 | 115.031 | 219.601 | -4.713 | -104.569 |
| B/1000000 | filter_ms | 14.966 | 15.542 | 38.718 | 0.576 | -23.176 |
| B/1000000 | wheel_ms | 7.551 | 7.899 | 26.986 | 0.348 | -19.086 |
| B/1000000 | peak_gui_rss_mib | 96.887 | 96.844 | 101.734 | -0.043 | -4.891 |
| B/1000000 | peak_tree_rss_mib | 111.762 | 111.781 | 133.746 | 0.020 | -21.965 |
| B/1000000 | settle_ms | 1008.009 | 1047.482 | 934.083 | 39.473 | 113.398 |
| B/1000000 | idle_cpu_percent | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |

## Raw samples

Samples appear in run order; none are discarded.

| --- | --- | --- | --- | --- |
| A/1000 | frame_ms | 56.513, 57.622, 66.294, 57.829, 71.940 | 57.479, 60.918, 61.099, 59.781, 59.830 | 241.319, 258.042, 228.546, 229.842, 236.044 |
| A/1000 | filter_ms | 9.198, 7.258, 9.068, 9.289, 8.814 | 6.640, 7.109, 9.252, 7.915, 17.895 | 39.498, 38.936, 37.633, 38.269, 38.462 |
| A/1000 | wheel_ms | 7.743, 7.827, 7.656, 7.475, 7.544 | 7.472, 7.424, 7.547, 8.311, 9.101 | 27.445, 27.526, 28.169, 27.868, 28.746 |
| A/1000 | peak_gui_rss_mib | 77.961, 77.613, 77.934, 77.582, 77.750 | 77.773, 78.039, 77.590, 78.023, 77.605 | 99.207, 98.914, 99.367, 99.254, 98.977 |
| A/1000 | peak_tree_rss_mib | 92.938, 92.570, 92.883, 92.594, 92.691 | 92.750, 92.988, 92.617, 93.000, 92.660 | 105.492, 104.863, 105.672, 105.566, 105.457 |
| A/1000 | settle_ms | 658.452, 638.429, 785.223, 658.597, 660.856 | 657.160, 639.753, 761.064, 656.356, 662.053 | 1265.078, 811.869, 774.461, 1258.379, 1261.553 |
| A/1000 | idle_cpu_percent | 0.000, 0.000, 0.000, 0.000, 0.000 | 0.000, 0.000, 0.000, 0.000, 0.000 | 0.000, 0.000, 0.000, 0.994, 0.000 |
| B/1000 | frame_ms | 54.291, 60.679, 55.430, 58.941, 53.822 | 56.726, 66.818, 67.140, 61.698, 57.897 | 217.797, 215.903, 217.908, 214.254, 215.878 |
| B/1000 | filter_ms | 9.631, 9.457, 6.940, 10.533, 9.759 | 6.934, 8.731, 6.449, 6.784, 8.271 | 38.838, 40.581, 37.434, 40.262, 38.646 |
| B/1000 | wheel_ms | 7.368, 7.687, 6.908, 7.557, 8.449 | 7.630, 7.878, 7.719, 7.448, 7.808 | 28.441, 27.957, 27.195, 26.188, 27.608 |
| B/1000 | peak_gui_rss_mib | 77.297, 77.406, 77.664, 77.434, 77.582 | 77.633, 77.656, 77.523, 77.652, 77.500 | 101.723, 102.082, 102.008, 101.977, 101.422 |
| B/1000 | peak_tree_rss_mib | 92.242, 92.336, 92.652, 92.395, 92.488 | 92.641, 92.613, 92.449, 92.660, 92.430 | 107.574, 108.258, 107.770, 108.129, 107.766 |
| B/1000 | settle_ms | 758.866, 758.219, 654.572, 760.329, 674.488 | 633.325, 758.231, 654.373, 782.677, 635.941 | 750.115, 1242.110, 765.543, 750.436, 745.668 |
| B/1000 | idle_cpu_percent | 0.000, 0.000, 0.000, 0.000, 0.000 | 0.000, 0.000, 0.000, 0.000, 0.000 | 0.000, 0.000, 0.000, 0.000, 0.000 |
| A/10000 | frame_ms | 65.776, 62.706, 59.590, 63.864, 60.643 | 68.081, 69.659, 69.091, 61.412, 59.804 | 229.412, 230.324, 233.677, 254.652, 248.915 |
| A/10000 | filter_ms | 8.921, 6.984, 6.547, 8.126, 7.359 | 9.109, 6.662, 17.436, 7.041, 8.647 | 38.368, 37.602, 38.917, 40.173, 62.833 |
| A/10000 | wheel_ms | 7.754, 7.685, 7.455, 7.471, 7.884 | 7.717, 7.415, 8.002, 7.750, 7.817 | 29.714, 28.043, 27.592, 27.598, 29.969 |
| A/10000 | peak_gui_rss_mib | 78.027, 78.113, 77.914, 77.930, 77.836 | 78.211, 77.863, 77.258, 78.078, 77.762 | 99.133, 98.957, 99.121, 98.961, 98.984 |
| A/10000 | peak_tree_rss_mib | 93.059, 93.090, 92.879, 92.871, 92.852 | 93.055, 92.977, 92.203, 92.984, 92.738 | 105.500, 105.535, 105.473, 107.672, 105.723 |
| A/10000 | settle_ms | 659.957, 640.173, 761.160, 657.587, 660.035 | 786.753, 777.107, 783.195, 638.651, 763.035 | 789.501, 773.114, 791.972, 811.508, 809.250 |
| A/10000 | idle_cpu_percent | 0.000, 0.000, 0.000, 0.000, 0.000 | 0.000, 0.000, 0.000, 0.000, 0.000 | 0.000, 0.000, 0.000, 0.000, 0.000 |
| B/10000 | frame_ms | 59.918, 57.248, 56.170, 58.222, 65.294 | 59.642, 56.783, 58.476, 54.582, 64.303 | 216.514, 278.927, 222.939, 227.085, 222.652 |
| B/10000 | filter_ms | 8.016, 9.261, 8.982, 8.476, 9.088 | 8.569, 8.775, 9.229, 5.607, 12.030 | 37.602, 37.909, 40.905, 41.283, 40.213 |
| B/10000 | wheel_ms | 7.640, 8.232, 7.435, 6.937, 7.800 | 7.559, 7.470, 7.457, 8.080, 8.254 | 27.054, 27.651, 27.674, 27.268, 27.562 |
| B/10000 | peak_gui_rss_mib | 78.090, 77.848, 77.824, 77.797, 77.934 | 77.926, 77.926, 77.949, 77.883, 77.398 | 101.414, 101.652, 101.465, 101.859, 101.688 |
| B/10000 | peak_tree_rss_mib | 93.086, 92.953, 92.809, 92.734, 92.914 | 92.973, 92.859, 92.863, 92.746, 92.340 | 107.746, 107.676, 107.809, 108.695, 108.195 |
| B/10000 | settle_ms | 777.486, 758.412, 638.829, 639.087, 658.265 | 757.571, 760.251, 655.822, 635.112, 758.496 | 838.430, 892.296, 748.889, 767.398, 781.826 |
| B/10000 | idle_cpu_percent | 0.000, 0.000, 0.000, 0.000, 0.000 | 0.000, 0.000, 0.000, 0.000, 0.000 | 0.000, 0.000, 0.000, 0.000, 0.000 |
| A/100000 | frame_ms | 67.852, 62.762, 63.205, 64.865, 64.316 | 100.778, 68.454, 72.525, 66.384, 66.728 | 237.924, 232.945, 253.451, 233.658, 231.410 |
| A/100000 | filter_ms | 5.808, 5.791, 5.678, 9.501, 7.481 | 8.849, 9.671, 6.826, 9.365, 5.690 | 43.005, 43.456, 40.369, 40.117, 39.144 |
| A/100000 | wheel_ms | 7.486, 7.076, 7.906, 7.324, 7.866 | 7.655, 7.967, 7.906, 6.884, 7.427 | 29.264, 27.908, 26.957, 27.014, 27.553 |
| A/100000 | peak_gui_rss_mib | 79.734, 79.551, 79.750, 79.691, 79.668 | 79.609, 79.691, 79.617, 80.027, 79.605 | 99.445, 98.957, 98.793, 99.125, 98.625 |
| A/100000 | peak_tree_rss_mib | 94.617, 94.414, 94.727, 94.730, 94.688 | 94.555, 94.738, 94.570, 94.957, 94.570 | 108.320, 108.012, 107.742, 108.016, 107.387 |
| A/100000 | settle_ms | 659.399, 763.679, 763.098, 642.018, 637.282 | 704.597, 659.217, 766.761, 761.819, 642.218 | 814.201, 802.886, 831.950, 796.109, 811.425 |
| A/100000 | idle_cpu_percent | 0.000, 0.000, 0.000, 0.000, 0.000 | 0.000, 0.000, 0.000, 0.000, 0.000 | 0.000, 0.000, 0.000, 0.000, 0.000 |
| B/100000 | frame_ms | 61.219, 60.443, 61.977, 71.277, 61.711 | 63.685, 65.209, 64.089, 62.083, 63.201 | 226.569, 232.099, 264.977, 223.278, 229.754 |
| B/100000 | filter_ms | 10.238, 9.139, 13.442, 6.720, 7.894 | 8.374, 8.670, 10.136, 9.612, 10.061 | 40.746, 38.929, 41.521, 38.387, 41.163 |
| B/100000 | wheel_ms | 8.065, 7.561, 10.935, 8.296, 7.522 | 7.778, 7.482, 7.416, 7.738, 7.889 | 26.773, 28.408, 27.842, 27.908, 28.367 |
| B/100000 | peak_gui_rss_mib | 79.453, 79.492, 79.676, 79.520, 79.613 | 79.395, 79.629, 79.762, 79.531, 79.254 | 101.984, 102.125, 101.406, 101.922, 101.215 |
| B/100000 | peak_tree_rss_mib | 94.367, 94.449, 94.668, 94.367, 94.555 | 94.383, 94.523, 94.625, 94.477, 94.254 | 110.617, 111.129, 110.398, 110.668, 110.137 |
| B/100000 | settle_ms | 678.166, 635.791, 637.496, 659.744, 632.434 | 756.401, 760.133, 656.165, 633.815, 755.482 | 803.902, 801.963, 826.287, 783.565, 789.390 |
| B/100000 | idle_cpu_percent | 0.000, 0.000, 0.000, 0.000, 0.000 | 0.000, 0.000, 0.000, 0.000, 0.000 | 0.000, 0.000, 0.000, 0.000, 0.000 |
| A/images-2000 | frame_ms | 56.632, 57.400, 57.613, 59.232, 57.732 | 55.523, 59.393, 61.038, 60.661, 57.113 | 236.292, 242.934, 242.741, 232.523, 242.577 |
| A/images-2000 | filter_ms | 8.709, 8.989, 8.887, 8.781, 7.418 | 10.438, 7.339, 8.547, 6.940, 8.763 | 40.075, 40.717, 65.825, 39.277, 39.746 |
| A/images-2000 | wheel_ms | 7.583, 7.585, 7.659, 7.913, 7.729 | 7.672, 7.726, 7.653, 7.465, 6.952 | 28.287, 26.111, 29.610, 29.126, 27.735 |
| A/images-2000 | peak_gui_rss_mib | 102.051, 97.445, 97.379, 97.180, 97.211 | 85.828, 85.824, 86.031, 85.742, 86.062 | 109.441, 110.008, 109.520, 109.293, 109.559 |
| A/images-2000 | peak_tree_rss_mib | 157.855, 159.945, 151.711, 158.195, 160.215 | 121.812, 120.754, 127.012, 124.977, 129.352 | 119.461, 120.164, 117.824, 115.824, 120.621 |
| A/images-2000 | settle_ms | 781.890, 785.490, 759.315, 781.020, 780.059 | 760.614, 782.732, 783.290, 779.840, 762.639 | 775.553, 1274.418, 900.593, 884.794, 971.607 |
| A/images-2000 | idle_cpu_percent | 0.000, 0.000, 0.000, 0.000, 0.000 | 0.000, 0.000, 0.000, 0.000, 0.000 | 0.000, 0.000, 0.000, 0.000, 0.000 |
| A/images-2000 | thumbnail_ms | 168.843, 169.175, 168.853, 171.409, 170.456 | 190.013, 169.579, 169.322, 169.146, 170.993 | unavailable, unavailable, unavailable, unavailable, unavailable |
| A/images-2000 | preview_ms | 117.689, 123.834, 123.799, 168.068, 130.211 | 60.794, 63.634, 51.378, 51.719, 49.032 | 71.436, 65.951, 56.484, 57.266, 55.796 |
| B/images-2000 | frame_ms | 56.674, 57.954, 54.870, 59.139, 57.159 | 60.666, 57.593, 63.027, 56.510, 58.015 | 218.218, 222.688, 214.914, 214.693, 218.412 |
| B/images-2000 | filter_ms | 8.403, 9.194, 9.652, 7.522, 10.095 | 6.610, 9.100, 8.177, 8.240, 8.420 | 41.792, 41.474, 74.992, 39.724, 41.279 |
| B/images-2000 | wheel_ms | 7.009, 8.362, 7.868, 7.562, 7.584 | 7.469, 7.219, 7.591, 7.090, 7.834 | 27.671, 27.186, 32.325, 27.043, 28.479 |
| B/images-2000 | peak_gui_rss_mib | 97.262, 108.852, 97.520, 97.551, 97.430 | 85.848, 85.809, 85.754, 85.977, 85.863 | 111.039, 113.199, 115.496, 115.156, 112.559 |
| B/images-2000 | peak_tree_rss_mib | 159.680, 156.059, 154.863, 144.344, 156.105 | 122.516, 122.309, 116.980, 106.719, 100.840 | 117.406, 119.715, 121.809, 121.449, 122.020 |
| B/images-2000 | settle_ms | 758.258, 781.447, 757.173, 758.313, 759.359 | 781.001, 760.830, 781.783, 759.322, 761.197 | 849.821, 1257.563, 747.988, 1243.239, 751.464 |
| B/images-2000 | idle_cpu_percent | 0.000, 0.000, 0.000, 0.000, 0.000 | 0.000, 0.000, 0.000, 0.000, 0.000 | 0.000, 0.000, 0.000, 0.000, 0.000 |
| B/images-2000 | thumbnail_ms | 169.747, 168.134, 168.483, 168.570, 169.750 | 167.037, 169.905, 167.050, 168.800, 169.648 | unavailable, unavailable, unavailable, unavailable, unavailable |
| B/images-2000 | preview_ms | 81.743, 68.263, 61.492, 65.220, 62.202 | 33.126, 32.620, 33.882, 35.614, 24.031 | 47.851, 40.871, 83.631, 42.369, 38.839 |
| A/1000000 | frame_ms | 128.740, 118.449, 117.436, 124.028, 119.003 | 118.768, 118.710, 117.866, 120.475, 119.910 | 236.659, 242.733, 228.272, 241.073, 233.317 |
| A/1000000 | filter_ms | 12.944, 16.261, 17.518, 12.543, 17.569 | 13.084, 18.702, 21.317, 12.707, 16.856 | 39.452, 40.217, 38.861, 39.347, 38.543 |
| A/1000000 | wheel_ms | 7.995, 7.445, 7.405, 7.863, 7.193 | 7.521, 8.313, 6.833, 7.442, 8.329 | 26.665, 28.943, 26.924, 28.952, 27.407 |
| A/1000000 | peak_gui_rss_mib | 96.770, 97.020, 96.727, 96.629, 96.961 | 96.684, 96.715, 96.891, 96.684, 96.664 | 98.949, 98.902, 98.938, 98.973, 99.285 |
| A/1000000 | peak_tree_rss_mib | 111.660, 112.074, 111.738, 111.652, 111.852 | 111.641, 111.605, 111.855, 111.586, 111.699 | 131.113, 131.305, 131.184, 131.039, 131.566 |
| A/1000000 | settle_ms | 991.696, 964.115, 959.460, 987.007, 985.999 | 1030.494, 1028.996, 1045.420, 986.224, 987.067 | 944.046, 1281.222, 935.969, 962.577, 1260.723 |
| A/1000000 | idle_cpu_percent | 0.000, 0.000, 0.000, 0.000, 0.000 | 0.000, 0.000, 0.000, 0.000, 0.000 | 0.000, 0.000, 0.000, 0.000, 0.000 |
| B/1000000 | frame_ms | 120.747, 113.207, 119.745, 134.141, 113.398 | 114.565, 119.250, 112.294, 115.031, 144.003 | 213.760, 220.153, 268.790, 219.601, 218.300 |
| B/1000000 | filter_ms | 11.406, 16.652, 15.474, 14.966, 10.795 | 16.979, 15.149, 11.019, 16.507, 15.542 | 37.508, 40.339, 38.718, 40.430, 37.418 |
| B/1000000 | wheel_ms | 7.551, 7.739, 7.469, 7.031, 7.741 | 7.899, 7.400, 8.211, 8.030, 7.462 | 26.474, 27.800, 27.752, 26.986, 25.816 |
| B/1000000 | peak_gui_rss_mib | 96.855, 96.758, 96.887, 96.922, 96.898 | 96.816, 96.844, 97.078, 96.746, 97.238 | 101.645, 101.719, 101.734, 101.836, 101.910 |
| B/1000000 | peak_tree_rss_mib | 111.742, 111.570, 111.781, 111.793, 111.762 | 111.699, 111.738, 112.066, 111.781, 112.195 | 133.746, 133.371, 133.801, 133.691, 133.965 |
| B/1000000 | settle_ms | 1008.038, 1006.118, 1008.009, 1027.466, 1006.880 | 1048.114, 1027.158, 1025.051, 1047.482, 1049.176 | 975.356, 914.172, 975.881, 934.083, 926.859 |
| B/1000000 | idle_cpu_percent | 0.000, 0.000, 0.000, 0.000, 0.000 | 0.000, 0.000, 0.000, 0.000, 0.000 | 0.995, 0.996, 0.000, 0.000, 0.000 |

## Interpretation and limits

sample. In A, the first two after samples (60.794 and 63.634 ms) exceed the
win in this five-run matrix, not an every-launch guarantee. In B every after

The full tables retain all positive changes. In particular, 1M settle medians
increase by 42.997 ms in A and 39.473 ms in B, and 10k settle by 117.150 and
99.306 ms. These are not code-path regressions introduced by this patch:
Studio, CLI, archive and device executables have byte-identical SHA-256 hashes
between the two products. Only the preview executable differs. No preview
helper runs in the numeric-folder cases; in images-2000, first frame, settle,
idle CPU, scrolling, filtering and thumbnail measurements all finish before
the first preview request. Before/after raw ranges overlap, including the
small-folder approximately 640/760 ms settle modes. The observation demonstrates
batch/runtime variation despite the same frozen executable, rather than an
added operation on those paths. No sample was dropped or tolerance added.

thumbnail observations in this baseline, so a thumbnail comparison is unavailable.
Both harness exits are 1 because other project-wide comparisons remain unmet;
both collections are complete with 50 runs, no collection error and no Docker
cleanup error. Each raw session retains host load, affinity, limits, OOM events,
phase events and semantic listing checks.

## Work attribution and safeguards

The helper keeps small raster images at native resolution, bounds larger
rasters to 1280 × 1280 and lets the GUI fit the supplied pixels to the view.
A QImageReader format hint comes from the already content-detected MIME, with
Qt content autodetection enabled; misleading filename suffixes are tested.
This avoids unnecessary codec probing for built-in formats. Diagnostic loader
traces on the benchmark 256 × 256 PNG show 12 image plugins versus zero and
6,553,654 versus 262,198 framed bytes (25 times fewer pixels). Those traces
attribute work; they are not substituted latency measurements.

EXIF orientation, exact native alpha/pixels, larger raster downscaling, bounded
SVG/PDF rendering, content/error/geometry/allocation/payload limits, one separate
helper process, cancellation, deadlines and explicit-only media playback remain
covered by tests. The GUI does not decode source files. There is no eager worker,
prefetch or preview cache that could shift the cost into startup or idle time.
The native-size rule applies to raster images; vectors retain bounded render
resolution so they remain sharp when enlarged.

Identities: common Studio SHA-256
`be3b2c01961d6a578ccf191e1fd2363e513993693d39bf543bd7f521142b746c`;
before preview `c81ebed04b2cefa9ed89662d6e4bad6aeeeb06f5f981d2f29b499ab7697d304c`;
after preview `1b91259e95c579f99296ca20f993ad5aad6e2e435cf1e8580065bb8acbca845c`.
Baseline JSON SHA-256:
`96a709119350b8be1e721141c2ad428862b7d3b3e439e91c766fd199248917ff`.
Omakiten task 3351 holds the table and validation handoff; full JSONs and frozen
products are under `/tmp/omafiles-3351/official-ea36/` for the tester.

## Inherited CPU affinity (task 3355)

Base `c775706`, before from its clean archive and after from that archive plus
3355. Both were compiled from scratch, then their hashes were frozen. The
Studio hashes are `cabf3a19fd82d5eddf8b1fd8058d0a0b47e2cff0d6592b3e00fe15587bcc3cc6`
(before) and `c213cc2fb1011ac0a4fc65a29f521ef6c491c6f908c64ffe097b4e9de1e6a7a6`
(after); the other four product binaries are identical. The change is in the
shared child-process setup in Studio: an omitted CPU selection preserves the
calling thread's affinity. Explicit thumbnail CPU selection is unchanged.
The preview helper remains a separate process with the same resource limits,
protocol, cancellation and decoder APIs.

The complete official matrices used five repetitions per case in A (CPU 0,
one CPU quota, 512 MiB) and B (CPUs 0,1, two CPU quota, 1 GiB), with
`--cores 0,1 --load-threshold 4 --load-timeout 600`. Neither build nor tests ran
was interrupted by sustained external Rust compilation; its complete before
and partial after are retained, not combined with the second pair. The second
120-second preflight recorded another short external build; the per-session
load guard waited until load was below 4 before every measured session. Both
second matrices completed all 50 sessions without errors or cleanup failures.

### Image preview and process-tree memory

Official medians for `images-2000`; preview is milliseconds and RSS is MiB.

| --- | --- | ---: | ---: | ---: |
| A | preview_ms | 37.465 | 37.809 | 57.266 |
| B | preview_ms | 30.832 | 27.318 | 42.369 |
| A | peak_tree_rss_mib | 106.332 | 107.438 | 119.461 |
| B | peak_tree_rss_mib | 117.992 | 101.176 | 121.449 |

Twenty alternating before/after pairs per configuration used the same image
volume and the same frozen products (80 sessions). All observations are kept.
Ratio-of-medians is reported alongside each pair's relative change. The
interval below uses the 6th through 15th sorted changes from 20 pairs; its
nominal median coverage is 95.86% for independent pairs, not a guarantee under
arbitrary host correlation.

| Configuration | Metric | Before median | After median | Change | Median paired change | Paired interval |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| A | preview_ms | 36.852 | 36.672 | -0.49% | -0.00% | -5.51% to +2.03% |
| B | preview_ms | 34.626 | 28.795 | -16.84% | -14.66% | -18.85% to +0.05% |
| A | peak_tree_rss_mib | 107.166 | 106.885 | -0.26% | +0.07% | -1.33% to +2.54% |
| B | peak_tree_rss_mib | 106.844 | 105.830 | -0.95% | -0.03% | -5.18% to +12.29% |

The final product's A/B preview ratio is 1.2735x, versus 1.0643x before. The
median per-pair A/B ratio is 1.2699x with an interval of 1.1320x to 1.3533x.
A shows no consistent slowdown and neither configuration shows a consistent
increase in process-tree RSS. The B before/after preview interval narrowly
includes zero change; the observed median improvement is not a guarantee
that every request is faster. Affinity permits parallel decoder work and
concurrent GUI/helper scheduling; it does not make a serial decoder parallel.
These PNG results do not establish scaling for every image or PDF format.

### Other metrics and limitations

recorded in task 3355 comments 203458 and 203461. Both official comparisons
750.436 ms. This baseline loss is already present in the clean before; no
all-metric victory is claimed. Image paired controls show intervals crossing
zero for the other nonzero metrics. Small RSS and timing differences remain
visible rather than being rounded into an asserted universal improvement.

The A/1M filter median was 12.140 ms before and 16.309 ms after in the
five-run matrices. A separate control of 20 alternating pairs reversed this
difference: 16.421 ms before and 11.647 ms after, with a median paired
change of -0.75% and an interval of -29.51% to +4.48%. Both
products exhibit fast and slow samples. This does not establish a filter
speedup caused by the preview change; it supplies no consistent evidence
of the apparent five-run filter regression. Every pair and the original
matrices remain recorded in task 3355.
