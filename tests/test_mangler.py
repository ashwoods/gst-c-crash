"""Run each mangler mode in a child gst-launch-1.0 and check what it breaks.

Each test looks at what an element, muxer or app downstream of mangler sees,
or at what gst-validate-1.0 reports where it reports the mode. Debian has no
gst-validate, so those tests run only where it is installed.
"""
import os
import shutil
import subprocess
import sys

import pytest

TIMEOUT = 10
SOURCE = 20
NUM_BUFFERS = 5
MANGLED = SOURCE - NUM_BUFFERS
MODES = ["no-pts", "backwards", "gap", "bad-duration", "before-segment",
         "drop-eos", "drop-segment", "drop-stream-start", "sticky-storm"]

needs_validate = pytest.mark.skipif(shutil.which("gst-validate-1.0") is None,
                                    reason="needs gst-validate-1.0")


def launch(mode, downstream="fakesink", num_buffers=NUM_BUFFERS,
           upstream="", tool=("gst-launch-1.0", "--no-fault"), env=None,
           timeout=TIMEOUT):
    source = f"videotestsrc num-buffers={SOURCE}" + (
        f" ! {upstream}" if upstream else "")
    return subprocess.run(
        [*tool, *source.split(), "!", "mangler", f"mode={mode}",
         f"num-buffers={num_buffers}", "!", *downstream.split()],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=timeout,
        env={**os.environ, **(env or {})},
    )


def imperfect(mode, num_buffers=NUM_BUFFERS):
    """The imperfect-timestamp messages identity posts behind mangler."""
    p = launch(mode, "identity check-imperfect-timestamp=true ! fakesink",
               num_buffers, tool=("gst-launch-1.0", "--no-fault", "-m"))
    assert p.returncode == 0, p.stdout
    return [line for line in p.stdout.splitlines()
            if "(element): imperfect-timestamp," in line]


@pytest.mark.parametrize("mode", MODES)
def test_untouched_before_num_buffers(mode):
    # the source ends before num-buffers have passed: timestamps stay perfect,
    # and the stream reaches EOS
    assert imperfect(mode, num_buffers=SOURCE + 1) == []


def test_no_pts_fails_qtmux():
    p = launch("no-pts", "qtmux ! fakesink", upstream="jpegenc")
    assert p.returncode == 1, p.stdout
    assert "Buffer has no PTS." in p.stdout


def deltas(mode):
    return [int(line.split("delta=(gint64)")[1].split(",")[0])
            for line in imperfect(mode)]


def test_backwards_jumps_back_to_the_first_timestamp():
    # once, from buffer 4 (133 ms, lasting 33 ms) to buffer 0's 0 ms
    assert deltas("backwards") == [-166666666]


def test_gap_jumps_ahead_a_second():
    assert deltas("gap") == [1000000000]


def test_bad_duration_overlaps_every_next_buffer():
    # each doubled duration runs a frame (33 ms, rounded) into the next buffer
    found = deltas("bad-duration")
    assert len(found) == MANGLED - 1, found
    assert set(found) <= {-33333333, -33333334}, found


def test_before_segment_buffers_are_dropped():
    # the new segment starts a second ahead: the sink drops every buffer after
    # num-buffers, since the source ends before the segment starts
    p = launch("before-segment", env={"GST_DEBUG": "basesink:5"})
    assert p.returncode == 0, p.stdout
    assert p.stdout.count("dropping buffer, out of clipping segment") == \
        MANGLED


def test_drop_eos_hangs_and_leaves_mp4_without_moov(tmp_path):
    # the moov atom is written on EOS: without it the file cannot be played
    out = tmp_path / "out.mov"
    with pytest.raises(subprocess.TimeoutExpired):
        # unbuffered, so what qtmux wrote is on disk when the timeout kills it
        launch("drop-eos",
               f"qtmux ! filesink buffer-mode=unbuffered location={out}",
               upstream="jpegenc", timeout=3)
    data = out.read_bytes()
    assert b"mdat" in data
    assert b"moov" not in data


@pytest.mark.parametrize("num_buffers, seen", [(0, False), (NUM_BUFFERS, True)])
def test_drop_stream_start_hides_stream_start_message(num_buffers, seen):
    # stream-start comes before any buffer, so only num-buffers=0 drops it
    p = launch("drop-stream-start", num_buffers=num_buffers,
               tool=("gst-launch-1.0", "--no-fault", "-m"))
    assert p.returncode == 0, p.stdout
    assert ('from element "pipeline0" (stream-start)' in p.stdout) == seen


@pytest.mark.parametrize("num_buffers, warned", [(0, True),
                                                 (NUM_BUFFERS, False)])
def test_drop_segment_lets_data_flow_before_segment(num_buffers, warned):
    # GStreamer's own check, on in its default builds; the first segment comes
    # before any buffer, so only num-buffers=0 drops it
    p = launch("drop-segment", num_buffers=num_buffers)
    assert p.returncode == 0, p.stdout
    assert ("Got data flow before segment event" in p.stdout) == warned


def test_sticky_storm_resends_caps_before_every_buffer():
    p = launch("sticky-storm", "fakesink silent=false",
               tool=("gst-launch-1.0", "--no-fault", "-v"))
    assert p.returncode == 0, p.stdout
    # the negotiated caps, then one more for each buffer after num-buffers
    assert p.stdout.count("E (type: caps") == 1 + MANGLED


@needs_validate
@pytest.mark.parametrize("mode, num_buffers, issues", [
    ("drop-segment", 0, ["buffer::before-segment",
                         "event::eos-without-segment"]),
    ("sticky-storm", NUM_BUFFERS, ["event::caps-duplicate"]),
])
def test_validate_reports(mode, num_buffers, issues):
    p = launch(mode, num_buffers=num_buffers, tool=("gst-validate-1.0",))
    for issue in issues:
        assert f"({issue})" in p.stdout, p.stdout


def test_num_buffers_restarts_with_each_play():
    # one element, played twice: the countdown starts over on the second play
    helper = os.path.join(os.path.dirname(__file__), "mangler_restart.py")
    p = subprocess.run(
        [sys.executable, helper, str(NUM_BUFFERS)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=TIMEOUT,
    )
    assert p.returncode == 0, p.stdout
    assert p.stdout.split()[-2:] == ["eos:5", "eos:5"], p.stdout
