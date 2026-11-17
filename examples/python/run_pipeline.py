"""Run a pipeline the way an application should: check every state change,
watch the bus, retry runtime errors, notice a stall and stop with a deadline.

    python3 run_pipeline.py [--retries N] [--stall-timeout S] PIPELINE

Exit status: 0 at EOS, 1 on an error, 2 for a pipeline that cannot be built,
3 when the pipeline is stuck and cannot be stopped.
"""
import argparse
import os
import sys
import threading

import gi

gi.require_version("Gst", "1.0")
from gi.repository import GLib, Gst  # noqa: E402

STOP_TIMEOUT = 2  # seconds


def say(line):
    print(line, flush=True)


# --8<-- [start:stop]
def stop(pipeline):
    """Set the pipeline to NULL, giving up after STOP_TIMEOUT seconds.

    A blocked streaming thread holds its pad's stream lock, and the state
    change waits for that lock forever, so never wait for it unbounded.
    """
    result = []
    thread = threading.Thread(
        target=lambda: result.append(pipeline.set_state(Gst.State.NULL)),
        daemon=True)
    thread.start()
    thread.join(STOP_TIMEOUT)
    if not result:
        say(f"error: the pipeline did not stop within {STOP_TIMEOUT} s")
        # the stuck thread cannot be freed: leave it to the supervisor
        os._exit(3)
    if result[0] == Gst.StateChangeReturn.FAILURE:
        say("error: could not stop the pipeline")
        return False
    return True
# --8<-- [end:stop]


# --8<-- [start:bus]
def play(pipeline, stall_timeout):
    """Play until EOS, an error or a stall. Return "eos", "error" or "stall"."""
    bus = pipeline.get_bus()
    last_position, idle = None, 0
    while True:
        msg = bus.timed_pop_filtered(
            Gst.SECOND,
            Gst.MessageType.EOS | Gst.MessageType.ERROR
            | Gst.MessageType.WARNING)
        if msg is None:
            # no message for a second: is data still flowing?
            _, position = pipeline.query_position(Gst.Format.TIME)
            idle = idle + 1 if position == last_position else 0
            last_position = position
            if idle >= stall_timeout:
                say(f"stalled: no data for {stall_timeout} s")
                return "stall"
        elif msg.type == Gst.MessageType.EOS:
            say("eos")
            return "eos"
        elif msg.type == Gst.MessageType.WARNING:
            err, debug = msg.parse_warning()
            say(f"warning: from {msg.src.get_path_string()}: {err.message}")
        else:
            # the first error is the cause; the ones after it are its echo
            err, debug = msg.parse_error()
            say(f"error: from {msg.src.get_path_string()}: {err.message}")
            say(f"debug: {debug}")
            return "error"
# --8<-- [end:bus]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--retries", type=int, default=0)
    parser.add_argument("--stall-timeout", type=int, default=5)
    parser.add_argument("pipeline")
    args = parser.parse_args()

    Gst.init(None)
    # --8<-- [start:build]
    try:
        pipeline = Gst.parse_launch(args.pipeline)
    except GLib.Error as err:
        say(f"error: could not build the pipeline: {err.message}")
        return 2
    # --8<-- [end:build]

    # --8<-- [start:retry]
    for attempt in range(args.retries + 1):
        if attempt:
            say(f"retrying ({attempt}/{args.retries})")
        if pipeline.set_state(Gst.State.PLAYING) == \
                Gst.StateChangeReturn.FAILURE:
            # an element refused to start: retrying will not help. It may
            # have posted why on the bus, or not
            say("error: could not start the pipeline")
            msg = pipeline.get_bus().pop_filtered(Gst.MessageType.ERROR)
            if msg:
                err, debug = msg.parse_error()
                say(f"error: from {msg.src.get_path_string()}: "
                    f"{err.message}")
            stop(pipeline)
            return 1
        outcome = play(pipeline, args.stall_timeout)
        if not stop(pipeline):
            return 1
        if outcome == "eos":
            return 0
    return 1
    # --8<-- [end:retry]


if __name__ == "__main__":
    sys.exit(main())
