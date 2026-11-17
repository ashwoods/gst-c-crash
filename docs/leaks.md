# Leaks

leaky leaks on every buffer once `num-buffers` buffers have passed, in one of
two ways that need different tools:

| `method`           | Leaks, per buffer                                       | Found by                                 |
|--------------------|---------------------------------------------------------|------------------------------------------|
| `malloc` (default) | a 4-byte `malloc()` block                               | macOS `leaks`, valgrind, AddressSanitizer |
| `ref`              | an extra reference on the buffer: the whole frame stays | GStreamer's leaks tracer                 |

The leaks tracer knows GStreamer's objects (buffers, events, caps, elements,
pads) and nothing else; the memory tools know every `malloc()` but not what a
buffer is. A `ref` leak is a full video frame (600 KB at 640×480), so always
bound the source: with 110 buffers and the default `num-buffers` both methods
leak 10 times.

## GStreamer objects: the leaks tracer

`GST_TRACERS=leaks` tracks every GStreamer object, and warns at exit about
those still alive:

```sh
GST_TRACERS=leaks gst-launch-1.0 -q videotestsrc num-buffers=110 ! leaky method=ref ! fakesink
```

```
** (gst-launch-1.0:46012): WARNING **: 10:13:26.052: Leaks detected and logged under GST_DEBUG=GST_TRACER:7
```

`GST_DEBUG=GST_TRACER:7` lists them, one `object-alive` line each, here 10
buffers:

```
object-alive, type-name=(string)GstBuffer, address=(gpointer)0x7701258a00, description=(string)buffer: 0x7701258a00, pts 0:00:03.533333333, dts 99:99:99.999999999, dur 0:00:00.033333333, size 614400, offset 106, offset_end 107, flags 0x0, ref-count=(uint)1, trace=(string);
```

It reports at `gst_deinit()`. `gst-launch-1.0` calls it; an application has
to call it too before it exits (`Gst.deinit()`, `gst_deinit()`,
`unsafe { gst::deinit() }`), or the tracer never reports.

### Who holds the reference

The tracer's `stack-traces-flags=full` adds where each object was created,
which for a leaked buffer is the source's buffer pool: true, and no help. A
leak is a reference nobody gives back, so ask for the stack of every `ref`
and `unref` too, with `check-refs=true`:

```sh
GST_TRACERS="leaks(check-refs=true,stack-traces-flags=full)" GST_DEBUG=GST_TRACER:7 \
    gst-launch-1.0 -q videotestsrc num-buffers=110 ! leaky method=ref ! fakesink
```

Each leaked buffer then comes with an `object-refings` line per reference
change. The one without a matching `unreffed` is the leak:

```
object-refings, …, type-name=(string)GstBuffer, address=(gpointer)0x79c8cdcf00, description=(string)reffed, ref-count=(uint)2, trace=(string)
0   libgstreamer-1.0.0.dylib   0x0000000102aa5cf4 gst_debug_get_stack_trace + 52
1   libgstcoretracers.dylib    0x000000010b5011e0 handle_object_reffed + 128
2   libgstreamer-1.0.0.dylib   0x0000000102aad98c gst_mini_object_ref + 156
3   libgstcrash.dylib          0x000000010b572864 gst_buffer_ref + 24
4   libgstcrash.dylib          0x000000010b572840 leaky_ref_leak + 24
5   libgstcrash.dylib          0x000000010b5727ec leaky_call_method + 92
6   libgstcrash.dylib          0x000000010b572654 gst_leaky_transform_ip + 196
```

The output is large (thousands of lines for 110 buffers): add
`filters=GstBuffer` to the tracer's parameters to track one type only. The
trace above is from macOS. On Linux the frames of GStreamer's own libraries
have names, but the plugin's often print as `??` with only an address.

### In CI

`G_DEBUG=fatal-warnings` makes the `Leaks detected` warning abort the process
(SIGTRAP, exit 133), so a test that leaks fails:

```sh
GST_TRACERS=leaks G_DEBUG=fatal-warnings gst-launch-1.0 -q videotestsrc num-buffers=110 \
    ! leaky method=ref ! fakesink
echo $?    # 133
```

It turns every other GLib warning fatal too, which is usually what a test
wants.

### In a running application

A service that runs for weeks never reaches `gst_deinit()`, and what matters
there is whether objects pile up while it runs. The tracer answers that with
checkpoints: `activity-start-tracking`, then `activity-get-checkpoint`, which
returns the objects created and the objects freed since. `leak-check` in
`examples/` finds the tracer and plays a pipeline between two checkpoints:

=== "Python"

    ```python
    --8<-- "examples/python/leak_check.py:tracer"
    ```

=== "C"

    ```c
    --8<-- "examples/c/leak-check.c:tracer"
    ```

=== "Rust"

    ```rust
    --8<-- "examples/rust/src/bin/leak-check.rs:tracer"
    ```

Then it counts, per type, what was created minus what was freed:

=== "Python"

    ```python
    --8<-- "examples/python/leak_check.py:checkpoint"
    ```

=== "C"

    ```c
    --8<-- "examples/c/leak-check.c:checkpoint"
    ```

=== "Rust"

    ```rust
    --8<-- "examples/rust/src/bin/leak-check.rs:checkpoint"
    ```

=== "Python"

    ```sh
    GST_TRACERS=leaks python3 examples/python/leak_check.py \
        "videotestsrc num-buffers=110 ! leaky method=ref ! fakesink"
    ```

=== "C"

    ```sh
    GST_TRACERS=leaks build/examples/c/leak-check \
        "videotestsrc num-buffers=110 ! leaky method=ref ! fakesink"
    ```

=== "Rust"

    ```sh
    GST_TRACERS=leaks cargo run --manifest-path examples/rust/Cargo.toml --bin leak-check -- \
        "videotestsrc num-buffers=110 ! leaky method=ref ! fakesink"
    ```

```
leaked: 10 GstBuffer
leaked: 1 GstCaps
leaked: 10 GstMemory
leaked: 1 GstVideoBufferPool
```

Each leaked buffer keeps its memory, the pool it came from and the pool's
caps alive. Two things make the count trustworthy:

- **Warm up first.** The first run of a pipeline creates objects GStreamer
  keeps for good: the system clock, a task pool, caps and pad templates.
  Tracked from the start, a pipeline that leaks nothing reports 10 of
  those. The example plays the pipeline once before tracking.
- **Look for growth, not for a number.** In a long-running application, take
  a checkpoint every few minutes: a leak grows with every one, while a cache
  or a pool fills up once and stays.

With `method=malloc` the same run says `no leaks`: the tracer cannot see
memory that is not a GStreamer object.

## Plain memory: leaks, valgrind and AddressSanitizer

A `malloc()` without its `free()` needs a memory tool.

On macOS, `leaks` checks the process as it exits. `MallocStackLogging`
records where each block was allocated:

```sh
MallocStackLogging=1 leaks --atExit -- gst-launch-1.0 -q videotestsrc num-buffers=110 \
    ! leaky ! fakesink
```

```
9 INSTANCES OF 'ROOT LEAK: <malloc in leaky_malloc_leak>'
```

It reports 9 blocks, not 10: the last block is still reachable through a
global variable, and `leaks` counts only blocks nothing points to.

On Linux (for example in `docker compose run --rm shell`) valgrind needs no
rebuild, and AddressSanitizer is much faster but needs the plugin built with
it; both report 36 bytes in 9 blocks from `leaky_malloc_leak`, and each also
lists the 16 KB block GLib allocates when it loads. LeakSanitizer and valgrind
do not run on macOS arm64.

```sh
valgrind --leak-check=full gst-launch-1.0 -q videotestsrc num-buffers=110 ! leaky ! fakesink
```

```
==6== 36 bytes in 9 blocks are definitely lost in loss record 815 of 1,676
==6==    at 0x48850C8: malloc (in /usr/libexec/valgrind/vgpreload_memcheck-arm64-linux.so)
==6==    by 0x5C12307: leaky_malloc_leak (gstleaky.c:76)
==6==    by 0x5C1238F: leaky_call_method (gstleaky.c:96)
==6==    by 0x5C127CF: gst_leaky_transform_ip (gstleaky.c:204)
…
```

AddressSanitizer needs the plugin built with it, and its runtime preloaded,
since gst-launch-1.0 itself is not instrumented:

```sh
meson setup /tmp/asan -Db_sanitize=address && ninja -C /tmp/asan
GST_PLUGIN_PATH=/tmp/asan/gst LD_PRELOAD=$(gcc -print-file-name=libasan.so) \
    gst-launch-1.0 -q videotestsrc num-buffers=110 ! leaky ! fakesink
```

```
==139==ERROR: LeakSanitizer: detected memory leaks
…
Direct leak of 36 byte(s) in 9 object(s) allocated from:
    #0 0x… in __interceptor_malloc ../../../../src/libsanitizer/asan/asan_malloc_linux.cpp:69
    #1 0x… in leaky_malloc_leak ../../src/gst/gstleaky.c:76
    #2 0x… in leaky_call_method ../../src/gst/gstleaky.c:96
    #3 0x… in gst_leaky_transform_ip ../../src/gst/gstleaky.c:204
…
```

## Growth that is not a leak

Memory can grow without a leak: a `queue` whose limits are off
(`max-size-buffers=0 max-size-bytes=0 max-size-time=0`) in front of a slow
element holds every buffer it gets, and frees them all at the end. The leaks
tracer finds nothing at exit, but checkpoints during the run show buffers
piling up. A queue's `current-level-buffers` property shows it directly.

## In the tests

`tests/test_leaky.py` counts `ref` leaks with the leaks tracer and `malloc`
leaks with macOS `leaks` (skipped elsewhere). `tests/test_examples.py` checks
that every language's `leak-check` finds the 10 buffers, and finds nothing
with `method=malloc`. `tests/test_doc_samples.py` runs every command on this
page and checks the output shown, valgrind and AddressSanitizer included on
Linux.
