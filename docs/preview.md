# Quick preview

Select a file and press **i** to preview it inside the window. **i** or **Escape**
closes the preview and returns to the list. **j / Right** and **k / Left** move
to the next and previous file row in the current sorted, filtered listing,
without wrapping. Marks are unchanged. The row menu and command palette also
offer **Quick preview**. Press and hold a file row with touch or pen to open
its menu, then tap **Quick preview**. This works in list, details and icon
layouts, including tag and saved-view results. Tapping the footer **menu**
action opens the same menu for the current row.

**Tab / Shift+Tab** move through the visible controls. **Enter / Space** activate
the focused button. Previous, Next and Close buttons work with pointer, touch
and pen. The panel stays inside the client area; wheel, touch dragging,
**Up / Down**, **Page Up / Down** and **Home / End** scroll overflowing contents. Scrolling
does not change files. Text is plain, selectable, read-only text.
File navigation and panel scrolling use unmodified keys. With the text focused,
**Shift+Arrow**, **Shift+Home / End**, **Ctrl+Shift+Left / Right** and
**Ctrl+Shift+Home / End** use native text selection; **Ctrl+Home / End** move
the text caret. A modified gesture that Qt's text control does not support,
such as **Ctrl+Shift+Up / Down** or modified page keys, does nothing rather
than scrolling an ancestor panel. Text focus and the previewed file stay put.
**Ctrl+A / Ctrl+C** select and copy text; plain **Right / Left** and **j / k**
retain their next/previous-file actions.

The key follows [ranger's `i` / `display_file` binding](https://github.com/ranger/ranger/blob/master/ranger/config/rc.conf),
which coexists with Space for marking. [Finder](https://support.apple.com/guide/mac-help/preview-a-file-mh14119/mac)
uses Space for Quick Look.
[Dolphin uses F11 for its Information panel](https://docs.kde.org/stable_kf6/en/dolphin/dolphin/command-reference.html),
which includes a preview; its F3 splits the view. Choosing **i** preserves the
existing marking key and follows a file manager with the same keyboard model.

Images show a static frame, including animated formats. Text reads at most
64 KiB of UTF-8, with at most 1,000 lines and 4,096 characters per line; a
truncation notice identifies an incomplete preview. PDFs show only their first
page on a white page background. Archives list up to 500 entry names without extracting anything; a notice
identifies a capped listing. Unsupported, unreadable or encrypted files, and
files that cannot be decoded, show a reason instead of opening another
application. Decoder and file-access errors report the actual failure; a
size-limit message means the corresponding size was read and exceeded that
limit. An image with an unreadable header is reported as unreadable, not
oversized. If the decoder supplies no specific cause, failed image decoding
reports incomplete or unreadable image data.

An image that decodes partially, such as a JPEG truncated after a readable
header, shows what the decoder recovered, as other image viewers do. A partial
decode may not produce a warning; a displayed image does not establish that
the source file is complete.

A loaded preview is a snapshot. External edits or deletion do not replace
already loaded contents. Close and reopen to read the current file; next and
previous still follow the surviving rows in the listing.

Audio and video start stopped. **P** or **Play** starts playback with sound;
**P** or **Pause** pauses it. **[ / ]** and the seek buttons move five seconds.
Moving to another file or closing kills the current preview process. Opening
another overlay pauses playback; closing that overlay does not resume it.
Media uses Qt Multimedia and the installed codecs. Network playlists are not
played. A failed preview can be retried by closing and reopening it; no failure
marker or thumbnail-cache entry is written.

The `omafiles-preview` helper is installed alongside the front ends by
`mise run install`. File probing, parsing, rendering and media decoding happen
in that separate process, started only when requested. There is one active
preview process, no prefetch and no preview cache. Replacing a request cancels
the old one before starting another. The GUI receives bounded plain text and
raw image pixels, and does not decode the source file.

The helper inherits the launching thread's CPU affinity and cgroup membership.
It can use the allowed CPUs when a decoder has parallel work; serial image and
PDF operations are not made parallel by affinity alone. The kernel's cpuset and
aggregate CPU quota continue to constrain it. Helpers with an explicitly
selected CPU, such as thumbnail workers, use one allowed CPU. This follows
Linux's [fork/exec affinity inheritance](https://man7.org/linux/man-pages/man2/sched_setaffinity.2.html)
and [cgroup CPU bandwidth control](https://docs.kernel.org/admin-guide/cgroup-v2.html).
The preview helper's aggregate resident memory is sampled every 25 ms and stopped above 512 MiB; this permits transient
overshoot between samples. Initial reads and media loading time out after five
seconds. Playback itself has no five-second duration limit. Text/output caps
and backpressure bound the transfer to the GUI. Images are limited to 64 MiB
source files, 64 megapixels and a 32 MiB decoder allocation budget. PDFs accept
up to 16 MiB; archives up to 64 MiB. Raster images are downscaled only when
needed to fit 1280 × 1280; smaller sources keep their native pixels through
decoding and IPC, with display fitting performed by the GUI. EXIF orientation
is still applied. Decoder selection uses a format hint from the MIME detected
from the file contents, retaining Qt's format autodetection; the filename
extension does not determine the decoder. SVG and PDF vector content remains
rendered at the bounded preview resolution, even when its logical size is small.
Video is limited to 4096 × 2160 input frames and ten displayed frames per second.
These limits keep preview work independent of the size of the folder.
