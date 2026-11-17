#!/bin/sh
set -e
# Per-worktree setup: a Python venv for the tests and the style check, and a
# meson build folder. Each step runs only when its result is missing.
# The venv uses system site-packages for PyGObject/Gst (Homebrew gstreamer).
[ -x .venv/bin/pip ] || python3 -m venv --system-site-packages .venv
# mixtape[test] pins pytest 5 and plugins that break on current Python, so
# install mixtape alone with a current pytest.
[ -x .venv/bin/pytest ] ||
  .venv/bin/pip install -q --disable-pip-version-check 'mixtape==0.5.1' pytest pytest-asyncio
# GStreamer's own indent, with its style built in: `just indent` checks with it
[ -x .venv/bin/gst-indent-1.0 ] ||
  .venv/bin/pip install -q --disable-pip-version-check 'gst-indent==1.0.0'
# meson wraps the compiler in sccache when it is installed, and sccache puts
# its socket in TMPDIR: the lander's checks set one too long for a socket
# path ("path must be shorter than SUN_LEN"), so build without it.
if [ ! -d build ]; then
  CC=cc meson setup build >/dev/null
elif grep -q sccache build/build.ninja; then
  CC=cc meson setup --wipe build >/dev/null
fi
