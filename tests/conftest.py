import pytest
from mixtape.players import AsyncPlayer


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