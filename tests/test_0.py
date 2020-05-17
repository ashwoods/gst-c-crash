import pytest
import asyncio


@pytest.mark.asyncio
async def test_hello_world(Player):
    """Hello world test to verify docker setup"""

    p = Player.from_description("videotestsrc ! fakesink")
    await p.play()
    await asyncio.sleep(5)
    await p.stop()

def test_verify_plugins_installed(Gst):
    """Test build boilerplate and plugin found"""
    assert Gst.Registry.get().find_plugin("crash") 