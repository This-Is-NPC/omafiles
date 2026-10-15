# File thumbnails

List, details and icon views show previews for readable images and formats
supported by installed desktop thumbnailers, including video and PDF. The file
icon remains available while a preview is pending or unavailable. Opening,
selecting, marking and dragging a preview uses the same mouse, touch, pen and
keyboard actions as the file icon; no extra control or focus stop is added.

Previews use the freedesktop shared cache under
`$XDG_CACHE_HOME/thumbnails`, or `$HOME/.cache/thumbnails` when the XDG path is
unset or relative. List and details previews use `normal` (up to 128 pixels);
icon previews use `large` (up to 256 pixels). Aspect ratio, transparency and
image orientation are preserved. Files inside the thumbnail cache are not
thumbnail sources.

A cache filename is the MD5 of the encoded absolute file URI followed by
`.png`. Omafiles checks the PNG's `Thumb::URI`, `Thumb::MTime` and, when
present, `Thumb::Size` against the readable source before using it. Stale or
malformed entries are regenerated. New entries include these fields and
`Software`, use private directory/file permissions (0700/0600), and are
published by an atomic rename. Cache directory and entry symlinks are refused;
a cache that cannot be safely written does not prevent displaying a preview.

Qt reads image formats it supports. Other formats use matching
`[Thumbnailer Entry]` files in `$XDG_DATA_HOME/thumbnailers` and
`$XDG_DATA_DIRS/thumbnailers`, including `/usr/share/thumbnailers` by default.
A user descriptor overrides the same filename in a system directory.
`TryExec` and the `%i`, `%u`, `%o`, `%s` and `%%` fields are supported.
Commands run directly, without a shell, with private temporary output, a
five-second wall-clock/CPU budget and an 8 MiB output limit. Each helper uses one
available CPU, so decoders that choose their thread count from CPU affinity do
not fan out across the desktop. A background watchdog samples the helper and
its descendants every 25 ms and cancels the process group if their combined
resident memory exceeds its share of a 512 MiB helper budget. The budget is
divided by the number of thumbnail workers (512 MiB with one, 256 MiB with two).
Virtual address space is not capped: thread
stacks and mapped libraries can reserve much more space than they use.
Generated previews are normalized and receive the shared-cache metadata.

Confirmed invalid image headers, including malformed output from a successful
helper, are remembered as a tiny PNG under
`fail/omafiles-<version>/`, with the source URI, mtime, size and an `invalid-data` error classification.
Unclassified failure records cannot establish that the source is invalid. A changed source
is retried. Cancellation, unreadable sources, unavailable optional helpers and helper
execution failures do not create failure records. A nonzero exit, signal,
timeout or resource watchdog cannot prove that the source is invalid; retrying
must remain possible without changing the file. Decoder errors that cannot
distinguish damaged image data from memory exhaustion also remain retriable;
this can include a corrupt compressed payload with a valid header.

The first frame shows file icons immediately. Thumbnail requests start after
that frame and after the viewport has been stable briefly. Only rows that
intersect the actual viewport request previews; cached and pooled offscreen
delegates do not. The reuse pool is cleared while a directory loads, so its
rows receive the current source paths. Visible previews watch their source, including in-place saves and replacement
by rename. A source change clears the displayed preview, cancels obsolete work
and generates a current preview without leaving the folder. The visible row's
metadata and details update with it. Repairing a visible invalid image retries
it; even same-size, same-timestamp edits invalidate that live preview. Only
visible preview sources are watched through one shared observer, keeping
large folders and view changes bounded. Its directory watches report entries
only, as described in [Browsing](browsing.md#watching-the-folder-on-screen).
Rows leaving the viewport release their images; movement cancels obsolete
work while keeping previews for rows that remain visible. A dedicated pool
and a queue of at most 128 pending visible requests keep decoding, cache I/O
and helper processes off the GUI thread. Obsolete requests are removed before
current visible work starts; active work checks cancellation before publication.
Opening quick preview cancels thumbnail requests in the underlying file panes,
keeping foreground decoding ahead of background work. Existing display images
remain available; closing preview resumes requests for visible rows.

The pool follows the shared [CPU-budget policy](browsing.md) used by directory
listing and filtering: usable affinity, ancestor quotas, a four-worker ceiling
and a conservative single-worker path. In the parallel
path, native image workers retain the process affinity so the scheduler can
balance them with foreground work. Each external thumbnailer is assigned its
slot's single allowed CPU, keeping helpers on separate cores without pinning
native decoders or the GUI thread.

At most four decodes run at once. Qt's image allocation limit is configured to
32 MiB at application startup, before thumbnail workers or cache readers start. External helpers share the 512 MiB admission budget described above.
Completed display images are reduced to the actual icon size; the shared cache
retains the standard 128/256-pixel tiers. Inputs above 64 megapixels, decoder allocations above 32 MiB
and helper output above 8 MiB are refused; an in-process decoder finishes its
current bounded read
before noticing cancellation. The source is checked again before publication,
so a file changed during generation does not receive an obsolete preview or
failure marker.
