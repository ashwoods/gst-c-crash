# Changelog
All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed
- Faulty's `gobject` method is now called `flow-error`, which says what it does: it returns `GST_FLOW_ERROR` without posting an error message. `method=gobject` no longer works.
- Faulty and leaky describe themselves correctly in gst-inspect-1.0: their own descriptions, method and `num-buffers` help texts, and method descriptions. The plugin points at github.com/ashwoods/gst-c-crash.
- Per-buffer countdown messages log at LOG level, on the element.

### Fixed
- The method enums are registered thread-safely, and leaky maps its methods explicitly instead of indexing a table by the enum value.
- `meson setup` no longer needs gstreamer-check, and only installs the pre-commit hook when there is a `.git` folder.

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

[Unreleased]: https://github.com/ashwoods/gst-c-crash/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/ashwoods/gst-c-crash/releases/tag/v0.1.0