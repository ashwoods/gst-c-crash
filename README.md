# 💥 Gst-Crashy [WIP]

This is a Gst plugin for causing leaks and crashes.

You want to know how to debug problems before they surface. This repository's
goal is two-fold:

- provide a few elements to test debug and crash reporting infrastructure and tooling.
- provide a few tutorials and best practices on debugging gstreamer applications.

The plugin contains three filter elements based on `GstBaseTransform`:

- `leaky`: provides several mechanisms of leaking memory.
- `faulty`: provides a few ways of crashing a pipeline.
- `mangler`: breaks timestamps and events.

## Quickstart

### Installation

Install GStreamer with its development files, meson and ninja:

```sh
# macOS
brew install gstreamer meson ninja
# Debian/Ubuntu
sudo apt install build-essential meson ninja-build libgstreamer1.0-dev \
    gstreamer1.0-tools gstreamer1.0-plugins-base
```

Build the plugin and point GStreamer at it:

```sh
meson setup build
ninja -C build
export GST_PLUGIN_PATH=$PWD/build/gst
gst-inspect-1.0 faulty
```

Or build and test in Docker (Debian bookworm), with the repository mounted at
`/src`:

```sh
docker compose run --rm build   # meson + ninja into build/
docker compose run --rm test    # pytest
docker compose run --rm shell   # bash, with GST_PLUGIN_PATH set
```

Docker and a native build both use `build/`: delete it when you switch.

### Overview

All three elements pass buffers through untouched until `num-buffers` buffers
(default 100) have gone by, then misbehave on every buffer after that, the way
`method` (`mode` for mangler) says.

#### Leaky

| `method`           | Leaks, per buffer                                                          |
|--------------------|----------------------------------------------------------------------------|
| `malloc` (default) | a 4-byte `malloc()` that is never freed                                    |
| `ref`              | an extra reference on the buffer, so the whole frame and its memory stay alive |

A `ref` leak is a full video frame, so bound the source: with 110 buffers and
the default `num-buffers` both methods leak 10 times.

```sh
gst-launch-1.0 videotestsrc num-buffers=110 ! leaky method=ref ! fakesink
```

To see the leak:

- `ref`, with GStreamer's leaks tracer. On its own it warns `Leaks detected`;
  `GST_DEBUG=GST_TRACER:7` lists every object still alive, here 10 `GstBuffer`s:

  ```sh
  GST_TRACERS=leaks GST_DEBUG=GST_TRACER:7 gst-launch-1.0 videotestsrc num-buffers=110 \
      ! leaky method=ref ! fakesink 2>&1 | grep -c 'type-name=(string)GstBuffer,'
  ```

- `malloc`, on macOS, with `leaks`. It reports
  `9 INSTANCES OF 'ROOT LEAK: <malloc in leaky_malloc_leak>'`: the last block
  is still reachable through a global, so a malloc leak always shows one fewer.

  ```sh
  MallocStackLogging=1 leaks --atExit -- gst-launch-1.0 -q videotestsrc num-buffers=110 \
      ! leaky ! fakesink
  ```

- `malloc`, on Linux (for example in `docker compose run --rm shell`), with
  valgrind or AddressSanitizer; both report 36 bytes in 9 blocks from
  `leaky_malloc_leak (gstleaky.c:76)`. LeakSanitizer and valgrind do not run on
  macOS arm64.

  ```sh
  valgrind --leak-check=full gst-launch-1.0 -q videotestsrc num-buffers=110 ! leaky ! fakesink

  meson setup /tmp/asan -Db_sanitize=address && ninja -C /tmp/asan
  GST_PLUGIN_PATH=/tmp/asan/gst LD_PRELOAD=$(gcc -print-file-name=libasan.so) \
      gst-launch-1.0 -q videotestsrc num-buffers=110 ! leaky ! fakesink
  ```

#### Faulty

| `method`            | What it does                                                    | What gst-launch-1.0 or an app sees |
|---------------------|-----------------------------------------------------------------|------------------------------------|
| `sigsegv` (default) | `raise(SIGSEGV)` in the streaming thread                         | the process dies of SIGSEGV (exit 139) |
| `sigbus`            | `raise(SIGBUS)`                                                  | dies of SIGBUS (exit 138) |
| `sigabrt`           | `raise(SIGABRT)`                                                 | dies of SIGABRT (exit 134) |
| `custom`            | writes to a null pointer, a placeholder for your own crash       | dies of SIGSEGV (exit 139) |
| `gst`               | posts a `GST_ELEMENT_ERROR` and returns `GST_FLOW_ERROR`          | an error from faulty on the bus: `Cannot do this anymore.` |
| `flow-error`        | logs `Boom!` with `GST_ERROR_OBJECT` and returns `GST_FLOW_ERROR` without posting an error | only the source's `Internal data stream error`; `Boom!` shows with `GST_DEBUG=faulty:1`, and gst-validate reports `buffer::flow-error-without-error-message` |
| `unref`             | unrefs the buffer once too often, then pushes it on             | CRITICALs from other elements and an `Internal data stream error`, or a crash elsewhere |
| `double-free`       | frees a `malloc()` block twice                                   | dies of SIGABRT in `free()` |
| `use-after-free`    | writes to a freed `malloc()` block                               | a crash later in the allocator, or nothing until a tool catches it |
| `hang`              | logs `Hanging the streaming thread.` and blocks it forever       | data stops and the pipeline cannot shut down: Ctrl-C does nothing, the process has to be killed. For testing watchdogs |
| `stall`             | sleeps in the streaming thread for `stall-time` ms (default 2000), once, then passes buffers again | a pause: no data for `stall-time`, then the stream goes on to EOS. `Stalling the streaming thread for N ms.` shows with `GST_DEBUG=faulty:1`; the `watchdog` element reports `Watchdog triggered` |
| `spin`              | busy-loops in the streaming thread for `stall-time` ms, once, then passes buffers again | the same pause, but with a CPU core at 100%: user time equals wall time. `Spinning the streaming thread for N ms.` shows with `GST_DEBUG=faulty:1` |
| `deadlock`          | the streaming thread and a helper thread, `faulty-deadlock`, take two locks in opposite orders. Logs `Deadlocking the streaming thread.` | data stops for good, with no CPU use, and so does the app's `g_object_get()` on faulty: Ctrl-C does nothing, the process has to be killed. Needs a debugger to see why |
| `start`             | `start()` fails, whatever `num-buffers` is                       | `Failed to set pipeline to PAUSED.` (exit 255) |
| `stop`              | the READY→NULL state change fails, every time                    | the stream runs to EOS, then `Failed to stop.` (exit 1); an app's `set_state(NULL)` returns `FAILURE` |

```sh
gst-launch-1.0 videotestsrc ! faulty num-buffers=5 method=sigabrt ! fakesink
```

The memory corruption methods rarely crash where the bug is:
[docs/memory-corruption.md](docs/memory-corruption.md) shows how valgrind,
AddressSanitizer and `G_DEBUG=fatal-criticals` find them.

The blocking methods (`stall`, `spin`, `deadlock`) never crash:
[docs/blocking.md](docs/blocking.md) shows how the watchdog element, the
latency and rusage tracers, `top` and a debugger find each. `stall-time` sets
how long `stall` and `spin` block, in milliseconds.

On Linux gst-launch-1.0 catches SIGSEGV and waits for a debugger
(`Spinning. Please run 'gdb gst-launch-1.0 <pid>'`); pass `--no-fault` to let
the signal kill it.

#### Mangler

mangler breaks timestamps and events instead of crashing, so a muxer, a sink,
an app or gst-validate has something to notice. It logs what it does once, at
ERROR level (`GST_DEBUG=mangler:1`).

| `mode`                | What it does                                                  | What an app, muxer or tool sees |
|-----------------------|---------------------------------------------------------------|---------------------------------|
| `no-pts` (default)    | clears PTS and DTS                                            | qtmux fails with `Buffer has no PTS.` (exit 1); matroskamux only warns `Invalid buffer timestamp` |
| `backwards`           | starts the timestamps over from the first buffer's            | qtmux logs `decreasing DTS value` errors but exits 0; identity's `check-imperfect-timestamp` posts one message, `delta=(gint64)-166666666` |
| `gap`                 | moves the timestamps 1 s ahead, without a DISCONT flag        | identity posts `delta=(gint64)1000000000`; a sink with `sync=true` waits a second: 20 buffers take 1.8 s instead of 0.8 s |
| `bad-duration`        | doubles every duration                                        | each buffer overlaps the next: identity posts `delta=(gint64)-33333333` for each; muxers say nothing |
| `before-segment`      | pushes a new segment that starts 1 s after the buffer         | the sink drops the next second of buffers silently; `GST_DEBUG=basesink:5` logs `dropping buffer, out of clipping segment` |
| `drop-eos`            | swallows EOS                                                  | the pipeline never ends (kill it); an mp4 has no `moov` atom, so it does not play |
| `drop-segment`        | swallows segment events                                       | `Got data flow before segment event` warnings; gst-validate reports `buffer::before-segment` and `event::eos-without-segment` |
| `drop-stream-start`   | swallows stream-start events                                  | `Got data flow before stream-start event` warnings, and no stream-start message on the bus |
| `sticky-storm`        | sends the caps again before every buffer                      | gst-validate reports `event::caps-duplicate`; `fakesink silent=false` lists 16 caps events |

```sh
gst-launch-1.0 videotestsrc num-buffers=20 ! jpegenc ! mangler num-buffers=5 mode=no-pts ! qtmux ! fakesink
```

[docs/timestamps-and-events.md](docs/timestamps-and-events.md) shows the
command and output that catches each mode.

The stream's first segment and stream-start come before any buffer, so
`drop-segment` and `drop-stream-start` do something only with `num-buffers=0`.
`backwards` shifts by the distance from the first buffer, so it needs
`num-buffers=2` or more: at 0 nothing moves, at 1 the second buffer repeats
the first one's timestamp.

## Contributing

The [Justfile](Justfile) wraps the common tasks: run `just` to list them.
`just setup` creates the virtualenv and `build/`, `just test` builds and runs
the tests (extra arguments go to pytest: `just test -k stall`), `just indent`
checks the style and `just docker-test` runs the tests in Docker.

Run the tests in a virtualenv that sees the system's PyGObject and GStreamer
bindings (Homebrew `pygobject3`, Debian `python3-gst-1.0`). Install `mixtape`
on its own: its `[test]` extra pins a pytest that breaks on current Python.

```sh
python3 -m venv --system-site-packages .venv
.venv/bin/pip install mixtape==0.5.1 pytest pytest-asyncio
GST_PLUGIN_PATH=$PWD/build/gst .venv/bin/pytest tests
```

Each faulty method runs in a `gst-launch-1.0 --no-fault` subprocess, and the
test checks its exit signal or error message: a crashing pipeline inside
pytest would kill the test run. Each leaky method runs in one too, and the test
counts its leaked buffers with the leaks tracer (`ref`) or its lost blocks with
macOS `leaks` (`malloc`, skipped elsewhere). The memory corruption tests run
under valgrind where it is installed. The blocking tests check that `stall`
triggers a watchdog and recovers, that `spin` burns CPU time and that
`deadlock` blocks the pipeline and a property read. Each mangler mode runs in
one too, and the test checks what an element or muxer downstream sees; the
gst-validate tests skip where `gst-validate-1.0` is missing, as in Docker
(Debian has no package for it). GitHub Actions runs
`docker compose run --rm test`, valgrind included, on every push.

The code follows GStreamer's style. `hooks/pre-commit.hook` checks it with GNU
indent (`brew install gnu-indent`, Debian `indent`) and refuses to commit
without it. GStreamer's style comes from GNU indent 2.2.12 (Debian's); 2.2.13,
Homebrew's, writes `GstBuffer *buf` and flags every `GstBuffer * buf`. Install
it with:

```sh
install -m 755 hooks/pre-commit.hook "$(git rev-parse --git-path hooks)/pre-commit"
```

Record changes in [Changelog.md](Changelog.md).
