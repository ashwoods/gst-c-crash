import pytest
import asyncio


@pytest.mark.asyncio
async def test_faulty_setup(Player):
    p = Player.from_description("videotestsrc ! faulty ! fakesink")
    await p.play()
    await asyncio.sleep(5)
    await p.stop()