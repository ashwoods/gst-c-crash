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
# the docs site, which tests/test_docs.py builds
.venv/bin/python -c 'import material' 2>/dev/null ||
  .venv/bin/pip install -q --disable-pip-version-check -r docs/requirements.txt
# meson wraps the compiler in sccache when it is installed, and sccache puts
# its socket in TMPDIR: the lander's checks set one too long for a socket
# path ("path must be shorter than SUN_LEN"), so build without it.
if [ ! -d build ]; then
  CC=cc meson setup build >/dev/null
elif grep -q sccache build/build.ninja; then
  CC=cc meson setup --wipe build >/dev/null
fi
