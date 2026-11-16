"""Run each faulty method in a child gst-launch-1.0 and check how it ends.

The signal methods kill the process they run in, so they must never run
inside pytest itself.
"""
import os
import shutil
import signal
import subprocess
import sys

import pytest

TIMEOUT = 10


def launch(method=None, timeout=TIMEOUT, wrap=(), env=None):
    faulty = "faulty num-buffers=5" + (f" method={method}" if method else "")
    return subprocess.run(
        # --no-fault: on Linux gst-launch catches SIGSEGV and spins for gdb
        [*wrap, "gst-launch-1.0", "--no-fault", "videotestsrc",
         "num-buffers=50", "!", *faulty.split(), "!", "fakesink"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=timeout,
        # GST_PLUGIN_PATH is inherited; the debug log shows faulty's own errors
        env={**os.environ, "GST_DEBUG": "faulty:1", **(env or {})},
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
        ("flow-error", "Boom!"),
        ("stop", "ERROR: from element /GstPipeline:pipeline0/GstFaulty:faulty0:"
                 " Failed to stop."),
    ],
)
def test_error(method, text):
    p = launch(method)
    assert p.returncode != 0, p.stdout
    assert text in p.stdout


def test_num_buffers_restarts_with_each_play():
    # one element, played twice: the countdown starts over on the second play
    helper = os.path.join(os.path.dirname(__file__), "faulty_restart.py")
    p = subprocess.run(
        [sys.executable, helper, "gst", "5"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=TIMEOUT,
    )
    assert p.returncode == 0, p.stdout
    assert p.stdout.split()[-2:] == ["error:5", "error:5"], p.stdout


def test_double_free_aborts():
    # macOS malloc and glibc both notice a block freed twice, and abort
    p = launch("double-free")
    assert p.returncode == -signal.SIGABRT, p.stdout


@pytest.mark.skipif(sys.platform != "darwin", reason="macOS malloc")
@pytest.mark.parametrize("method, env, text", [
    # macOS's zone allocator traps on the freelist the write corrupted
    ("use-after-free", {}, ""),
    # the freed buffer is the source's next frame: GLib complains about it
    # there, far from faulty, and fatal-criticals makes that a crash
    ("unref", {"G_DEBUG": "fatal-criticals"},
     "assertion 'GST_IS_BUFFER (buffer)' failed"),
])
def test_memory_corruption_macos(method, env, text):
    p = launch(method, env=env)
    assert p.returncode == -signal.SIGTRAP, p.stdout
    assert text in p.stdout


@pytest.mark.skipif(shutil.which("valgrind") is None, reason="needs valgrind")
@pytest.mark.parametrize("method, frame", [
    ("unref", "faulty_unref (gstfaulty.c:"),
    ("double-free", "faulty_double_free (gstfaulty.c:"),
    ("use-after-free", "faulty_use_after_free (gstfaulty.c:"),
])
def test_memory_corruption_valgrind(method, frame):
    p = launch(method, timeout=120,
               wrap=("valgrind", "--error-exitcode=99", "--num-callers=30"))
    assert frame in p.stdout, p.stdout


def test_hang():
    with pytest.raises(subprocess.TimeoutExpired) as e:
        launch("hang", timeout=3)
    # the output read before the child was killed is bytes, even in text mode
    assert b"Hanging the streaming thread." in e.value.output
