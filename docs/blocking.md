# Catching blocked pipelines

A pipeline that stops making progress does not crash, so there is no crash
report and no stack trace: it just sits there. Which tool tells you why
depends on how it is stuck. faulty has three methods that block the streaming
thread on purpose once `num-buffers` buffers have passed:

| `method`   | The bug                                                                  | Caught by                  |
|------------|--------------------------------------------------------------------------|----------------------------|
| `stall`    | sleeps for `stall-time` once, then passes buffers again                  | watchdog, latency tracer   |
| `spin`     | busy-loops for `stall-time` once, then passes buffers again              | rusage tracer, `top`       |
| `deadlock` | takes two locks in opposite orders in two threads, and never recovers    | a debugger, `bt all`       |

`stall-time` is in milliseconds and defaults to 2000. `stall` and `spin` are
bounded on purpose, so a test can wait for them; `hang` is the endless one. Each
logs what it does at ERROR level, which `GST_DEBUG=faulty:1` shows, and the
countdown starts again each time the element starts.

## stall: the watchdog

A stalled streaming thread sleeps, so it uses no CPU and nothing looks wrong
except that no data arrives. The `watchdog` element (from
gst-plugins-bad, `debugutilsbad`) posts an error on the bus when no buffer
passes it for `timeout` milliseconds, which is 1000 by default. Put it
downstream of what you suspect, with a timeout below the stall:

```sh
GST_DEBUG=faulty:1 gst-launch-1.0 videotestsrc num-buffers=50 \
    ! faulty num-buffers=5 method=stall ! watchdog timeout=300 ! fakesink
```

```
… faulty … gst_faulty_transform_ip:<faulty0> Stalling the streaming thread for 2000 ms.
ERROR: from element /GstPipeline:pipeline0/GstWatchdog:watchdog0: Watchdog triggered
Additional debug info:
…/gstwatchdog.c(196): gst_watchdog_trigger (): /GstPipeline:pipeline0/GstWatchdog:watchdog0:
Watchdog triggered
Execution ended after 0:00:00.436749416
```

(macOS; Linux prints the same lines.) The watchdog only says where the data
stopped and when. Note that it ends the pipeline at 300 ms, long before the
stall would have. A stall shorter than the timeout passes unnoticed: with
`stall-time=100` and `timeout=2000` the same pipeline reaches
`Got EOS from element "pipeline0".` and exits 0.

The latency tracer shows the stall after the fact, and for every buffer, so it
also finds slow elements that the watchdog's one timeout would miss. It logs
how long each buffer took from the source to the sink, in nanoseconds:

```sh
GST_TRACERS=latency GST_DEBUG=GST_TRACER:7 gst-launch-1.0 -q videotestsrc num-buffers=10 \
    ! faulty num-buffers=5 method=stall ! fakesink 2>&1 | grep ' latency, src'
```

```
… GST_TRACER :0:: latency, …, src-element=(string)videotestsrc0, …, sink-element=(string)fakesink0, …, time=(guint64)7458, ts=…
… GST_TRACER :0:: latency, …, src-element=(string)videotestsrc0, …, sink-element=(string)fakesink0, …, time=(guint64)5125, ts=…
0:00:02.194600625 … GST_TRACER :0:: latency, …, src-element=(string)videotestsrc0, …, sink-element=(string)fakesink0, …, time=(guint64)2150039625, ts=…
… GST_TRACER :0:: latency, …, src-element=(string)videotestsrc0, …, sink-element=(string)fakesink0, …, time=(guint64)34042, ts=…
```

(macOS; Linux has the same shape, with `time=(guint64)2001788049` for the
stalled buffer.) The first buffers take microseconds, the sixth one 2.15 s: the
buffer after the `num-buffers` ones that passed. The latency is recorded when
the buffer arrives, so the line is only logged after the stall ends. Add
`flags=element` (`GST_TRACERS='latency(flags=element)'`) to also get the time
each element took, which names the culprit:

```
element-latency, element=(string)faulty0, src=(string)src, time=(guint64)2542, ts=(guint64)20628459;
element-latency, element=(string)faulty0, src=(string)src, time=(guint64)2333, ts=(guint64)20873375;
element-latency, element=(string)faulty0, src=(string)src, time=(guint64)2023647875, ts=(guint64)2044762209;
```

(macOS, another run.)

## spin: the rusage tracer and top

A spinning thread is as stuck as a sleeping one, but it burns a core, so the
machine gets hot and loud and the stall shows up as CPU load instead of
silence. The rusage tracer logs each thread's CPU time and load whenever a
buffer is pushed; `cpuload` is per mille, 1000 being one full core. It prints a
line per push, so keep the last one per thread:

```sh
GST_TRACERS=rusage GST_DEBUG=GST_TRACER:7 gst-launch-1.0 -q videotestsrc num-buffers=10 \
    ! faulty num-buffers=5 method=spin ! fakesink 2>&1 \
    | grep -o 'thread-rusage, thread-id=(guint64)[0-9].*' \
    | awk '{last[$2]=$0} END {for (t in last) print last[t]}'
```

```
thread-rusage, thread-id=(guint64)191303737194544, ts=(guint64)2195925320, average-cpuload=(uint)12, current-cpuload=(uint)938, time=(guint64)27269509;
thread-rusage, thread-id=(guint64)191303738668032, ts=(guint64)2195130792, average-cpuload=(uint)912, current-cpuload=(uint)750, time=(guint64)2004144784;
```

(Linux.) `time` is the thread's CPU time in nanoseconds: one thread has used
2.0 s in the 2.2 s the pipeline ran, an average load of 91%, and the other
27 ms. Thread ids are not names; the busy one is the streaming thread. The same
run with `method=stall` has no such thread: none used more than 30 ms of CPU
and the average loads were at most 12 per mille (macOS and Linux).

Because it is logged on buffers, the tracer reports the spin only once the
spin ends. To see it while it happens, look at the process:

- **Linux**, `top -H` lists one row per thread, with names (cut to 15
  characters), and the spinning one is at 100%:

  ```sh
  top -H -b -n 2 -d 1 -p <pid> | tail -5
  ```

  ```
      PID USER      PR  NI    VIRT    RES    SHR S  %CPU  %MEM     TIME+ COMMAND
       13 root      20   0  163656  11060   6300 R  99.0   0.6   0:02.26 videote+
        6 root      20   0  163656  11060   6300 S  98.0   0.6   0:02.28 gst-lau+
       14 root      20   0  163656  11060   6300 S   0.0   0.6   0:00.00 gmain
  ```

  The row for the main thread (`gst-lau+`, the process's first thread) repeats
  the whole process's CPU time: read the row below it. The thread is
  `videotestsrc0:src`, the source's streaming thread, which runs faulty. A
  stalled pipeline shows 0.0 in every row.

- **macOS**, `top` has no threads, only the process, and `ps -M` lists the
  threads without names:

  ```sh
  top -l 3 -s 1 -pid <pid> -stats pid,command,cpu,th,state | tail -2
  ps -M -p <pid>
  ```

  ```
  PID    COMMAND        %CPU #TH STATE
  81409  gst-launch-1.0 98.4 8/1 running

  USER               PID   TT   %CPU STAT PRI     STIME     UTIME COMMAND
  …                81409   ??    0.1 S    20T   0:00.02   0:00.03 gst-launch-1.0 …
                   81409         0.0 S    20T   0:00.00   0:00.00
                   …
                   81409        98.7 R    20T   0:00.03   0:05.62
                   81409         0.0 S    20T   0:00.00   0:00.00
  ```

  (`ps -M -p <pid>`, with the user name and the command line cut.) The busy
  thread is the one at 98.7% in the running state, and its time is nearly all
  user time (`UTIME`), as it is for any loop that reads the clock in user
  space. A debugger names it (`lldb -p <pid> --batch -o "bt all"`):

  ```
    thread #7, name = 'videotestsrc0:src'
        frame #0: …libsystem_kernel.dylib`mach_absolute_time + 108
        frame #1: …libglib-2.0.0.dylib`g_get_monotonic_time_ns + 24
        frame #2: …libglib-2.0.0.dylib`g_get_monotonic_time + 12
        frame #3: …libgstcrash.dylib`faulty_spin(ms=10000) at gstfaulty.c:152:10
        frame #4: …libgstcrash.dylib`gst_faulty_transform_ip(base=…, buf=…) at gstfaulty.c:330:9
  ```

For a whole run, user time against wall time says the same. A stall has almost
none, a spin has all of it (`time`'s `user` on Linux, `/usr/bin/time -l` on
macOS; here with the default 2 s):

```
              stall                      spin
real          0m2.033s                   0m2.029s
user          0m0.019s                   0m2.013s
```

(Linux, `time gst-launch-1.0 -q videotestsrc num-buffers=10 ! faulty
num-buffers=5 method=<stall|spin> ! fakesink`.)

## deadlock: a debugger

A deadlock is two threads that each wait for a lock the other holds, so
neither ever moves again. There is no timeout, no CPU use and no log line
after `Deadlocking the streaming thread.`: only a stack trace of every thread
shows who waits for what. Ctrl-C does not end it; the process has to be
killed with `kill -9`.

faulty builds the classic AB-BA inversion on purpose. The streaming thread holds the sink pad's stream lock
(A), which `gst_pad_chain` takes before it calls faulty, and waits for the
element's object lock (B). A helper thread named `faulty-deadlock` holds B and
waits for A. Run the pipeline in the background, give it a moment to block,
and attach to it:

```sh
gst-launch-1.0 videotestsrc num-buffers=50 ! faulty num-buffers=5 method=deadlock ! fakesink &
sleep 1.5
lldb -p $! --batch -o "bt all"                    # macOS
gdb -p $! -batch -ex "thread apply all bt"        # Linux
kill -9 $!
```

- **macOS**, `lldb`:

  ```
    thread #7, name = 'videotestsrc0:src'
        frame #0: …libsystem_kernel.dylib`__psynch_mutexwait + 8
        frame #1: …libsystem_pthread.dylib`_pthread_mutex_firstfit_lock_wait + 84
        frame #2: …libsystem_pthread.dylib`_pthread_mutex_firstfit_lock_slow + 216
        frame #3: …libglib-2.0.0.dylib`g_mutex_lock + 28
        frame #4: …libgstcrash.dylib`faulty_deadlock(faulty=…) at gstfaulty.c:181:3
        frame #5: …libgstcrash.dylib`gst_faulty_transform_ip(base=…, buf=…) at gstfaulty.c:336:5
        frame #6: …libgstbase-1.0.0.dylib`default_generate_output + 456
        frame #7: …libgstbase-1.0.0.dylib`gst_base_transform_chain + 288
        frame #8: …libgstreamer-1.0.0.dylib`gst_pad_chain_data_unchecked + 488
        frame #9: …libgstreamer-1.0.0.dylib`gst_pad_push_data + 204
        frame #10: …libgstreamer-1.0.0.dylib`gst_pad_push + 288
        frame #11: …libgstbase-1.0.0.dylib`gst_base_src_loop + 1540
        …
    thread #9, name = 'faulty-deadlock'
        frame #0: …libsystem_kernel.dylib`__psynch_mutexwait + 8
        frame #1: …libsystem_pthread.dylib`_pthread_mutex_firstfit_lock_wait + 84
        frame #2: …libsystem_pthread.dylib`_pthread_mutex_firstfit_lock_slow + 216
        frame #3: …libgstcrash.dylib`faulty_deadlock_thread(data=…) at gstfaulty.c:165:3
        frame #4: …libglib-2.0.0.dylib`g_thread_proxy + 84
        frame #5: …libsystem_pthread.dylib`_pthread_start + 136
  ```

- **Linux**, `gdb` (in Docker: `docker compose run --rm --cap-add=SYS_PTRACE
  shell`, which gives it the right to attach). The Debian
  GStreamer and GLib packages have no symbols for their internal functions, so
  those frames are blank:

  ```
  Thread 4 (Thread … (LWP 15) "faulty-deadlock"):
  #0  futex_wait (private=0, expected=2, futex_word=…) at ../sysdeps/nptl/futex-internal.h:146
  #1  __GI___lll_lock_wait (futex=futex@entry=…, private=private@entry=0) at ./nptl/lowlevellock.c:49
  #2  … in lll_mutex_lock_optimized (mutex=…) at ./nptl/pthread_mutex_lock.c:48
  #3  ___pthread_mutex_lock (mutex=…) at ./nptl/pthread_mutex_lock.c:128
  #4  … in faulty_deadlock_thread (data=…) at ../gst/gstfaulty.c:165
  …
  Thread 2 (Thread … (LWP 13) "videotestsrc0:s"):
  #0  syscall () at ../sysdeps/unix/sysv/linux/aarch64/syscall.S:38
  #1  … in  () at /lib/aarch64-linux-gnu/libglib-2.0.so.0
  #2  … in faulty_deadlock (faulty=… [GstFaulty|faulty0]) at ../gst/gstfaulty.c:181
  #3  … in gst_faulty_transform_ip (base=… [GstBaseTransform|faulty0], buf=…) at ../gst/gstfaulty.c:336
  #4  … in  () at /lib/aarch64-linux-gnu/libgstbase-1.0.so.0
  #5  … in  () at /lib/aarch64-linux-gnu/libgstbase-1.0.so.0
  #6  … in  () at /lib/aarch64-linux-gnu/libgstreamer-1.0.so.0
  #7  … in  () at /lib/aarch64-linux-gnu/libgstreamer-1.0.so.0
  #8  … in gst_pad_push () at /lib/aarch64-linux-gnu/libgstreamer-1.0.so.0
  …
  ```

How to read it. First find the threads that are stuck in a mutex wait
(`__psynch_mutexwait` on macOS, `futex_wait` on Linux): here two, while the
others are idle in `poll`, a run loop or a condition wait. Then
take each one's innermost frame of your own code, which says which lock it
wants:

- `videotestsrc0:src` waits in `faulty_deadlock` at `gstfaulty.c:181`, which
  is `GST_OBJECT_LOCK (faulty)`: lock B. It got there through
  `gst_pad_chain_data_unchecked`, the frame that takes the sink pad's stream
  lock, so it already holds A.
- `faulty-deadlock` waits in `faulty_deadlock_thread` at `gstfaulty.c:165`,
  which is `GST_PAD_STREAM_LOCK`: lock A. The line above it, 162, locked B,
  which no stack shows: a backtrace only shows what a thread is waiting for,
  and the code tells what it holds.

Two threads each waiting for a lock that the other one's code path took: that
is the inversion. Look for such a pair when a pipeline hangs with no CPU use.
(The `faulty-deadlock` frames go straight from `faulty_deadlock_thread` to
the pthread ones, with no `g_rec_mutex_lock` between: the compiler made it a
tail call.)

Everything else that needs A or B blocks too, and the application's own
threads are among them. `get_property` and `set_property` take the object lock,
so an app reading a property from its main thread freezes:
`tests/faulty_deadlock.py` does, with `g_object_get(faulty, "num-buffers")`.
Attached with lldb (macOS):

```
* thread #1, queue = 'com.apple.main-thread', stop reason = signal SIGSTOP
  * frame #0: …libsystem_kernel.dylib`__psynch_mutexwait + 8
    frame #1: …libsystem_pthread.dylib`_pthread_mutex_firstfit_lock_wait + 84
    frame #2: …libsystem_pthread.dylib`_pthread_mutex_firstfit_lock_slow + 216
    frame #3: …libglib-2.0.0.dylib`g_mutex_lock + 28
    frame #4: …libgstcrash.dylib`gst_faulty_get_property(object=…, prop_id=1, value=…, pspec=…) at gstfaulty.c:417:3
    frame #5: …libgobject-2.0.0.dylib`g_object_get_property + 416
    frame #6: …_gi.cpython-315-darwin.so`pygi_get_property_value + 168
    …
```

Without a debugger, Python's `faulthandler` shows where an app is stuck at
the Python level (the test uses `faulthandler.dump_traceback_later(3, exit=True)`):

```
Timeout (0:00:03)!
Thread … (most recent call first):
  File "…/tests/faulty_deadlock.py", line 17 in read_property
  File "…/tests/faulty_deadlock.py", line 30 in main
  File "…/tests/faulty_deadlock.py", line 35 in <module>
```

Python's own stack ends in the property read and cannot say that it waits for
a mutex, nor who holds it: that takes the native backtrace above. Setting the
pipeline's state to NULL hangs as well (`set_state(Gst.State.NULL)` from
Python never returns).

A plain `hang` looks different: its streaming thread sleeps in `g_usleep`,
called from `gst_faulty_transform_ip` (`gstfaulty.c:318` on macOS), and nothing
else is stuck.

## In the tests

`tests/test_faulty.py` runs each method in a `gst-launch-1.0 --no-fault`
subprocess, like the others, and checks:

- `test_stall_triggers_watchdog`: a 2 s stall against `watchdog timeout=300`
  exits 1 with `Watchdog triggered`, and the stall's log line.
- `test_stall_recovers`: a 100 ms stall against a 2 s watchdog exits 0 with
  `Got EOS`.
- `test_spin_burns_cpu`: a 2 s spin uses at least 75% of its wall time in CPU
  time, measured with `RUSAGE_CHILDREN`, which a sleeping stall would not.
- `test_deadlock_blocks_pipeline`: the pipeline is still running after 3 s
  (the subprocess times out) and logged `Deadlocking the streaming thread.`.
- `test_deadlock_blocks_property_reads`: `tests/faulty_deadlock.py` reads a
  property from the main thread of a deadlocked pipeline, and `faulthandler`
  ends it after 3 s with the main thread's innermost Python frame in
  `read_property`.

None of them needs a debugger; gdb is in the Docker image for reading the
traces above.
