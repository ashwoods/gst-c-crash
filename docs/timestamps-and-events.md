# Catching broken timestamps and events

A stream with bad timestamps or missing events rarely crashes. It plays
wrong, or a muxer writes a file that does not play, or the pipeline never
ends. mangler breaks the stream on purpose once `num-buffers` buffers have
passed, and the tool that notices depends on the break:

| `mode`              | The bug                                                  | Caught by                               |
|---------------------|----------------------------------------------------------|-----------------------------------------|
| `no-pts`            | clears PTS and DTS                                       | qtmux (error), matroskamux (warning)    |
| `backwards`         | timestamps start over from the first buffer's            | identity, qtmux (errors)                |
| `gap`               | timestamps jump 1 s ahead, without a DISCONT flag        | identity, a synced sink (1 s freeze)    |
| `bad-duration`      | doubles every duration, so each buffer overlaps the next | identity                                |
| `before-segment`    | a new segment starts 1 s ahead of the buffers            | basesink's debug log                    |
| `drop-eos`          | swallows EOS                                             | a timeout, the mp4 with no `moov`       |
| `drop-segment`      | swallows segment events                                  | GStreamer's warning, gst-validate       |
| `drop-stream-start` | swallows stream-start events                             | GStreamer's warning, the bus            |
| `sticky-storm`      | sends the caps again before every buffer                 | gst-validate, `fakesink silent=false`   |

Each logs what it does once, at ERROR level (`GST_DEBUG=mangler:1`), for
example `Clearing timestamps.`

The examples use `videotestsrc num-buffers=20`: the first 5 buffers pass, the
other 15 are broken. `drop-segment` and `drop-stream-start` are the exception.
The stream's first segment and stream-start come before any buffer, so they
are swallowed only with `num-buffers=0`. A later one needs a seek or a new
stream.

## no-pts: muxers

Muxers need timestamps to place each frame in the file. qtmux refuses a
buffer without a PTS and stops the pipeline:

```sh
gst-launch-1.0 videotestsrc num-buffers=20 ! jpegenc \
    ! mangler mode=no-pts num-buffers=5 ! qtmux ! fakesink
```

```
ERROR: from element /GstPipeline:pipeline0/GstQTMux:qtmux0: Could not multiplex stream.
Additional debug info:
…/gstqtmux.c(5936): gst_qt_mux_add_buffer (): /GstPipeline:pipeline0/GstQTMux:qtmux0:
Buffer has no PTS.
```

The exit status is 1. matroskamux is more forgiving: it drops each buffer
with a warning, 15 of them, and the run still ends with `Got EOS` and 0:

```sh
GST_DEBUG=matroskamux:2 gst-launch-1.0 videotestsrc num-buffers=20 ! jpegenc \
    ! mangler mode=no-pts num-buffers=5 ! matroskamux ! fakesink
```

```
… WARN … matroskamux matroska-mux.c:4082:gst_matroska_mux_write_data:<matroskamux0:video_0> Invalid buffer timestamp; dropping buffer
```

identity's `check-imperfect-timestamp` does not help here: it ignores buffers
without timestamps and posts nothing.

## backwards, gap, bad-duration: identity

`identity check-imperfect-timestamp=true` posts an `imperfect-timestamp`
message whenever a buffer does not start where the previous one ended. `-m`
makes gst-launch-1.0 print element messages. `delta` is the difference in
nanoseconds.

`backwards` moves the timestamps back by how far the stream has come, so
buffer 5 gets buffer 0's timestamp. identity sees it once, at the jump, and
the buffers after it are in order again:

```sh
gst-launch-1.0 -m videotestsrc num-buffers=20 ! mangler mode=backwards num-buffers=5 \
    ! identity check-imperfect-timestamp=true ! fakesink
```

```
… (element): imperfect-timestamp, delta=(gint64)-166666666, prev-timestamp=(guint64)133333333, …, cur-timestamp=(guint64)0, …
```

The shift is the distance from the first buffer, so `backwards` needs
`num-buffers=2` or more: with 0 nothing moves, with 1 the second buffer
repeats the first one's timestamp instead of going below it.

`gap` moves every timestamp 1 s ahead, without a DISCONT flag to say the jump
is on purpose. identity sees one jump of 1 s, and later buffers keep the
offset:

```
… (element): imperfect-timestamp, delta=(gint64)1000000000, prev-timestamp=(guint64)133333333, …, cur-timestamp=(guint64)1166666666, …
```

A sink that syncs to the clock waits out the gap, which is the freeze a
viewer sees. Compare 20 buffers with the gap and without it (`num-buffers=25`
never triggers it):

```sh
time gst-launch-1.0 -q videotestsrc num-buffers=20 ! mangler mode=gap num-buffers=5 ! fakesink sync=true
time gst-launch-1.0 -q videotestsrc num-buffers=20 ! mangler mode=gap num-buffers=25 ! fakesink sync=true
```

```
real	0m1.828s
real	0m0.800s
```

`bad-duration` doubles each duration, so each buffer claims the next
buffer's frame too. identity reports a delta of about one frame (33 ms)
before every buffer after the first broken one: 14 messages, 9 of
`delta=(gint64)-33333333` and 5 of `-33333334`:

```
… (element): imperfect-timestamp, delta=(gint64)-33333334, prev-timestamp=(guint64)166666666, prev-duration=(guint64)66666668, …, cur-timestamp=(guint64)200000000, …
```

`backwards` also reaches the muxers, which need DTS to increase. qtmux logs
an error for each of the 4 buffers whose DTS is below the last one's, and
still finishes with exit 0. Demuxing the mp4 again shows DTS stuck at
133 ms and samples with a duration of 0:

```sh
GST_DEBUG=qtmux:1 gst-launch-1.0 videotestsrc num-buffers=20 ! jpegenc \
    ! mangler mode=backwards num-buffers=5 ! qtmux ! fakesink
```

```
… ERROR … qtmux gstqtmux.c:5627:gst_qt_mux_add_buffer: decreasing DTS value 0:00:00.000000000 < 0:00:00.133333333
… ERROR … qtmux gstqtmux.c:5627:gst_qt_mux_add_buffer: decreasing DTS value 0:00:00.033333334 < 0:00:00.133333333
…
```

qtmux and matroskamux say nothing about `bad-duration`, and gst-validate
reports only an incidental `query::position-superior-duration` for `gap`, so
use identity.

## before-segment: the sink's debug log

At the first broken buffer mangler sends a new segment that starts 1 s after
that buffer. It keeps the same running time, so the next second of buffers
falls before the segment start. A sink drops those without a word: the run
exits 0 and plays nothing. basesink logs each drop at DEBUG level; the source
here ends within the second, so all 15 broken buffers are dropped:

```sh
GST_DEBUG=basesink:5 gst-launch-1.0 videotestsrc num-buffers=20 \
    ! mangler mode=before-segment num-buffers=5 ! fakesink 2>&1 \
    | grep -c 'dropping buffer, out of clipping segment'
```

```
15
```

Each line reads `gst_base_sink_chain_unlocked:<fakesink0> dropping buffer,
out of clipping segment`. gst-validate reports nothing: its
`buffer::is-out-of-segment` check only covers demuxers.

## drop-eos: a timeout and the file

Without EOS nothing ends: the sink never sees it and gst-launch-1.0 waits
forever, so always run it under `timeout`. The harm is
in the file. qtmux writes the `moov` atom, which the player needs to find the
frames, when it gets EOS:

```sh
timeout 3 gst-launch-1.0 videotestsrc num-buffers=20 ! jpegenc \
    ! mangler mode=drop-eos num-buffers=5 ! qtmux ! filesink location=/tmp/x.mov
echo $?; grep -c moov /tmp/x.mov; grep -c mdat /tmp/x.mov
```

```
124
0
1
```

124 is `timeout`'s exit status. The file has the video data (`mdat`) and no
`moov`, so it cannot be played: the real "mp4 has no moov atom" bug. With
`num-buffers=100` EOS passes and `grep -c moov` finds 1. `gst-launch-1.0 -e`
and Ctrl-C send their own EOS from the source, and mangler swallows that too.

## drop-segment, drop-stream-start: GStreamer's own warning

Data must follow a stream-start and a segment event. GStreamer checks it in
`gst_pad_push_data` and on the receiving pad, and prints a warning that every
tool shows, gst-launch-1.0 included. These two need `num-buffers=0`:

```sh
gst-launch-1.0 videotestsrc num-buffers=20 ! mangler mode=drop-segment num-buffers=0 ! fakesink
```

```
(gst-launch-1.0:93606): GStreamer-WARNING **: …: …gst_pad_push_data:<mangler0:src> Got data flow before segment event
(gst-launch-1.0:93606): GStreamer-WARNING **: …: …gst_pad_chain_data_unchecked:<fakesink0:sink> Got data flow before segment event
```

The run still ends with `Got EOS` and 0. gst-validate-1.0 names the break
and the pads: `buffer::before-segment` and `event::eos-without-segment`, both
on `<mangler0:src, fakesink0:sink>`, and `g-log::warning` for the two core
warnings. It prints a backtrace after each; trimmed here:

```sh
gst-validate-1.0 videotestsrc num-buffers=20 ! mangler mode=drop-segment num-buffers=0 ! fakesink
```

```
   warning : buffer was received before a segment (buffer::before-segment)
             Detected on <mangler0:src, fakesink0:sink>
   warning : EOS received without segment event before (event::eos-without-segment)
             Detected on <mangler0:src, fakesink0:sink>
   warning : We got a g_log warning (g-log::warning)
Issues found: 4
=======> Test PASSED (Return value: 0)
```

Warnings do not fail gst-validate: its status is 0 unless an issue is
critical, so read the issues, not the status.

`drop-stream-start` gives the same warning with `Got data flow before
stream-start event`, and gst-validate reports only that, as `g-log::warning`.
An app sees something else: the pipeline posts a stream-start message on the
bus (`gst-launch-1.0 -m` prints `Got message … from element "pipeline0"
(stream-start)`). With `num-buffers=5` the message is there; with
`num-buffers=0` it is missing, and an app that waits for it waits for good:

```sh
gst-launch-1.0 -m videotestsrc num-buffers=20 ! mangler mode=drop-stream-start num-buffers=0 ! fakesink \
    2>/dev/null | grep -c '(stream-start)'
```

```
0
```

## sticky-storm: gst-validate

Caps are sticky: a pad sends them once, and a second event with the same caps
is a sign of a bug, and wasteful, since elements renegotiate. mangler sends
one before every broken buffer. A fakesink in verbose mode lists the sticky
events it sees: 16 caps, the negotiated one plus 15:

```sh
gst-launch-1.0 -v videotestsrc num-buffers=20 ! mangler mode=sticky-storm num-buffers=5 \
    ! fakesink silent=false 2>&1 | grep -c 'E (type: caps'
```

```
16
```

gst-validate names it:

```sh
gst-validate-1.0 videotestsrc num-buffers=20 ! mangler mode=sticky-storm num-buffers=5 ! fakesink
```

```
   warning : received the same caps twice (event::caps-duplicate)
             Detected on <fakesink0:sink>
Issues found: 1
=======> Test PASSED (Return value: 0)
```

## What mangler leaves out

There is no mode for a DTS after the PTS. Nothing in GStreamer reports it:
qtmux writes a negative composition offset without a word, matroskamux
ignores DTS and gst-validate stays silent, so a test could not tell the mode
from none.

For other bugs of this kind, use what GStreamer has: identity's
`check-imperfect-offset` and `drop-probability`, the `breakmydata` element
for corrupt data, and gst-validate scenarios for seeks and flushes.

## In the tests

`tests/test_mangler.py` runs each mode in a `gst-launch-1.0 --no-fault`
subprocess and checks what an element downstream sees: identity's deltas for
`backwards`, `gap` and `bad-duration`, qtmux's `Buffer has no PTS.` for
`no-pts`, 15 dropped buffers for `before-segment`, an mp4 with `mdat` and no
`moov` for `drop-eos` under a 3 s timeout, the missing stream-start message
and GStreamer's warning for the two `num-buffers=0` modes, and 16 caps events
for `sticky-storm`. Every mode must also leave a stream that ends before
`num-buffers` buffers untouched. The gst-validate tests (`drop-segment`,
`sticky-storm`) are skipped where `gst-validate-1.0` is not installed, as in
the Docker image: Debian has no package for it. `tests/test_doc_samples.py`
runs every command on this page and checks the output shown.
