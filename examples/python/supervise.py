"""Run a pipeline in a child process, so a crash kills the child and not the
application, and restart it when it dies.

    python3 supervise.py [--restarts N] PIPELINE

Exit status: 0 when the child reaches EOS, 1 when it fails or crashes once
more than --restarts allows.
"""
import argparse
import faulthandler
import signal
import subprocess
import sys

import gi

gi.require_version("Gst", "1.0")
from gi.repository import Gst  # noqa: E402


def say(line):
    print(line, flush=True)


# --8<-- [start:child]
def child(description):
    """Play the pipeline until EOS (exit 0) or an error (exit 1)."""
    # on SIGSEGV, SIGBUS, SIGABRT, SIGFPE or SIGILL, print every Python
    # thread's stack to stderr, then die of the signal as before
    faulthandler.enable()
    Gst.init(None)
    pipeline = Gst.parse_launch(description)
    pipeline.set_state(Gst.State.PLAYING)
    msg = pipeline.get_bus().timed_pop_filtered(
        Gst.CLOCK_TIME_NONE, Gst.MessageType.EOS | Gst.MessageType.ERROR)
    pipeline.set_state(Gst.State.NULL)
    return 0 if msg.type == Gst.MessageType.EOS else 1
# --8<-- [end:child]


# --8<-- [start:supervise]
def supervise(description, restarts):
    for attempt in range(restarts + 1):
        if attempt:
            say(f"restarting ({attempt}/{restarts})")
        status = subprocess.run(
            [sys.executable, __file__, "--child", description]).returncode
        if status == 0:
            say("eos")
            return 0
        if status < 0:
            # killed by a signal: here is where a crash reporter would pick
            # up the core dump or minidump
            say(f"crashed: {signal.Signals(-status).name}")
        else:
            say(f"failed: exit {status}")
    return 1
# --8<-- [end:supervise]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--restarts", type=int, default=0)
    parser.add_argument("--child", action="store_true")
    parser.add_argument("pipeline")
    args = parser.parse_args()
    if args.child:
        return child(args.pipeline)
    return supervise(args.pipeline, args.restarts)


if __name__ == "__main__":
    sys.exit(main())
