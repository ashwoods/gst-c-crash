"""Run each faulty method in a child gst-launch-1.0 and check how it ends.

The signal methods kill the process they run in, so they must never run
inside pytest itself.
"""
import os
import signal
import subprocess

import pytest

TIMEOUT = 10


def launch(method=None, timeout=TIMEOUT):
    faulty = "faulty num-buffers=5" + (f" method={method}" if method else "")
    return subprocess.run(
        ["gst-launch-1.0", "videotestsrc", "num-buffers=50", "!",
         *faulty.split(), "!", "fakesink"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=timeout,
        # GST_PLUGIN_PATH is inherited; the debug log shows faulty's own errors
        env={**os.environ, "GST_DEBUG": "faulty:1"},
    )


@pytest.mark.parametrize(
    "method, sig",
    [
        (None, signal.SIGSEGV),
        ("sigsegv", signal.SIGSEGV),
        ("sigbus", signal.SIGBUS),
        ("sigabrt", signal.SIGABRT),
        ("custom", signal.SIGSEGV),
    ],
)
def test_signal(method, sig):
    p = launch(method)
    assert p.returncode == -sig, p.stdout


@pytest.mark.parametrize(
    "method, text",
    [
        ("start", "Failed to set pipeline to PAUSED."),
        ("gst", "ERROR: from element /GstPipeline:pipeline0/GstFaulty:faulty0:"
                " Cannot do this anymore."),
        ("gobject", "Boom!"),
        ("stop", "ERROR: from element /GstPipeline:pipeline0/GstFaulty:faulty0:"
                 " Failed to stop."),
    ],
)
def test_error(method, text):
    p = launch(method)
    assert p.returncode != 0, p.stdout
    assert text in p.stdout


def test_hang():
    with pytest.raises(subprocess.TimeoutExpired) as e:
        launch("hang", timeout=3)
    # the output read before the child was killed is bytes, even in text mode
    assert b"Hanging the streaming thread." in e.value.output
