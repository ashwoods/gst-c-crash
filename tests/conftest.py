import os

import pytest
from mixtape.players import AsyncPlayer

# Every test sets the debug output it needs. One inherited from the shell
# (GST_DEBUG=*:6, a GST_DEBUG_FILE) would change what the pipelines print,
# and can fill a disk with log.
for name in ("GST_DEBUG", "GST_DEBUG_FILE", "GST_DEBUG_DUMP_DOT_DIR",
             "GST_TRACERS", "G_DEBUG"):
    os.environ.pop(name, None)


def pytest_configure(config):
    config.addinivalue_line(
        "markers",
        "covers(*sections): the example sections (name:section, as in "
        "run_pipeline:build) whose code this test exercises; docs/ shows it "
        "under the snippet (hooks/source_lines.py)")


@pytest.fixture
def Gst():
    import gi
    from gi.repository import Gst

    Gst.init(None)
    yield Gst
    # Gst.deinit()

@pytest.fixture
def Player(Gst):
    return AsyncPlayer