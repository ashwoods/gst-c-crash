"""Run each leaky method in a child gst-launch-1.0 and count what it leaks.

A leak only shows once the process exits, so each run is its own process.
The source is bounded: a ref leak keeps a whole video frame alive.
"""
import os
import shutil
import subprocess

import pytest

TIMEOUT = 60
SOURCE = 20
NUM_BUFFERS = 5
LEAKS = SOURCE - NUM_BUFFERS


def launch(method, num_buffers=NUM_BUFFERS, wrap=(), env=None):
    return subprocess.run(
        # --no-fault: on Linux gst-launch catches SIGSEGV and spins for gdb
        [*wrap, "gst-launch-1.0", "--no-fault", "videotestsrc",
         f"num-buffers={SOURCE}", "!", "leaky", f"method={method}",
         f"num-buffers={num_buffers}", "!", "fakesink"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=TIMEOUT,
        env={**os.environ, **(env or {})},
    )


def leaked_buffers(num_buffers):
    p = launch("ref", num_buffers,
               env={"GST_TRACERS": "leaks", "GST_DEBUG": "GST_TRACER:7"})
    assert p.returncode == 0, p.stdout
    return p.stdout.count("type-name=(string)GstBuffer,")


def test_ref_leaks_a_buffer_per_buffer_after_num_buffers():
    assert leaked_buffers(NUM_BUFFERS) == LEAKS


def test_ref_leaks_nothing_before_num_buffers():
    assert leaked_buffers(SOURCE) == 0


@pytest.mark.skipif(shutil.which("leaks") is None,
                    reason="needs macOS leaks")
@pytest.mark.parametrize("num_buffers, lost", [(NUM_BUFFERS, LEAKS - 1),
                                               (SOURCE, 0)])
def test_malloc_leaks_after_num_buffers(num_buffers, lost):
    p = launch("malloc", num_buffers, wrap=("leaks", "--atExit", "--"),
               env={"MallocStackLogging": "1"})
    # the last block stays reachable through a global, so one fewer is lost
    root = "INSTANCES OF 'ROOT LEAK: <malloc in leaky_malloc_leak>'"
    if lost:
        assert f"STACK OF {lost} {root}" in p.stdout, p.stdout
    else:
        assert "leaky_malloc_leak" not in p.stdout, p.stdout
