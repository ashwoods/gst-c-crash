#!/bin/sh
set -e
# Per-worktree setup: a Python venv for the tests and a meson build folder.
# The venv uses system site-packages for PyGObject/Gst (Homebrew gstreamer).
# mixtape[test] pins pytest 5 and plugins that break on current Python, so
# install mixtape alone with a current pytest.
[ -x .venv/bin/pytest ] || {
  python3 -m venv --system-site-packages .venv
  .venv/bin/pip install -q --disable-pip-version-check 'mixtape==0.5.1' pytest pytest-asyncio
}
[ -d build ] || meson setup build >/dev/null
