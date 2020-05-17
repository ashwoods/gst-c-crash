import pytest
from mixtape.players import AsyncPlayer

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