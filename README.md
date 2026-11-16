# 💥 Gst-Crashy [WIP]

This is a Gst plugin for causing leaks and crashes.

You want to know how to debug problems before they surface. This repository's
goal is two-fold:

- provide a few elements to test debug and crash reporting infrastructure and tooling.
- provide a few tutorials and best practices on debugging gstreamer applications.

The plugin contains two filter elements based on `GstBaseTransform`:

- `leaky`: provides several mechanisms of leaking memory.
- `faulty`: provides a few ways of crashing a pipeline.

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

Both elements pass buffers through untouched until `num-buffers` buffers
(default 100) have gone by, then misbehave on every buffer after that, the way
`method` says.

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
  `leaky_malloc_leak (gstleaky.c:68)`. LeakSanitizer and valgrind do not run on
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
| `gobject`           | logs `Boom!` with `GST_ERROR_OBJECT` and returns `GST_FLOW_ERROR` | only the source's `Internal data stream error`; `Boom!` shows with `GST_DEBUG=faulty:1` |
| `hang`              | logs `Hanging the streaming thread.` and blocks it forever       | data stops and the pipeline cannot shut down: Ctrl-C does nothing, the process has to be killed. For testing watchdogs |
| `start`             | `start()` fails, whatever `num-buffers` is                       | `Failed to set pipeline to PAUSED.` (exit 255) |
| `stop`              | the READY→NULL state change fails, every time                    | the stream runs to EOS, then `Failed to stop.` (exit 1); an app's `set_state(NULL)` returns `FAILURE` |

```sh
gst-launch-1.0 videotestsrc ! faulty num-buffers=5 method=sigabrt ! fakesink
```

On Linux gst-launch-1.0 catches SIGSEGV and waits for a debugger
(`Spinning. Please run 'gdb gst-launch-1.0 <pid>'`); pass `--no-fault` to let
the signal kill it.

## Contributing

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
macOS `leaks` (`malloc`, skipped elsewhere).

The code follows GStreamer's style. `meson setup` installs a pre-commit hook
that checks it with GNU indent and refuses to commit without it
(`brew install gnu-indent`, Debian `indent`). Record changes in
[Changelog.md](Changelog.md).
