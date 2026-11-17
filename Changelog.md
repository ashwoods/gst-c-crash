# Changelog
All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Tutorials on [handling errors](docs/errors.md) in an application, [crashes](docs/crashes.md) and [leaks](docs/leaks.md), covering faulty's signal, `gst`, `flow-error`, `start`, `stop` and `hang` methods and both leaky methods.
- `examples/` has three small applications, each in Python, C and Rust: `run-pipeline` (watches the bus, checks state changes, retries errors, detects stalls, stops with a deadline), `supervise` (runs the pipeline in a child process and restarts it when it crashes) and `leak-check` (counts the objects a run leaves alive with the leaks tracer's checkpoints). `ninja` builds the C ones; `tests/test_examples.py` runs all three languages, and the Docker image now has Rust.
- A documentation site built with Material for MkDocs from `docs/`, with the examples in linked language tabs. A GitHub Actions workflow builds it on every push and publishes master to GitHub Pages; `just docs` serves it locally.
- Every example snippet in the docs says where it lives and which tests cover it, with links to GitHub. Tests declare what they cover with `@pytest.mark.covers("run_pipeline:build")`, and `mkdocs build --strict` fails when an included section has no covering test.
- `tests/test_doc_samples.py` runs every command shown in `docs/` and the README and checks it against the output the page shows. Outputs that cannot be checked, such as debugger sessions and macOS crash reports, are titled "Illustrative output", and a block that is neither tested nor marked fails the suite.

### Changed
- The style check uses GStreamer's own `gst-indent` 1.0.0 from PyPI instead of whatever GNU indent the system has, so macOS and Linux check with the same version. `just setup` installs it into `.venv`, `just indent` checks with it, the new `just fix-indent` rewrites files in place, and the pre-commit hook uses it. GitHub Actions runs `just indent` on every push.

### Fixed
- `gstcrash.c` and `gstleaky.c` follow GStreamer's style.
- The docs' outputs match what the commands print today: valgrind and AddressSanitizer line numbers, the qtmux no-PTS error, the run-pipeline retry output. The dot-file example creates its folder first.
- The docs site URL is https://shashi.to/gst-c-crash/.

## [0.2.0] - 2026-11-16

### Added
- Faulty's `unref`, `double-free` and `use-after-free` methods corrupt memory on purpose: an extra buffer unref, a block freed twice, a write to a freed block. [docs/memory-corruption.md](docs/memory-corruption.md) shows what each looks like with and without valgrind, AddressSanitizer and `G_DEBUG=fatal-criticals`.
- Faulty's `stall`, `spin` and `deadlock` methods block the streaming thread on purpose: `stall` sleeps and `spin` busy-loops for the new `stall-time` property (milliseconds, default 2000) and then both carry on, and `deadlock` locks two threads against each other for good. [docs/blocking.md](docs/blocking.md) shows how the watchdog element, the latency and rusage tracers, `top`, lldb and gdb find each. The Docker image includes gdb.
- The `mangler` element breaks timestamps and events on purpose after `num-buffers` buffers, with nine modes: `no-pts`, `backwards`, `gap`, `bad-duration`, `before-segment`, `drop-eos`, `drop-segment`, `drop-stream-start` and `sticky-storm`. [docs/timestamps-and-events.md](docs/timestamps-and-events.md) shows how qtmux, matroskamux, identity, basesink, gst-validate and a timeout catch each. The gst-validate tests skip where gst-validate-1.0 is missing, as in Docker.
- A Justfile for the common tasks: `setup`, `build`, `test`, `inspect`, `indent`, `clean`, `install-hook` and the Docker `docker-build`, `docker-test` and `docker-shell`. `just` lists them.
- A GitHub Actions workflow builds and tests the plugin in the Docker image on every push, valgrind tests included.

### Changed
- Faulty's `gobject` method is now called `flow-error`, which says what it does: it returns `GST_FLOW_ERROR` without posting an error message. `method=gobject` no longer works.
- Faulty and leaky describe themselves correctly in gst-inspect-1.0: their own descriptions, method and `num-buffers` help texts, and method descriptions. The plugin points at github.com/ashwoods/gst-c-crash.
- Per-buffer countdown messages log at LOG level, on the element.
- Faulty's property getters and setters now take the object lock, as GStreamer elements do. The `deadlock` method relies on it: its helper thread keeps the lock, so an app's property reads freeze too.

### Fixed
- `docker compose run --rm test` no longer leaves a core dump of a crashing faulty test in the repository.
- The method enums are registered thread-safely, and leaky maps its methods explicitly instead of indexing a table by the enum value.
- `meson setup` no longer needs gstreamer-check, and no longer tries to copy the pre-commit hook into `.git`, which failed silently; the README says how to install it.

## [0.1.0] - 2026-11-16
First release.

### Changed
- The README covers installation (native and Docker), every leaky and faulty method, how to see their leaks and crashes, and how to run the tests.
- The Docker image is based on Debian bookworm, since bullseye's packages no longer install, and includes valgrind. It installs mixtape with a current pytest.
- Faulty tests run gst-launch-1.0 with `--no-fault`, so a SIGSEGV kills it on Linux too instead of leaving it waiting for a debugger.
- Faulty tests run each method in a gst-launch-1.0 subprocess and check its exit signal or error, so a crash no longer kills pytest.
- Leaky tests run each method in a gst-launch-1.0 subprocess and assert how many buffers or blocks it leaks, instead of playing an unchecked pipeline for 5 seconds.

### Added
- The `leaky` element leaks memory on purpose, with the `malloc` and `ref` methods, after `num-buffers` buffers.
- The `faulty` element crashes or fails a pipeline on purpose, with the `sigsegv`, `sigbus`, `sigabrt`, `custom`, `start`, `stop`, `gst` and `gobject` methods, after `num-buffers` buffers.
- Faulty's `hang` method blocks the streaming thread for good once `num-buffers` have passed, so the pipeline stalls and cannot be shut down, for testing watchdogs and timeouts.

### Fixed
- Faulty's `num-buffers` countdown restarts each time the element starts, so a pipeline played again after an error (NULL→PLAYING) passes `num-buffers` buffers before failing again instead of failing on the first one.
- Leaky's `num-buffers` property now takes effect, and the countdown restarts each time the element starts. It used to start leaking after 100 buffers whatever was set.
- Faulty's `stop` method now fails the READY→NULL state change with a "Failed to stop." error instead of hanging gst-launch-1.0. Its failing `stop()` left the upstream elements running.
- Faulty's `custom` method now crashes with SIGSEGV: methods map to their crash functions explicitly instead of indexing a table by the enum value, and the null-pointer write is volatile so the compiler cannot delete it.

[Unreleased]: https://github.com/ashwoods/gst-c-crash/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/ashwoods/gst-c-crash/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/ashwoods/gst-c-crash/releases/tag/v0.1.0