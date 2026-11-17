"""Run the example applications in examples/ against faulty and leaky.

Every language's example prints the same lines and exits with the same
status, so each test runs once per language.
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
BUILD = Path(os.environ.get("GST_PLUGIN_PATH", ROOT / "build" / "gst")).parent
RUST_TARGET = Path(os.environ.get("CARGO_TARGET_DIR",
                                  ROOT / "examples" / "rust" / "target"))
SOURCE = "videotestsrc num-buffers=50 ! faulty num-buffers=5"


@pytest.fixture(scope="session")
def rust_built():
    if not shutil.which("cargo"):
        return False
    subprocess.run(
        ["cargo", "build", "--quiet", "--manifest-path",
         str(ROOT / "examples" / "rust" / "Cargo.toml")],
        check=True, timeout=600)
    return True


@pytest.fixture(params=["python", "c", "rust"])
def example(request):
    """Return a function that gives the command for one example program."""
    lang = request.param
    if lang == "rust" and not request.getfixturevalue("rust_built"):
        pytest.skip("cargo is not installed")

    def command(name):
        if lang == "python":
            return [sys.executable,
                    str(ROOT / "examples" / "python" / f"{name}.py")]
        if lang == "c":
            path = BUILD / "examples" / "c" / name.replace("_", "-")
        else:
            path = RUST_TARGET / "debug" / name.replace("_", "-")
        if not path.exists():
            pytest.skip(f"{path} is not built")
        return [str(path)]
    return command


def run(cmd, *args, env=None, timeout=30):
    return subprocess.run(
        cmd + list(args), capture_output=True, text=True, timeout=timeout,
        env={**os.environ, **(env or {})})


def test_run_pipeline_retries_an_element_error(example):
    out = run(example("run_pipeline"), "--retries", "1",
              f"{SOURCE} method=gst ! fakesink")
    error = "error: from /GstPipeline:pipeline0/GstFaulty:faulty0: " \
        "Cannot do this anymore."
    assert out.stdout.count(error) == 2, out.stdout
    assert "retrying (1/1)" in out.stdout
    assert out.returncode == 1


def test_run_pipeline_flow_error_blames_the_source(example):
    out = run(example("run_pipeline"), f"{SOURCE} method=flow-error ! fakesink")
    assert "error: from /GstPipeline:pipeline0/GstVideoTestSrc:videotestsrc0: " \
        "Internal data stream error." in out.stdout, out.stdout
    assert "reason error (-5)" in out.stdout
    assert out.returncode == 1


def test_run_pipeline_start_failure_is_not_retried(example):
    out = run(example("run_pipeline"), "--retries", "2",
              f"{SOURCE} method=start ! fakesink")
    assert out.stdout.splitlines() == ["error: could not start the pipeline"]
    assert out.returncode == 1


def test_run_pipeline_stop_failure(example):
    out = run(example("run_pipeline"), f"{SOURCE} method=stop ! fakesink")
    assert out.stdout.splitlines() == ["eos", "error: could not stop the pipeline"]
    assert out.returncode == 1


def test_run_pipeline_gives_up_on_a_hang(example):
    out = run(example("run_pipeline"), "--stall-timeout=1",
              f"{SOURCE} method=hang ! fakesink")
    assert out.stdout.splitlines() == [
        "stalled: no data for 1 s",
        "error: the pipeline did not stop within 2 s"]
    assert out.returncode == 3


def test_run_pipeline_bad_description(example):
    out = run(example("run_pipeline"), "videotestsrc ! nosuchelement ! fakesink")
    assert out.stdout.startswith("error: could not build the pipeline: ")
    assert out.returncode == 2


def test_run_pipeline_eos(example):
    out = run(example("run_pipeline"), "videotestsrc num-buffers=5 ! fakesink")
    assert out.stdout.splitlines() == ["eos"]
    assert out.returncode == 0



@pytest.mark.parametrize("method,signal", [
    ("sigsegv", "SIGSEGV"), ("sigabrt", "SIGABRT"), ("sigbus", "SIGBUS")])
def test_supervise_restarts_a_crashed_child(example, method, signal):
    out = run(example("supervise"), "--restarts", "1",
              f"{SOURCE} method={method} ! fakesink")
    lines = out.stdout.splitlines()
    assert lines == [f"crashed: {signal}", "restarting (1/1)",
                     f"crashed: {signal}"], out.stdout
    assert out.returncode == 1


def test_supervise_reports_an_error_exit(example):
    out = run(example("supervise"), f"{SOURCE} method=gst ! fakesink")
    assert out.stdout.splitlines() == ["failed: exit 1"]
    assert out.returncode == 1


def test_supervise_start_failure(example):
    # faulty's start method posts no error: a child that ignores the failed
    # set_state waits for a message forever
    out = run(example("supervise"), f"{SOURCE} method=start ! fakesink")
    assert out.stdout.splitlines() == ["failed: exit 1"]
    assert out.returncode == 1


def test_supervise_eos(example):
    out = run(example("supervise"), "videotestsrc num-buffers=5 ! fakesink")
    assert out.stdout.splitlines() == ["eos"]
    assert out.returncode == 0


def test_leak_check_finds_ref_leaks(example):
    out = run(example("leak_check"),
              "videotestsrc num-buffers=110 ! leaky method=ref ! fakesink",
              env={"GST_TRACERS": "leaks"})
    assert "leaked: 10 GstBuffer" in out.stdout.splitlines(), out.stdout
    assert out.returncode == 1


def test_leak_check_cannot_see_malloc_leaks(example):
    out = run(example("leak_check"),
              "videotestsrc num-buffers=110 ! leaky method=malloc ! fakesink",
              env={"GST_TRACERS": "leaks"})
    assert out.stdout.splitlines() == ["no leaks"]
    assert out.returncode == 0


def test_leak_check_start_failure(example):
    out = run(example("leak_check"), f"{SOURCE} method=start ! fakesink",
              env={"GST_TRACERS": "leaks"})
    assert out.stdout.splitlines() == ["error: could not start the pipeline"]
    assert out.returncode == 2


def test_leak_check_needs_the_tracer(example):
    env = {k: v for k, v in os.environ.items() if k != "GST_TRACERS"}
    out = subprocess.run(
        example("leak_check") + ["videotestsrc num-buffers=5 ! fakesink"],
        capture_output=True, text=True, timeout=30, env=env)
    assert out.stdout.splitlines() == [
        "error: no leaks tracer: run with GST_TRACERS=leaks"]
    assert out.returncode == 2
