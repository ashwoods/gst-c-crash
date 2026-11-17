"""Run the commands in the documentation and check the output it shows.

Every fenced code block in README.md and docs/*.md that is not an example
program (`--8<--`) is either a command (`sh`) or an output (no language, or
`text`), and every one needs an entry in SAMPLES below:

- `run`: a command block, run verbatim with `bash -c`. Its status and its
  expected strings are checked, and every expected string must also be in the
  output block that follows it (or the one `shows` names), so the output the
  page shows cannot drift from what the command prints.
- `output`: an output block with no command before it: `command` produces it.
- `not_run`: a command that is setup, or a debugger session, with the reason.
- `illustrative`: an output that cannot be checked (a debugger session, a
  macOS crash report). The page must say so in the block's title.

A block that no entry matches, or two do, fails `test_every_block_is_covered`,
so a sample added to a page without a test fails the suite. Where a tool or
platform is missing the sample is skipped, one by one.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
PAGES = [ROOT / "README.md", *sorted((ROOT / "docs").glob("*.md"))]
MACOS = sys.platform == "darwin"
LINUX = sys.platform.startswith("linux")
ILLUSTRATIVE = "Illustrative output"
AUTO = object()


# --- the blocks in the pages ------------------------------------------------

FENCE = re.compile(r"^( *)(`{3,})\s*(\S*)\s*(.*)$")


@dataclass(frozen=True)
class Block:
    page: str
    line: int
    lang: str
    title: str
    text: str

    @property
    def where(self):
        return f"{self.page}:{self.line}"

    @property
    def category(self):
        """`command`, `output`, or `example` (a program included from examples/)."""
        if "--8<--" in self.text:
            return "example"
        if self.lang == "sh":
            return "command"
        if self.lang in ("", "text"):
            return "output"
        raise AssertionError(f"{self.where}: a ```{self.lang} block without --8<--"
                             " is neither a command nor an output")


def extract(path):
    page = path.name
    lines = path.read_text().splitlines()
    blocks = []
    i = 0
    while i < len(lines):
        m = FENCE.match(lines[i])
        if not m:
            i += 1
            continue
        indent, ticks, lang, rest = m.groups()
        start = i + 1
        body = []
        i += 1
        while lines[i].strip() != ticks:
            # the blocks in tabs and list items are indented
            body.append(lines[i][len(indent):] if lines[i].startswith(indent)
                        else lines[i].lstrip())
            i += 1
        i += 1
        title = re.search(r'title="([^"]*)"', rest)
        blocks.append(Block(page, start, lang, title[1] if title else "",
                            "\n".join(body)))
    return blocks


BLOCKS = [b for path in PAGES for b in extract(path)]


# --- the registry -------------------------------------------------------------

@dataclass
class Sample:
    page: str
    name: str
    contains: str       # a unique substring of the block; `^` starts it with it
    kind: str           # run, output, not-run, illustrative
    why: str = ""
    command: str = ""   # output: the command that produces the block
    shows: object = AUTO  # run: contains of the output block ("auto": the next block)
    status: object = 0  # an int, a tuple of ints, or a {platform: int} dict
    timeout: int = 60
    hangs: bool = False  # the command is expected to run into the timeout
    expect: tuple = ()  # strings in the real output and in the block shown
    exact: bool = False  # the real output is the block shown
    check: object = None  # a function that asserts on the real output
    check_shown: bool = False  # and on the block shown, for numbers the page states
    only: str = ""      # "linux" or "darwin"
    needs: tuple = ()   # tools that must be installed

    @property
    def id(self):
        return f"{self.page}:{self.name}"

    @property
    def shown_category(self):
        """The category of the block `contains` finds."""
        return "command" if self.kind in ("run", "not-run") and not self.command \
            else "output"


def run(page, name, contains, **kw):
    return Sample(page, name, contains, "run", **kw)


def output(page, name, contains, command, **kw):
    return Sample(page, name, contains, "output", command=command, **kw)


def not_run(page, name, contains, why):
    return Sample(page, name, contains, "not-run", why=why)


def illustrative(page, name, contains, why):
    return Sample(page, name, contains, "illustrative", why=why)


def on(platform, *strings):
    """The strings that only this platform prints."""
    return strings if sys.platform.startswith(platform) else ()


def lines_of(out):
    return out.strip().splitlines()


def count(out, text, n):
    assert out.count(text) == n, f"{out.count(text)} x {text!r}, not {n}:\n{out}"


def times(out):
    return [int(t) for t in re.findall(r"time=\(guint64\)(\d+)", out)]


def last_line(expected):
    def check(out):
        assert lines_of(out)[-1] == expected, out
    return check


def gap_seconds(out):
    with_gap, without = (float(s) for s in re.findall(r"real\s+0m([\d.]+)s", out))
    # 20 buffers take 0.8 s, and 1.8 s with the 1 s gap
    assert with_gap >= 1.5, out
    assert without < 1.5, out


def latency_of_the_stall(out):
    # the stalled buffer took the 2 s of the stall to get from source to sink
    assert max(times(out)) >= 2_000_000_000, out


def rusage_of_the_spin(out):
    # one thread spent the spin on the CPU, the others next to nothing. The
    # spin lasts 2 s of wall time, so on a loaded machine it gets less CPU.
    spinner, *others = sorted(times(out), reverse=True)
    assert spinner >= 500_000_000, out
    assert all(t < spinner / 4 for t in others), out


def leaked_buffers(out):
    count(out, "object-alive, type-name=(string)GstBuffer, ", 10)


def bad_durations(out):
    count(out, "imperfect-timestamp, ", 14)
    count(out, "delta=(gint64)-33333333,", 9)
    count(out, "delta=(gint64)-33333334,", 5)


def python_c_stack(out):
    # faulthandler dumps the C stack of the crashed thread from Python 3.14
    if sys.version_info >= (3, 14):
        for text in ("Current thread's C stack trace (most recent call first):",
                     "faulty_signal_sigsegv+0x", "faulty_call_method+0x",
                     "gst_faulty_transform_ip+0x"):
            assert text in out, out


def supervised(out):
    count(out, "crashed: SIGSEGV", 2)
    count(out, "restarting (1/1)", 1)


PY = "python3 examples/python/run_pipeline.py"
RETRY = ('--retries 2 --stall-timeout 5 "videotestsrc num-buffers=50'
         ' ! faulty num-buffers=5 method=gst ! fakesink"')
SIGSEGV_PIPELINE = ('"videotestsrc num-buffers=50 ! faulty num-buffers=5'
                    ' method=sigsegv ! fakesink"')
LEAKY_PIPELINE = '"videotestsrc num-buffers=110 ! leaky method=ref ! fakesink"'
RUST_BUILD = 900  # the first cargo run builds the examples
SIGSEGV = signal.SIGSEGV + 128
SIGABRT = signal.SIGABRT + 128
SIGTRAP = signal.SIGTRAP + 128
IDENTITY = ("videotestsrc num-buffers=20 ! mangler mode={mode} num-buffers=5"
            " ! identity check-imperfect-timestamp=true ! fakesink")
ASAN = ("libasan",)

SAMPLES = [
    # --- README.md
    not_run("README.md", "install", "brew install gstreamer",
            "installs system packages"),
    not_run("README.md", "meson-setup", "meson setup build",
            "setup: `just setup` and the CI workflow do it for every run"),
    not_run("README.md", "docker", "docker compose run --rm build",
            "starts the containers that run these tests"),
    run("README.md", "leaky-ref", "leaky method=ref ! fakesink",
        expect=("Setting pipeline to NULL",)),
    run("README.md", "faulty-sigabrt", "method=sigabrt",
        status=SIGABRT, expect=("Setting pipeline to PLAYING",)),
    run("README.md", "mangler-no-pts", "mangler num-buffers=5 mode=no-pts ! qtmux",
        status=1, expect=("Buffer has no PTS.",)),
    not_run("README.md", "pytest", "pip install mixtape",
            "installs packages, and is how this test runs"),
    not_run("README.md", "hook", "install -m 755 hooks/pre-commit.hook",
            "installs a git hook in the checkout"),

    # --- index.md
    not_run("index.md", "build", "git clone https://github.com",
            "clones the repository and sets it up"),
    run("index.md", "faulty-sigabrt", "method=sigabrt",
        status=SIGABRT, expect=("Setting pipeline to PLAYING",)),

    # --- errors.md
    *[run("errors.md", f"retry-{lang}", contains, status=1,
          timeout=RUST_BUILD if lang == "rust" else 60,
          needs=("cargo",) if lang == "rust" else (),
          shows="retrying (1/2)",
          expect=("error: from /GstPipeline:pipeline0/GstFaulty:faulty0:"
                  " Cannot do this anymore.",
                  "debug: ../gst/gstfaulty.c(308): gst_faulty_transform_ip ()",
                  "retrying (1/2)", "retrying (2/2)"))
      for lang, contains in [("python", "python3 examples/python/run_pipeline.py"
                                        " --retries 2"),
                             ("c", "build/examples/c/run-pipeline --retries 2"),
                             ("rust", "--bin run-pipeline -- \\\n    --retries 2")]],
    output("errors.md", "build-error", "could not build the pipeline",
           f'{PY} "videotestsrc ! nosuchelement ! fakesink"',
           status=2, exact=True),
    run("errors.md", "gst-error", "^gst-launch-1.0 -q videotestsrc num-buffers=50 \\\n"
        "    ! faulty num-buffers=5 method=gst ! fakesink", status=1,
        expect=("ERROR: from element /GstPipeline:pipeline0/GstFaulty:faulty0:"
                " Cannot do this anymore.",
                "gstfaulty.c(308): gst_faulty_transform_ip ():"
                " /GstPipeline:pipeline0/GstFaulty:faulty0",
                "ERROR: from element /GstPipeline:pipeline0/GstVideoTestSrc:"
                "videotestsrc0: Internal data stream error.",
                "streaming stopped, reason error (-5)")),
    output("errors.md", "flow-error", "error: from /GstPipeline:pipeline0/"
           "GstVideoTestSrc:videotestsrc0",
           f'{PY} "videotestsrc num-buffers=50 ! faulty num-buffers=5'
           ' method=flow-error ! fakesink"', status=1,
           expect=("error: from /GstPipeline:pipeline0/GstVideoTestSrc:"
                   "videotestsrc0: Internal data stream error.",
                   "gst_base_src_loop (): /GstPipeline:pipeline0/"
                   "GstVideoTestSrc:videotestsrc0:",
                   "streaming stopped, reason error (-5)")),
    run("errors.md", "flow-error-debug", "GST_DEBUG=*:1", status=1,
        expect=("gstfaulty.c:311:gst_faulty_transform_ip:<faulty0>", "Boom!")),
    output("errors.md", "missing-file", "error: could not start the pipeline",
           f'{PY} "filesrc location=/tmp/nosuchfile ! fakesink"', status=1,
           expect=("error: could not start the pipeline",
                   "error: from /GstPipeline:pipeline0/GstFileSrc:filesrc0:"
                   " Resource not found.")),
    output("errors.md", "hang", "stalled: no data for 1 s",
           f'{PY} --stall-timeout 1 "videotestsrc num-buffers=50 !'
           ' faulty num-buffers=5 method=hang ! fakesink"', status=3,
           expect=("stalled: no data for 1 s",
                   "error: the pipeline did not stop within 2 s")),
    output("errors.md", "stop", "error: could not stop the pipeline",
           f'{PY} "videotestsrc num-buffers=50 ! faulty num-buffers=5'
           ' method=stop ! fakesink"', status=1,
           expect=("eos", "error: could not stop the pipeline")),
    run("errors.md", "dot-files", "GST_DEBUG_DUMP_DOT_DIR=/tmp/dots",
        expect=("-gst-launch.NULL_READY.dot", "-gst-launch.READY_PAUSED.dot",
                "-gst-launch.PAUSED_PLAYING.dot", "-gst-launch.error.dot")),

    # --- crashes.md
    illustrative("crashes.md", "macos-segfault", "Segmentation fault: 11",
                 "the message is the shell's, and bash -c prints another"),
    output("crashes.md", "linux-caught-sigsegv", "Caught SIGSEGV",
           "gst-launch-1.0 videotestsrc num-buffers=50"
           " ! faulty num-buffers=5 method=custom ! fakesink",
           only="linux", hangs=True, timeout=10,
           expect=("Caught SIGSEGV", "Spinning.  Please run 'gdb gst-launch-1.0 ",
                   "' to continue debugging, Ctrl-C to quit, or Ctrl-\\ to dump core.")),
    not_run("crashes.md", "debugger", "lldb -o run --", "a debugger session"),
    illustrative("crashes.md", "debugger-output", "stop reason = EXC_BAD_ACCESS",
                 "a macOS debugger session"),
    illustrative("crashes.md", "ips", "gst-launch-1.0-2026-11-17-101813.ips",
                 "an abridged macOS crash report"),
    not_run("crashes.md", "core-dump", "ulimit -c unlimited",
            "core dumps are off in the test image, so there is no core to open"),
    illustrative("crashes.md", "core-dump-output", "Program terminated with signal",
                 "a gdb session on a core file"),
    *[run("crashes.md", f"supervise-{lang}", contains, status=1,
          timeout=RUST_BUILD if lang == "rust" else 60,
          needs=("cargo",) if lang == "rust" else (),
          shows="restarting (1/1)", check=supervised,
          expect=("crashed: SIGSEGV", "restarting (1/1)"))
      for lang, contains in [("python", "python3 examples/python/supervise.py"),
                             ("c", "build/examples/c/supervise --restarts 1"),
                             ("rust", "--bin supervise -- --restarts 1")]],
    output("crashes.md", "faulthandler", "Fatal Python error: Segmentation fault",
           "python3 examples/python/supervise.py --restarts 0 " + SIGSEGV_PIPELINE,
           status=1, check=python_c_stack,
           expect=("Fatal Python error: Segmentation fault",)),

    # --- leaks.md
    run("leaks.md", "tracer", "GST_TRACERS=leaks gst-launch-1.0 -q", expect=(
        "WARNING **: ", "Leaks detected and logged under GST_DEBUG=GST_TRACER:7")),
    output("leaks.md", "object-alive", "object-alive, type-name=(string)GstBuffer",
           "GST_TRACERS=leaks GST_DEBUG=GST_TRACER:7 gst-launch-1.0 -q"
           " videotestsrc num-buffers=110 ! leaky method=ref ! fakesink 2>&1"
           " | grep 'object-alive, type-name=(string)GstBuffer,'",
           check=leaked_buffers,
           expect=("object-alive, type-name=(string)GstBuffer, address=(gpointer)0x",
                   "size 614400,", "ref-count=(uint)1, trace=(string);")),
    run("leaks.md", "check-refs", "check-refs=true", expect=(
        ", type-name=(string)GstBuffer, address=(gpointer)0x",
        "description=(string)reffed, ref-count=(uint)2, trace=(string)",
        # on Linux the plugin's frames print as `??`, not every run
        *on("darwin", "leaky_ref_leak"))),
    run("leaks.md", "fatal-warnings", "G_DEBUG=fatal-warnings",
        check=last_line("133"), expect=("Leaks detected",)),
    *[run("leaks.md", f"leak-check-{lang}", contains, status=1, shows="leaked: 10",
          timeout=RUST_BUILD if lang == "rust" else 60,
          needs=("cargo",) if lang == "rust" else (),
          expect=("leaked: 10 GstBuffer", "leaked: 1 GstCaps",
                  "leaked: 10 GstMemory", "leaked: 1 GstVideoBufferPool"))
      for lang, contains in [("python", "python3 examples/python/leak_check.py"),
                             ("c", "build/examples/c/leak-check"),
                             ("rust", "--bin leak-check")]],
    run("leaks.md", "leaks-macos", "MallocStackLogging=1 leaks", only="darwin",
        needs=("leaks",), status=1,
        expect=("9 INSTANCES OF 'ROOT LEAK: <malloc in leaky_malloc_leak>'",)),
    run("leaks.md", "valgrind", "valgrind --leak-check=full", only="linux",
        needs=("valgrind",), expect=(
            "36 bytes in 9 blocks are definitely lost",
            "leaky_malloc_leak (gstleaky.c:76)", "leaky_call_method (gstleaky.c:96)",
            "gst_leaky_transform_ip (gstleaky.c:204)")),
    run("leaks.md", "asan", "meson setup /tmp/asan", only="linux", needs=ASAN,
        timeout=300, status=1, expect=(
            "ERROR: LeakSanitizer: detected memory leaks",
            "Direct leak of 36 byte(s) in 9 object(s)", " in leaky_malloc_leak ",
            "gstleaky.c:76", "gstleaky.c:96", "gstleaky.c:204")),

    # --- memory-corruption.md
    output("memory-corruption.md", "criticals", "GST_IS_BUFFER (buffer)",
           "gst-launch-1.0 videotestsrc num-buffers=50 ! faulty num-buffers=5"
           " method=unref ! fakesink", status=(1, SIGTRAP),
           expect=("gst_video_frame_map_id: assertion 'GST_IS_BUFFER (buffer)' failed",
                   *on("linux", "gst_mini_object_unref: assertion "
                       "'GST_MINI_OBJECT_REFCOUNT_VALUE (mini_object) > 0' failed"))),
    run("memory-corruption.md", "fatal-criticals", "G_DEBUG=fatal-criticals",
        status=SIGTRAP, timeout=20, expect=("CRITICAL **",)),
    run("memory-corruption.md", "valgrind", "valgrind gst-launch-1.0",
        only="linux", needs=("valgrind",), expect=(
            "Invalid write of size 4", "faulty_use_after_free (gstfaulty.c:143)",
            "faulty_call_method (gstfaulty.c:200)",
            "gst_faulty_transform_ip (gstfaulty.c:341)",
            "faulty_use_after_free (gstfaulty.c:142)",
            "0 bytes inside a block of size 4 free'd")),
    run("memory-corruption.md", "asan", "meson setup /tmp/asan", only="linux",
        needs=ASAN, timeout=300, status=1, expect=(
            "ERROR: AddressSanitizer: heap-use-after-free on address 0x",
            "in faulty_use_after_free ", "gstfaulty.c:143", "gstfaulty.c:142",
            "freed by thread T",
            "previously allocated by thread T")),

    # --- blocking.md
    run("blocking.md", "watchdog", "watchdog timeout=300", status=1, expect=(
        "Stalling the streaming thread for 2000 ms.",
        "ERROR: from element /GstPipeline:pipeline0/GstWatchdog:watchdog0:"
        " Watchdog triggered", "Execution ended after ")),
    run("blocking.md", "latency", "GST_TRACERS=latency", check=latency_of_the_stall,
        check_shown=True,
        expect=("latency, ", "src-element=(string)videotestsrc0",
                "sink-element=(string)fakesink0", "time=(guint64)")),
    run("blocking.md", "rusage", "GST_TRACERS=rusage", check=rusage_of_the_spin,
        check_shown=True,
        expect=("thread-rusage, thread-id=(guint64)", "average-cpuload=(uint)",
                "time=(guint64)")),
    illustrative("blocking.md", "top", "PID USER", "an interactive Linux tool"),
    illustrative("blocking.md", "lldb-spin", "faulty_spin(ms=10000)",
                 "a macOS debugger session"),
    not_run("blocking.md", "attach", "lldb -p $!",
            "attaching a debugger to a process needs ptrace rights"),
    illustrative("blocking.md", "lldb-deadlock", "thread #9, name = 'faulty-deadlock'",
                 "a macOS debugger session"),
    illustrative("blocking.md", "gdb-deadlock", "Thread 4 (Thread",
                 "a gdb session in Docker"),
    illustrative("blocking.md", "lldb-main-thread", "* thread #1, queue =",
                 "a macOS debugger session"),

    # --- timestamps-and-events.md
    run("timestamps-and-events.md", "no-pts-qtmux", "! mangler mode=no-pts"
        " num-buffers=5 ! qtmux", status=1, expect=(
            "ERROR: from element /GstPipeline:pipeline0/GstQTMux:qtmux0:"
            " Could not multiplex stream.",
            "gst_qt_mux_add_buffer (): /GstPipeline:pipeline0/GstQTMux:qtmux0:",
            "Buffer has no PTS.")),
    run("timestamps-and-events.md", "no-pts-matroskamux", "matroskamux:2",
        check=lambda out: count(out, "Invalid buffer timestamp; dropping buffer", 15),
        expect=("gst_matroska_mux_write_data:<matroskamux0:video_0>",
                "Invalid buffer timestamp; dropping buffer")),
    run("timestamps-and-events.md", "backwards", "mode=backwards num-buffers=5 \\\n"
        "    ! identity", check=lambda out: count(out, "imperfect-timestamp, ", 1),
        expect=("(element): imperfect-timestamp, delta=(gint64)-166666666,"
                " prev-timestamp=(guint64)133333333,", "cur-timestamp=(guint64)0,")),
    output("timestamps-and-events.md", "gap-identity", "delta=(gint64)1000000000",
           "gst-launch-1.0 -m " + IDENTITY.format(mode="gap") + " 2>&1"
           " | grep imperfect-timestamp",
           check=lambda out: count(out, "imperfect-timestamp, ", 1),
           expect=("(element): imperfect-timestamp, delta=(gint64)1000000000,"
                   " prev-timestamp=(guint64)133333333,",
                   "cur-timestamp=(guint64)1166666666,")),
    run("timestamps-and-events.md", "gap-sync", "time gst-launch-1.0 -q",
        check=gap_seconds, check_shown=True),
    output("timestamps-and-events.md", "bad-duration", "delta=(gint64)-33333334",
           "gst-launch-1.0 -m " + IDENTITY.format(mode="bad-duration") + " 2>&1"
           " | grep imperfect-timestamp",
           check=bad_durations,
           expect=("(element): imperfect-timestamp, delta=(gint64)-33333334,"
                   " prev-timestamp=(guint64)166666666,"
                   " prev-duration=(guint64)66666668,",
                   "cur-timestamp=(guint64)200000000,")),
    run("timestamps-and-events.md", "backwards-qtmux", "GST_DEBUG=qtmux:1",
        check=lambda out: count(out, "decreasing DTS value", 4),
        expect=("gst_qt_mux_add_buffer",
                "decreasing DTS value 0:00:00.000000000 < 0:00:00.133333333",
                "decreasing DTS value 0:00:00.033333334 < 0:00:00.133333333")),
    run("timestamps-and-events.md", "before-segment", "mode=before-segment",
        exact=True),
    run("timestamps-and-events.md", "drop-eos", "mode=drop-eos", needs=("timeout",),
        expect=("124\n0\n1",)),
    run("timestamps-and-events.md", "drop-segment-warning", "gst-launch-1.0"
        " videotestsrc num-buffers=20 ! mangler mode=drop-segment", expect=(
            "gst_pad_push_data:<mangler0:src> Got data flow before segment event",
            "gst_pad_chain_data_unchecked:<fakesink0:sink> Got data flow"
            " before segment event")),
    run("timestamps-and-events.md", "drop-segment-validate", "gst-validate-1.0"
        " videotestsrc num-buffers=20 ! mangler mode=drop-segment",
        needs=("gst-validate-1.0",), expect=(
            "buffer was received before a segment (buffer::before-segment)",
            "EOS received without segment event before"
            " (event::eos-without-segment)",
            "We got a g_log warning (g-log::warning)",
            "Detected on <mangler0:src, fakesink0:sink>", "Issues found: 4",
            "=======> Test PASSED (Return value: 0)")),
    run("timestamps-and-events.md", "drop-stream-start", "mode=drop-stream-start",
        status=1, exact=True),  # grep -c exits 1 when it counts 0
    run("timestamps-and-events.md", "sticky-storm", "grep -c 'E (type: caps'",
        exact=True),
    run("timestamps-and-events.md", "sticky-storm-validate", "gst-validate-1.0"
        " videotestsrc num-buffers=20 ! mangler mode=sticky-storm",
        needs=("gst-validate-1.0",), expect=(
            "received the same caps twice (event::caps-duplicate)",
            "Detected on <fakesink0:sink>", "Issues found: 1",
            "=======> Test PASSED (Return value: 0)")),
]

SAMPLES_BY_ID = {s.id: s for s in SAMPLES}
assert len(SAMPLES_BY_ID) == len(SAMPLES), "two samples have the same id"


# --- finding the blocks of a sample --------------------------------------------

def matching(sample, blocks=BLOCKS):
    start = sample.contains.startswith("^")
    text = sample.contains.removeprefix("^")
    return [b for b in blocks if b.page == sample.page
            and b.category == sample.shown_category
            and (b.text.startswith(text) if start else text in b.text)]


def shown_block(sample, blocks=BLOCKS):
    """The output block that shows what a `run` or `output` sample prints."""
    if sample.kind == "output":
        return matching(sample, blocks)[0]
    if sample.shows is None:
        return None
    here = matching(sample, blocks)[0]
    if sample.shows is AUTO:
        following = blocks[blocks.index(here) + 1:][:1]
        return following[0] if following and following[0].category == "output" else None
    shown = [b for b in blocks if b.page == sample.page
             and b.category == "output" and sample.shows in b.text]
    return shown[0] if len(shown) == 1 else None


def test_every_block_is_covered():
    problems = []
    own = {}    # block -> the samples that match it
    shown = {}  # output block -> the run samples that show it
    for sample in SAMPLES:
        found = matching(sample)
        if len(found) != 1:
            problems.append(f"{sample.id}: {sample.contains!r} matches"
                            f" {len(found)} {sample.shown_category} blocks")
            continue
        if sample.kind == "run":
            block = shown_block(sample)
            if block:
                shown.setdefault(block, []).append(sample)
            elif sample.shows not in (None, AUTO):
                problems.append(f"{sample.id}: shows={sample.shows!r} matches"
                                " no single output block")
        else:
            own.setdefault(found[0], []).append(sample)
        if sample.kind == "run" and not sample.command:
            own.setdefault(found[0], []).append(sample)
    for block in BLOCKS:
        if block.category == "example":
            continue
        mine, showing = own.get(block, []), shown.get(block, [])
        if len(mine) + bool(showing) == 0:
            problems.append(f"{block.where}: this {block.category} block has no"
                            " sample in tests/test_doc_samples.py")
        elif len(mine) + bool(showing) > 1:
            problems.append(f"{block.where}: this {block.category} block has"
                            f" several samples: {[s.id for s in mine + showing]}")
    assert not problems, "\n".join(problems)


# --- running a sample -----------------------------------------------------------

def skip_reason(sample):
    if sample.only and not sys.platform.startswith(sample.only):
        return f"only on {sample.only}"
    for tool in sample.needs:
        if tool == "libasan":
            gcc = shutil.which("gcc")
            found = gcc and subprocess.run(
                [gcc, "-print-file-name=libasan.so"], capture_output=True,
                text=True).stdout.strip()
            if not found or not os.path.isabs(found):
                return "needs libasan"
        elif shutil.which(tool) is None:
            return f"needs {tool}"


def run_command(command, tmp_path, timeout):
    """Run a command of the docs: its output, its status, and whether it timed out."""
    # conftest.py has dropped the GST_DEBUG and friends of the caller's shell
    env = dict(os.environ)
    # `python3` in the docs is the Python that has the GStreamer bindings
    env["PATH"] = os.pathsep.join([str(Path(sys.executable).parent), env["PATH"]])
    # `time` prints 1,828s in a locale with a decimal comma
    env["LC_ALL"] = "C"
    proc = subprocess.Popen(
        ["bash", "-c", command.replace("/tmp/", f"{tmp_path}/")], cwd=ROOT,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        errors="replace", env=env, start_new_session=True)
    timed_out = False
    try:
        out, _ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        out, _ = proc.communicate()
        timed_out = True
    status = proc.returncode
    return out, 128 - status if status < 0 else status, timed_out


def expected_statuses(sample):
    status = sample.status
    if isinstance(status, dict):
        status = status[sys.platform]
    return status if isinstance(status, tuple) else (status,)


@pytest.mark.parametrize("sample", [s for s in SAMPLES if s.kind in ("run", "output")],
                         ids=lambda s: s.id)
def test_sample(sample, tmp_path):
    reason = skip_reason(sample)
    if reason:
        pytest.skip(reason)
    shown = shown_block(sample)
    assert not (shown and shown.title.startswith(ILLUSTRATIVE)), \
        f"{shown.where}: a checked output must not be titled illustrative"
    command = sample.command or matching(sample)[0].text
    out, status, timed_out = run_command(command, tmp_path, sample.timeout)
    assert timed_out == sample.hangs, f"timed_out={timed_out}:\n{out}"
    if not sample.hangs:
        assert status in expected_statuses(sample), f"exit status {status}:\n{out}"
    for text in sample.expect:
        assert text in out, f"{text!r} is not in the real output:\n{out}"
        if shown:
            assert text in shown.text, f"{text!r} is not in {shown.where}"
    if sample.exact:
        assert out.strip() == shown.text.strip(), f"differs from {shown.where}:\n{out}"
    if sample.check:
        sample.check(out)
    if sample.check_shown:
        assert shown, f"{sample.id}: no output block to check"
        sample.check(shown.text)


@pytest.mark.parametrize("sample", [s for s in SAMPLES if s.kind == "illustrative"],
                         ids=lambda s: s.id)
def test_illustrative_block_says_so(sample):
    block = matching(sample)[0]
    assert block.title.startswith(ILLUSTRATIVE), \
        f'{block.where}: add title="{ILLUSTRATIVE}: {sample.why}" to the fence'


def test_samples_say_why_they_are_not_run():
    assert [s.id for s in SAMPLES if s.kind in ("not-run", "illustrative")
            and not s.why] == []
