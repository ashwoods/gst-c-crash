"""Play a pipeline twice and report the GStreamer objects the second run
left alive, with the leaks tracer.

    GST_TRACERS=leaks python3 leak_check.py PIPELINE

Exit status: 0 when nothing leaked, 1 when something did, 2 when it cannot
check: no leaks tracer, or a pipeline that cannot be built or started.
"""
import sys
from collections import Counter

import gi

gi.require_version("Gst", "1.0")
from gi.repository import GLib, Gst  # noqa: E402


def say(line):
    print(line, flush=True)


def play(pipeline):
    """Play the pipeline to EOS or an error. Return False if it cannot start."""
    if pipeline.set_state(Gst.State.PLAYING) == \
            Gst.StateChangeReturn.FAILURE:
        say("error: could not start the pipeline")
        pipeline.set_state(Gst.State.NULL)
        return False
    pipeline.get_bus().timed_pop_filtered(
        Gst.CLOCK_TIME_NONE, Gst.MessageType.EOS | Gst.MessageType.ERROR)
    pipeline.set_state(Gst.State.NULL)
    return True


def main():
    Gst.init(None)
    # --8<-- [start:tracer]
    # the tracer exists only when GST_TRACERS names it before Gst.init()
    leaks = next((t for t in Gst.tracing_get_active_tracers()
                  if type(t).__name__ == "GstLeaksTracer"), None)
    if leaks is None:
        say("error: no leaks tracer: run with GST_TRACERS=leaks")
        return 2
    # --8<-- [end:tracer]
    try:
        pipeline = Gst.parse_launch(sys.argv[1])
    except GLib.Error as err:
        say(f"error: could not build the pipeline: {err.message}")
        return 2

    # --8<-- [start:checkpoint]
    # the first run creates the objects GStreamer keeps for good (the
    # system clock, caps and pad templates): leave them out
    if not play(pipeline):
        return 2
    leaks.emit("activity-start-tracking")
    play(pipeline)
    checkpoint = leaks.emit("activity-get-checkpoint")
    leaks.emit("activity-stop-tracking")

    # alive = created since tracking started, minus removed since
    alive = Counter(s.get_value("type-name") for s in
                    checkpoint.get_value("objects-created-list"))
    alive.subtract(s.get_value("type-name") for s in
                   checkpoint.get_value("objects-removed-list"))
    leaked = sorted((name, n) for name, n in alive.items() if n > 0)
    for name, n in leaked:
        say(f"leaked: {n} {name}")
    # --8<-- [end:checkpoint]
    if not leaked:
        say("no leaks")
    return 1 if leaked else 0


if __name__ == "__main__":
    sys.exit(main())
