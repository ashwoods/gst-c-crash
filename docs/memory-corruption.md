# Catching memory corruption

Memory corruption is the hardest crash to debug: the process dies, if it dies
at all, somewhere other than where the bug is, and the crash reporter's stack
trace points at innocent code. faulty has three methods that corrupt memory
on purpose once `num-buffers` buffers have passed:

| `method`         | The bug                                                        |
|------------------|----------------------------------------------------------------|
| `unref`          | one `gst_buffer_unref()` too many, then pushes the buffer on   |
| `double-free`    | frees a `malloc()` block twice                                 |
| `use-after-free` | writes to a `malloc()` block after freeing it                  |

## Without a tool

What you see depends on the allocator, not on the bug:

| `method`         | macOS                                         | Linux (glibc)                                     |
|------------------|-----------------------------------------------|---------------------------------------------------|
| `double-free`    | `pointer being freed was not allocated`, SIGABRT | `free(): double free detected in tcache 2`, SIGABRT |
| `use-after-free` | SIGTRAP inside `libsystem_malloc`, at a later `malloc()` | nothing at first, SIGABRT when the streaming thread ends (`unaligned tcache chunk detected`) |
| `unref`          | `GST_IS_BUFFER` CRITICALs from videotestsrc, then `Internal data stream error`; sometimes SIGTRAP | a refcount CRITICAL, then `GST_IS_BUFFER` ones, then `Internal data stream error` |

Only `double-free` stops where the bug is. The `use-after-free` crash happens
in the allocator, and the `unref` one in whichever element touches the freed
buffer next: here the source, which complains about its own frame (Linux;
macOS shows only the second line):

```
GStreamer-CRITICAL **: gst_mini_object_unref: assertion 'GST_MINI_OBJECT_REFCOUNT_VALUE (mini_object) > 0' failed
GStreamer-Video-CRITICAL **: gst_video_frame_map_id: assertion 'GST_IS_BUFFER (buffer)' failed
```

A refcount bug often shows up only as CRITICAL warnings like these. Make them
crash where they happen, so a debugger or crash reporter gets the stack:

```sh
G_DEBUG=fatal-criticals gst-launch-1.0 videotestsrc num-buffers=50 ! faulty num-buffers=5 method=unref ! fakesink
```

## valgrind (Linux)

valgrind needs no rebuild and sees every access, in GStreamer's code too.
Run it in `docker compose run --rm shell`:

```sh
valgrind gst-launch-1.0 -q videotestsrc num-buffers=20 \
    ! faulty num-buffers=5 method=use-after-free ! fakesink
```

```
Invalid write of size 4
   at 0x5C12BFC: faulty_use_after_free (gstfaulty.c:143)
   by 0x5C12D73: faulty_call_method (gstfaulty.c:200)
   by 0x5C135E3: gst_faulty_transform_ip (gstfaulty.c:341)
 Address 0x… is 0 bytes inside a block of size 4 free'd
   at 0x4887B40: free
   by 0x5C12BEF: faulty_use_after_free (gstfaulty.c:142)
```

`double-free` gives `Invalid free()` with both frees' stacks. For `unref`, the
first error is an `Invalid read` in basesink, which frees the buffer faulty
gave away; search the report for your own code (`grep -n faulty_unref`) to
find the unref that should not be there.

## AddressSanitizer (Linux)

ASan is much faster than valgrind, so it suits longer pipelines and CI, but it
only checks the code it compiled. Build the plugin with it and preload the
runtime, since gst-launch-1.0 itself is not instrumented:

```sh
meson setup /tmp/asan -Db_sanitize=address && ninja -C /tmp/asan
GST_PLUGIN_PATH=/tmp/asan/gst LD_PRELOAD=$(gcc -print-file-name=libasan.so) \
    ASAN_OPTIONS=detect_leaks=0 gst-launch-1.0 -q videotestsrc num-buffers=20 \
    ! faulty num-buffers=5 method=use-after-free ! fakesink
```

```
ERROR: AddressSanitizer: heap-use-after-free on address 0x…
    #0 in faulty_use_after_free ../../src/gst/gstfaulty.c:143
freed by thread T1 (videotestsrc0:s) here:
    #1 in faulty_use_after_free ../../src/gst/gstfaulty.c:142
previously allocated by thread T1 (videotestsrc0:s) here:
```

`double-free` gives `attempting double-free`. `unref` gives nothing: the bad
accesses are in libgstreamer, which ASan did not compile. Use valgrind or
fatal-criticals for refcount bugs, or build GStreamer itself with ASan.

The plugin builds with `buildtype=debug` (`-O0`). In an optimised build the
compiler turns the last call of `faulty_unref` and `faulty_double_free` into a
tail call, so their own frames can vanish from these stacks.

## In the tests

`tests/test_faulty.py` checks `double-free` aborts everywhere, the macOS
behaviour above on macOS, and that valgrind names each method's function
where valgrind is installed, as it is in the Docker image.
`tests/test_doc_samples.py` runs every command on this page and checks the
output shown, valgrind and AddressSanitizer included on Linux.
