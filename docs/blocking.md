# Catching blocked pipelines

A blocked pipeline does not crash, so there is no crash report and no stack
trace: it just stops. Which tool tells you why depends on how it is stuck.
faulty has three methods that block the streaming thread on purpose once
`num-buffers` buffers have passed:

| `method`   | The bug                                                       | Caught by                |
|------------|---------------------------------------------------------------|--------------------------|
| `stall`    | sleeps for `stall-time` ms (default 2000) once, then goes on  | watchdog, latency tracer |
| `spin`     | busy-loops for `stall-time` ms once, then goes on             | rusage tracer, `top`     |
| `deadlock` | two threads take two locks in opposite orders, for good       | a debugger's `bt all`    |

`stall` and `spin` are bounded so tests can wait for them; `hang` is the
endless one. Each logs what it does at ERROR level (`GST_DEBUG=faulty:1`).

## stall: the watchdog and the latency tracer

A stalled thread sleeps: no CPU, no errors, just no data. The `watchdog`
element (gst-plugins-bad) posts an error when no buffer passes it for
`timeout` ms. Put it downstream of the suspect, with a timeout below the
stall:

```sh
GST_DEBUG=faulty:1 gst-launch-1.0 videotestsrc num-buffers=50 \
    ! faulty num-buffers=5 method=stall ! watchdog timeout=300 ! fakesink
```

```
… gst_faulty_transform_ip:<faulty0> Stalling the streaming thread for 2000 ms.
ERROR: from element /GstPipeline:pipeline0/GstWatchdog:watchdog0: Watchdog triggered
Execution ended after 0:00:00.436749416
```

The watchdog reports at 300 ms, but the process still exits only after the
full 2 s: shutting down needs the sink pad's stream lock, which the sleeping
streaming thread holds. A stall below the timeout passes unnoticed: with
`stall-time=100` the same pipeline reaches `Got EOS` and exits 0.

The latency tracer logs, for every buffer, how long it took from source to
sink in nanoseconds, so it also finds slowdowns too short for a watchdog. The
line for the stalled buffer is logged when it arrives, after the stall:

```sh
GST_TRACERS=latency GST_DEBUG=GST_TRACER:7 gst-launch-1.0 -q videotestsrc num-buffers=10 \
    ! faulty num-buffers=5 method=stall ! fakesink 2>&1 | grep ' latency, src'
```

```
… latency, …, src-element=(string)videotestsrc0, …, sink-element=(string)fakesink0, …, time=(guint64)5125, ts=…
… latency, …, src-element=(string)videotestsrc0, …, sink-element=(string)fakesink0, …, time=(guint64)2150039625, ts=…
… latency, …, src-element=(string)videotestsrc0, …, sink-element=(string)fakesink0, …, time=(guint64)34042, ts=…
```

`GST_TRACERS='latency(flags=element)'` adds an `element-latency` line per
element, which names the culprit: `element=(string)faulty0, …,
time=(guint64)2023647875`.

## spin: the rusage tracer and top

A spinning thread is as stuck as a sleeping one, but it burns a core. The
rusage tracer logs each thread's CPU time (`time`, ns) and load (per mille)
on every buffer; keep the last line per thread:

```sh
GST_TRACERS=rusage GST_DEBUG=GST_TRACER:7 gst-launch-1.0 -q videotestsrc num-buffers=10 \
    ! faulty num-buffers=5 method=spin ! fakesink 2>&1 \
    | grep -o 'thread-rusage, thread-id=(guint64)[0-9].*' \
    | awk '{last[$2]=$0} END {for (t in last) print last[t]}'
```

```
thread-rusage, thread-id=(guint64)191303737194544, …, average-cpuload=(uint)12, …, time=(guint64)27269509;
thread-rusage, thread-id=(guint64)191303738668032, …, average-cpuload=(uint)912, …, time=(guint64)2004144784;
```

One thread used 2.0 s of CPU in the 2.2 s run; with `method=stall`
no thread uses more than 30 ms. Like the latency tracer, it reports only once
the spin ends. To watch it happen, look at the threads: on Linux,
`top -H -p <pid>` names them (cut to 15 characters):

```text title="Illustrative output: top on Linux"
    PID USER      PR  NI    VIRT    RES    SHR S  %CPU  %MEM     TIME+ COMMAND
     13 root      20   0  163656  11060   6300 R  99.0   0.6   0:02.26 videote+
```

macOS `top` shows only the process (`gst-launch-1.0 98.4`); lldb's `bt all`
names the thread and the loop:

```text title="Illustrative output: lldb on macOS"
  thread #7, name = 'videotestsrc0:src'
      frame #0: …libsystem_kernel.dylib`mach_absolute_time + 108
      frame #1: …libglib-2.0.0.dylib`g_get_monotonic_time_ns + 24
      frame #2: …libglib-2.0.0.dylib`g_get_monotonic_time + 12
      frame #3: …libgstcrash.dylib`faulty_spin(ms=10000) at gstfaulty.c:152:10
      frame #4: …libgstcrash.dylib`gst_faulty_transform_ip(base=…, buf=…) at gstfaulty.c:330:9
```

Without any tool, `time` says the same: a 2 s stall has `user 0m0.019s`, a
2 s spin `user 0m2.013s`.

## deadlock: a debugger

The streaming thread holds the sink pad's stream lock (A), taken by
`gst_pad_chain` before it calls faulty, and waits for the element's object
lock (B). A helper thread, `faulty-deadlock`, holds B and waits for A. Neither
moves again: no CPU, no log line after `Deadlocking the streaming thread.`,
and Ctrl-C does nothing. Attach a debugger and dump every thread:

```sh
gst-launch-1.0 videotestsrc num-buffers=50 ! faulty num-buffers=5 method=deadlock ! fakesink &
sleep 1.5
lldb -p $! --batch -o "bt all"                    # macOS
gdb -p $! -batch -ex "thread apply all bt"        # Linux
kill -9 $!
```

```text title="Illustrative output: lldb on macOS"
  thread #7, name = 'videotestsrc0:src'
      frame #0: …libsystem_kernel.dylib`__psynch_mutexwait + 8
      …
      frame #3: …libglib-2.0.0.dylib`g_mutex_lock + 28
      frame #4: …libgstcrash.dylib`faulty_deadlock(faulty=…) at gstfaulty.c:181:3
      frame #5: …libgstcrash.dylib`gst_faulty_transform_ip(base=…, buf=…) at gstfaulty.c:336:5
      …
      frame #8: …libgstreamer-1.0.0.dylib`gst_pad_chain_data_unchecked + 488
      …
  thread #9, name = 'faulty-deadlock'
      frame #0: …libsystem_kernel.dylib`__psynch_mutexwait + 8
      …
      frame #3: …libgstcrash.dylib`faulty_deadlock_thread(data=…) at gstfaulty.c:165:3
      frame #4: …libglib-2.0.0.dylib`g_thread_proxy + 84
```

gdb in Docker (`docker compose run --rm --cap-add=SYS_PTRACE shell`, so it
may attach) shows the same pair; Debian's GStreamer frames have no names:

```text title="Illustrative output: gdb in Docker"
Thread 4 (Thread … (LWP 15) "faulty-deadlock"):
#0  futex_wait (private=0, expected=2, futex_word=…) at ../sysdeps/nptl/futex-internal.h:146
…
#4  … in faulty_deadlock_thread (data=…) at ../gst/gstfaulty.c:165
Thread 2 (Thread … (LWP 13) "videotestsrc0:s"):
#0  syscall () at ../sysdeps/unix/sysv/linux/aarch64/syscall.S:38
#1  … in  () at /lib/aarch64-linux-gnu/libglib-2.0.so.0
#2  … in faulty_deadlock (faulty=… [GstFaulty|faulty0]) at ../gst/gstfaulty.c:181
```

To read it, find the threads in a mutex wait (`__psynch_mutexwait`,
`futex_wait`) and take each one's innermost frame of your own code: that line
is the lock it wants. `faulty_deadlock` at line 181 wants B
(`GST_OBJECT_LOCK`), and came through `gst_pad_chain_data_unchecked`, so it
holds A. `faulty_deadlock_thread` at line 165 wants A
(`GST_PAD_STREAM_LOCK`), and locked B on line 162: a backtrace shows what a
thread waits for, the code what it holds. Two threads each waiting for what
the other holds is the inversion.

Everything else that needs A or B blocks too, the app included: faulty's
property getters take the object lock, so `tests/faulty_deadlock.py`'s main
thread freezes in `faulty.get_property()`:

```text title="Illustrative output: lldb on macOS"
* thread #1, queue = 'com.apple.main-thread', stop reason = signal SIGSTOP
  * frame #0: …libsystem_kernel.dylib`__psynch_mutexwait + 8
    …
    frame #4: …libgstcrash.dylib`gst_faulty_get_property(object=…, prop_id=1, value=…, pspec=…) at gstfaulty.c:417:3
    frame #5: …libgobject-2.0.0.dylib`g_object_get_property + 416
```

`set_state(NULL)` never returns either. A `hang`, by contrast, shows one
thread in `g_usleep` and nothing else stuck.

## In the tests

`tests/test_faulty.py` checks that `stall` triggers a watchdog and that a
short one reaches EOS, that `spin`'s CPU time (`RUSAGE_CHILDREN`) is close to
its wall time, and that `deadlock` blocks gst-launch-1.0 and a property read
(`tests/faulty_deadlock.py`, which `faulthandler` ends after 3 s).
`tests/test_doc_samples.py` runs the watchdog, latency and rusage commands on
this page and checks the output shown; `top` and the debugger sessions are
marked illustrative.
