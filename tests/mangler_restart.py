"""Play one mangler pipeline twice and print how many buffers kept a PTS.

Run by test_mangler.py in its own process, never imported by pytest.
"""
import sys

import gi

gi.require_version("Gst", "1.0")
from gi.repository import Gst  # noqa: E402


def run(pipeline, stamped):
    stamped.clear()
    pipeline.set_state(Gst.State.PLAYING)
    msg = pipeline.get_bus().timed_pop_filtered(
        5 * Gst.SECOND, Gst.MessageType.ERROR | Gst.MessageType.EOS)
    pipeline.set_state(Gst.State.NULL)
    kind = msg.type.first_value_nick if msg else "timeout"
    return f"{kind}:{len(stamped)}"


def main(num_buffers):
    Gst.init(None)
    pipeline = Gst.parse_launch(
        "videotestsrc num-buffers=20 ! "
        f"mangler mode=no-pts num-buffers={num_buffers} ! "
        "fakesink name=sink signal-handoffs=true")
    stamped = []

    def handoff(sink, buf, pad):
        if buf.pts != Gst.CLOCK_TIME_NONE:
            stamped.append(buf.pts)

    pipeline.get_by_name("sink").connect("handoff", handoff)
    print(run(pipeline, stamped), run(pipeline, stamped))


if __name__ == "__main__":
    main(*sys.argv[1:])
