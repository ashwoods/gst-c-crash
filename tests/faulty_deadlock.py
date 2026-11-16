"""Read a property of a deadlocked faulty from the main thread, as an app would.

Run by test_faulty.py in its own process, never imported by pytest. The read
blocks for good, so faulthandler prints every thread's Python stack after a
few seconds and exits: the main thread's shows it stuck in read_property.
"""
import faulthandler
import time

import gi

gi.require_version("Gst", "1.0")
from gi.repository import Gst  # noqa: E402


def read_property(faulty):
    return faulty.get_property("num-buffers")


def main():
    Gst.init(None)
    pipeline = Gst.parse_launch(
        "videotestsrc num-buffers=50 ! "
        "faulty name=faulty method=deadlock num-buffers=5 ! fakesink")
    faulty = pipeline.get_by_name("faulty")
    pipeline.set_state(Gst.State.PLAYING)
    # the deadlock happens within milliseconds; until then the reads return
    faulthandler.dump_traceback_later(3, exit=True)
    while True:
        read_property(faulty)
        time.sleep(0.01)


if __name__ == "__main__":
    main()
