# gst-c-crash

GStreamer elements that crash, fail and leak on purpose, so you can test what
your application, your crash reporter and your tools do when a pipeline goes
wrong, and tutorials on debugging GStreamer applications.

The plugin has three filter elements. Each passes buffers through untouched
until `num-buffers` buffers (default 100) have gone by, then misbehaves on
every buffer after that, the way `method` (`mode` for mangler) says:

```sh
gst-launch-1.0 videotestsrc ! faulty num-buffers=5 method=sigabrt ! fakesink
```

Build it with meson and point GStreamer at it:

```sh
git clone https://github.com/ashwoods/gst-c-crash && cd gst-c-crash
meson setup build && ninja -C build
export GST_PLUGIN_PATH=$PWD/build/gst
gst-inspect-1.0 faulty
```

The [README](https://github.com/ashwoods/gst-c-crash#readme) has the
dependencies and the Docker setup.

## Tutorials

The examples come in Python, C and Rust; pick a language in any tab and every
page follows.

- [Handling errors](errors.md) in your application: the bus, state changes,
  retries, stalls and a bounded shutdown.
- [Crashes](crashes.md): debuggers, crash reports, core dumps, supervising
  the pipeline, and crash reporters.
- [Leaks](leaks.md): the leaks tracer, checkpoints in a running application,
  and the memory tools.
- [Memory corruption](memory-corruption.md): valgrind, AddressSanitizer and
  fatal criticals.
- [Blocked pipelines](blocking.md): the watchdog element, the latency and
  rusage tracers, and a debugger.
- [Timestamps and events](timestamps-and-events.md): muxers, identity and
  gst-validate.

## faulty

--8<-- "README.md:faulty"

## leaky

--8<-- "README.md:leaky"

## mangler

--8<-- "README.md:mangler"
