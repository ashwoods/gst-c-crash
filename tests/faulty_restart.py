"""Play one faulty pipeline twice and print how many buffers each run passed.

Run by test_faulty.py in its own process, never imported by pytest.
"""
import sys

import gi

gi.require_version("Gst", "1.0")
from gi.repository import Gst  # noqa: E402


def run(pipeline, counted):
    counted.clear()
    pipeline.set_state(Gst.State.PLAYING)
    msg = pipeline.get_bus().timed_pop_filtered(
        5 * Gst.SECOND, Gst.MessageType.ERROR | Gst.MessageType.EOS)
    pipeline.set_state(Gst.State.NULL)
    kind = msg.type.first_value_nick if msg else "timeout"
    return f"{kind}:{len(counted)}"


def main(method, num_buffers):
    Gst.init(None)
    pipeline = Gst.parse_launch(
        "videotestsrc num-buffers=50 ! "
        f"faulty method={method} num-buffers={num_buffers} ! "
        "fakesink name=sink signal-handoffs=true")
    counted = []
    pipeline.get_by_name("sink").connect(
        "handoff", lambda *args: counted.append(1))
    print(run(pipeline, counted), run(pipeline, counted))


if __name__ == "__main__":
    main(*sys.argv[1:])
