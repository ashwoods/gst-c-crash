# Handling errors in your application

A pipeline fails in more ways than an ERROR message on the bus. An element
may refuse to start without saying why, blame another element, or stop
moving altogether. faulty has a method for each of these:

| `method`     | What faulty does                                       | What the application sees                                  |
|--------------|--------------------------------------------------------|------------------------------------------------------------|
| `gst`        | posts an element error and returns `GST_FLOW_ERROR`    | two errors: faulty's, then the source's                    |
| `flow-error` | returns `GST_FLOW_ERROR` without posting an error      | one error, from the source, which did nothing wrong        |
| `start`      | fails `start()`                                        | `set_state()` fails, and the bus has no error to explain it |
| `stop`       | fails the READY→NULL state change                      | EOS, then `set_state(NULL)` fails                          |
| `hang`       | blocks the streaming thread for good                   | nothing at all: no message, and `set_state(NULL)` never returns |

`examples/` has the same small application in Python, C and Rust,
`run-pipeline`, which handles all five. This page goes through it one
practice at a time; pick your language in any tab and the others follow.

=== "Python"

    ```sh
    python3 examples/python/run_pipeline.py --retries 2 --stall-timeout 5 \
        "videotestsrc num-buffers=50 ! faulty num-buffers=5 method=gst ! fakesink"
    ```

=== "C"

    ```sh
    ninja -C build    # builds build/examples/c/ too
    build/examples/c/run-pipeline --retries 2 --stall-timeout 5 \
        "videotestsrc num-buffers=50 ! faulty num-buffers=5 method=gst ! fakesink"
    ```

=== "Rust"

    ```sh
    cargo run --manifest-path examples/rust/Cargo.toml --bin run-pipeline -- \
        --retries 2 --stall-timeout 5 \
        "videotestsrc num-buffers=50 ! faulty num-buffers=5 method=gst ! fakesink"
    ```

It exits 0 at EOS, 1 on an error, 2 when the pipeline cannot be built and 3
when the pipeline is stuck for good.

## Check that the pipeline was built

A typo or a missing plugin fails before anything runs. `gst-launch-1.0`
prints `WARNING: erroneous pipeline`; an application gets an error back and
must not go on with a NULL pipeline:

=== "Python"

    ```python
    --8<-- "examples/python/run_pipeline.py:build"
    ```

=== "C"

    ```c
    --8<-- "examples/c/run-pipeline.c:build"
    ```

=== "Rust"

    ```rust
    --8<-- "examples/rust/src/bin/run-pipeline.rs:build"
    ```

```
error: could not build the pipeline: no element "nosuchelement"
```

## Watch the bus, and act on the first error

Errors, warnings and EOS arrive as messages on the pipeline's bus, posted
from the streaming threads. Nothing reads them unless you do, and a pipeline
whose errors nobody reads just stops. Real applications usually add a bus
watch to their main loop (`gst_bus_add_watch`); the examples poll with
`timed_pop_filtered`, which is shorter and also wakes up every second to
check that data still moves (next section):

=== "Python"

    ```python
    --8<-- "examples/python/run_pipeline.py:bus"
    ```

=== "C"

    ```c
    --8<-- "examples/c/run-pipeline.c:bus"
    ```

=== "Rust"

    ```rust
    --8<-- "examples/rust/src/bin/run-pipeline.rs:bus"
    ```

One failure usually posts several errors. With `method=gst` faulty posts its
own, and the source, whose push failed, posts a second one, as
`gst-launch-1.0` shows:

```sh
gst-launch-1.0 -q videotestsrc num-buffers=50 \
    ! faulty num-buffers=5 method=gst ! fakesink
```

```
ERROR: from element /GstPipeline:pipeline0/GstFaulty:faulty0: Cannot do this anymore.
Additional debug info:
../gst/gstfaulty.c(308): gst_faulty_transform_ip (): /GstPipeline:pipeline0/GstFaulty:faulty0
ERROR: from element /GstPipeline:pipeline0/GstVideoTestSrc:videotestsrc0: Internal data stream error.
Additional debug info:
streaming stopped, reason error (-5)
```

The first is the cause; the source's `Internal data stream error` is only its
echo. So stop at the first error and report that one. An application that
reports the last error, or the one from the element it knows best, blames the
source.

`method=flow-error` shows the case where the echo is all you get: faulty
returns `GST_FLOW_ERROR` without posting a message, which is a bug in the
element, and the only error is the source's:

```
error: from /GstPipeline:pipeline0/GstVideoTestSrc:videotestsrc0: Internal data stream error.
debug: ../subprojects/gstreamer/libs/gst/base/gstbasesrc.c(3187): gst_base_src_loop (): /GstPipeline:pipeline0/GstVideoTestSrc:videotestsrc0:
streaming stopped, reason error (-5)
```

When an error comes from a source and its debug string says `streaming
stopped, reason error (-5)`, the source is innocent: some element downstream
returned the error. GStreamer's log names it. `GST_DEBUG=*:1` logs only
errors:

```sh
GST_DEBUG=*:1 gst-launch-1.0 -q videotestsrc num-buffers=50 \
    ! faulty num-buffers=5 method=flow-error ! fakesink
```

```
0:00:00.093666125 49873 0x16e0d7000 ERROR faulty gstfaulty.c:311:gst_faulty_transform_ip:<faulty0> Boom!
```

Other reasons in that string point elsewhere: `not-negotiated (-4)` is a caps
problem, `not-linked (-1)` an unlinked pad. If you write elements, post the
error yourself before returning `GST_FLOW_ERROR` (`GST_ELEMENT_ERROR`, or
`GST_ELEMENT_FLOW_ERROR`); gst-validate reports
`buffer::flow-error-without-error-message` when an element does not.

## Check every state change

`set_state()` returns `FAILURE` when an element refuses the change, and the
element may or may not post an error saying why. `filesrc` with a missing
file does:

```
error: could not start the pipeline
error: from /GstPipeline:pipeline0/GstFileSrc:filesrc0: Resource not found.
```

faulty's `start` method does not: the return value is all there is.
`gst-launch-1.0` prints `Failed to set pipeline to PAUSED.` and nothing else,
and an application that ignores the return value waits for messages that
never come. So check every `set_state()`, then look for an error on the bus
(the retry loop below does both).

## Retry what can recover

A pipeline that failed can be played again: set it to NULL, then to PLAYING.
That helps when the cause is outside it, such as a network source that lost
its server, and is useless when the pipeline itself is wrong, as a failed
start usually is. So retry runtime errors a few times, and give up at once on
a start failure:

=== "Python"

    ```python
    --8<-- "examples/python/run_pipeline.py:retry"
    ```

=== "C"

    ```c
    --8<-- "examples/c/run-pipeline.c:retry"
    ```

=== "Rust"

    ```rust
    --8<-- "examples/rust/src/bin/run-pipeline.rs:retry"
    ```

faulty counts down from `num-buffers` again every time it starts, so with
`method=gst` each attempt fails the same way:

```
error: from /GstPipeline:pipeline0/GstFaulty:faulty0: Cannot do this anymore.
debug: ../gst/gstfaulty.c(308): gst_faulty_transform_ip (): /GstPipeline:pipeline0/GstFaulty:faulty0
retrying (1/2)
error: from /GstPipeline:pipeline0/GstFaulty:faulty0: Cannot do this anymore.
debug: ../gst/gstfaulty.c(308): gst_faulty_transform_ip (): /GstPipeline:pipeline0/GstFaulty:faulty0
retrying (2/2)
error: from /GstPipeline:pipeline0/GstFaulty:faulty0: Cannot do this anymore.
debug: ../gst/gstfaulty.c(308): gst_faulty_transform_ip (): /GstPipeline:pipeline0/GstFaulty:faulty0
```

In a real application, wait longer between attempts, and use the error's
domain and code (`GST_RESOURCE_ERROR`, `GST_STREAM_ERROR`, …) to decide which
errors are worth retrying.

## Notice when nothing happens

A hung pipeline posts nothing: no error, no EOS. `method=hang` blocks
faulty's streaming thread for good, and an application that only waits for
messages waits forever. Check that data still moves: the bus loop above asks
for the pipeline's position whenever a second passes without a message, and
gives up when it has not changed for `--stall-timeout` seconds. The position
comes from the sinks, which report the last buffer they got, so it stops
moving when data does, live sources included. Before the first buffer there
is no position at all, so a source that takes longer than the timeout to
start, such as a network stream, needs a longer one.

The `watchdog` element does the same from inside the pipeline: it posts an
error when no buffer passes it for `timeout` ms (see
[Blocked pipelines](blocking.md)), so the bus loop sees an ordinary error.

## Stop with a deadline

Stopping a hung pipeline hangs too. `set_state(NULL)` has to stop the
streaming thread, which needs the stream lock the blocked thread holds, so
it waits forever: Ctrl-C does nothing to `gst-launch-1.0` in this state. Run
the state change in a thread and give it a deadline:

=== "Python"

    ```python
    --8<-- "examples/python/run_pipeline.py:stop"
    ```

=== "C"

    ```c
    --8<-- "examples/c/run-pipeline.c:stop"
    ```

=== "Rust"

    ```rust
    --8<-- "examples/rust/src/bin/run-pipeline.rs:stop"
    ```

```
stalled: no data for 1 s
error: the pipeline did not stop within 2 s
```

Nothing inside the process can free that thread, so the example exits with
status 3 and leaves the restart to whatever started it: systemd's
`Restart=on-failure`, a container runtime, or a supervising process as in
[Crashes](crashes.md).

## When stopping fails

`method=stop` lets the stream run to EOS, then fails the READY→NULL change:

```
eos
error: could not stop the pipeline
```

The pipeline is left in READY, and GStreamer complains when the application
drops it: `Trying to dispose element faulty0, but it is in READY instead of
the NULL state`. Nothing much can be done but log it and not reuse the
pipeline; the error faulty posts is flushed from the bus with the rest of the
pipeline's messages, so the return value is again all there is.

## Keep GStreamer's view for the report

An error message says what failed; GStreamer's log and the pipeline's
layout say why. Both are cheap to keep:

- `GST_DEBUG=2` logs every warning and error, which in a healthy pipeline is
  little; `GST_DEBUG_FILE=/path/to/log` sends it to a file instead of
  stderr. To route it into your own logging or crash reporter, as
  breadcrumbs for example, add a log function with `gst_debug_add_log_function`.
- With `GST_DEBUG_DUMP_DOT_DIR` set, `gst_debug_bin_to_dot_file()` writes the
  pipeline's elements, pads and caps as a Graphviz file. Call it when an
  error arrives. `gst-launch-1.0` does it for you, on every state change and
  error:

  ```sh
  mkdir -p /tmp/dots    # the folder has to exist
  GST_DEBUG_DUMP_DOT_DIR=/tmp/dots gst-launch-1.0 -q videotestsrc num-buffers=50 \
      ! faulty num-buffers=5 method=gst ! fakesink
  ls /tmp/dots
  ```

  ```
  0.00.00.028592250-gst-launch.NULL_READY.dot
  0.00.00.043342292-gst-launch.READY_PAUSED.dot
  0.00.00.044847750-gst-launch.PAUSED_PLAYING.dot
  0.00.00.048942584-gst-launch.error.dot
  …
  ```

## In the tests

`tests/test_examples.py` runs each language's `run-pipeline` against every
method on this page and checks that all three print the same lines and exit
with the same status. `tests/test_doc_samples.py` runs every command on this
page and checks the output shown.
