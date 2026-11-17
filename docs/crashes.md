# Crashes

A crash in a GStreamer application almost always happens in a streaming
thread, inside an element's C code, while the application's own code waits
somewhere else. Nothing in the process can recover from it; what matters is
that the stack ends up somewhere you can read it, and that something restarts
the work. faulty has four methods that crash the streaming thread once
`num-buffers` buffers have passed:

| `method`            | What it does                       | Exit status |
|---------------------|------------------------------------|-------------|
| `sigsegv` (default) | `raise(SIGSEGV)`                   | 139         |
| `sigbus`            | `raise(SIGBUS)`                    | 138         |
| `sigabrt`           | `raise(SIGABRT)`, like `abort()`, a failed `g_assert()` or glibc's heap checks | 134 |
| `custom`            | writes to a null pointer           | 139         |

`custom` is the most realistic: a real invalid access, with a faulting
address (`0x0`) that crash reporters show and group by. The `raise` methods
send the signal by hand, so their stacks end in `raise` and `pthread_kill`.
`custom` is also the one to edit when you want faulty to crash the way your
own bug does.

## Without a tool

On macOS `gst-launch-1.0` dies at once:

```
Setting pipeline to PLAYING ...
Segmentation fault: 11
```

On Linux it catches the signal and waits for a debugger to attach, which
keeps the process alive and the exit status away from whoever is watching:

```
Caught SIGSEGV
Spinning.  Please run 'gdb gst-launch-1.0 105' to continue debugging, Ctrl-C to quit, or Ctrl-\ to dump core.
```

Pass `--no-fault` to let it die. Only `gst-launch-1.0` does this; your
application dies like the macOS one, with no message at all.

## Get the stack

### Under a debugger

Run the pipeline under lldb or gdb and it stops at the crash, in the
streaming thread, named after the pad whose task runs it:

```sh
lldb -o run -- gst-launch-1.0 videotestsrc num-buffers=50 \
    ! faulty num-buffers=5 method=custom ! fakesink           # macOS
gdb -ex run --args gst-launch-1.0 --no-fault videotestsrc num-buffers=50 \
    ! faulty num-buffers=5 method=custom ! fakesink           # Linux
```

```
* thread #8, name = 'videotestsrc0:src', stop reason = EXC_BAD_ACCESS (code=1, address=0x0)
    frame #0: 0x0000000108e57a54 libgstcrash.dylib`faulty_custom_method(buf=0x00000075692803c0) at gstfaulty.c:115:23
   112 	{
   113 	  /* a write to a null pointer; volatile so the compiler cannot delete it.
   114 	   * Mainly meant as a placeholder for adaptation */
-> 115 	  *(volatile int *) 0 = 0;
```

`bt` prints the rest of the stack, and `bt all` (`thread apply all bt` in
gdb) every thread's.

### After the fact: macOS crash reports

macOS writes a report for every crashed process to
`~/Library/Logs/DiagnosticReports/`, a few seconds after the crash; Console.app
lists them under Crash Reports. The report is JSON (`.ips`) and holds every
thread's stack, symbolicated from the plugin's debug information:

```
gst-launch-1.0-2026-11-17-101813.ips
  exception:      EXC_BAD_ACCESS (SIGSEGV), KERN_INVALID_ADDRESS at 0x0000000000000000
  crashed thread: 7, videotestsrc0:src
    0 libgstcrash.dylib       faulty_custom_method + 12   (gstfaulty.c:115)
    1 libgstcrash.dylib       faulty_call_method + 92     (gstfaulty.c:200)
    2 libgstcrash.dylib       gst_faulty_transform_ip + 1388 (gstfaulty.c:341)
    3 libgstbase-1.0.0.dylib  default_generate_output + 456
    4 libgstbase-1.0.0.dylib  gst_base_transform_chain + 288
```

(The report's fields, abridged.)

### After the fact: Linux core dumps

Linux writes a core dump when the core size limit allows it, to wherever
`/proc/sys/kernel/core_pattern` says: a file named `core` in the working
directory, or `systemd-coredump`, whose dumps `coredumpctl gdb` opens. In
Docker (`docker compose run --rm shell`):

```sh
ulimit -c unlimited
gst-launch-1.0 --no-fault -q videotestsrc num-buffers=50 ! faulty num-buffers=5 method=custom ! fakesink
gdb -batch -ex bt $(which gst-launch-1.0) core
```

```
Program terminated with signal SIGSEGV, Segmentation fault.
#0  faulty_custom_method (buf=0xc86b7484b6c0) at ../../src/gst/gstfaulty.c:115
115	  *(volatile int *) 0 = 0;
[Current thread is 1 (Thread 0xec48d5e4f160 (LWP 120))]
#0  faulty_custom_method (buf=0xc86b7484b6c0) at ../../src/gst/gstfaulty.c:115
#1  0x0000ec48d5ee2d74 in faulty_call_method (method=FAULTY_CUSTOM_METHOD, buf=0xc86b7484b6c0) at ../../src/gst/gstfaulty.c:200
#2  0x0000ec48d5ee35e4 in gst_faulty_transform_ip (base=0xc86b74838200 [GstBaseTransform|faulty0], buf=0xc86b7484b6c0) at ../../src/gst/gstfaulty.c:341
#3  0x0000ec48d601975c in  () at /lib/aarch64-linux-gnu/libgstbase-1.0.so.0
#4  0x0000ec48d601902c in  () at /lib/aarch64-linux-gnu/libgstbase-1.0.so.0
#5  0x0000ec48d697045c in  () at /lib/aarch64-linux-gnu/libgstreamer-1.0.so.0
```

The plugin's frames have names and lines because it is a debug build;
Debian's GStreamer frames do not, because its debug information ships
separately. Install the `-dbgsym` packages, or let gdb fetch them with
`DEBUGINFOD_URLS=https://debuginfod.debian.net`. The `test` service in
`docker-compose.yml` turns core dumps off (`ulimits: core: 0`), because every
crashing test would leave one in the repository.

## In the application

### Run the pipeline in a child process

A crash takes down the whole process, so the robust design keeps the
pipeline in a process of its own and supervises it from another: the parent
sees how the child died, reports it and starts a new one. `examples/` has
this as `supervise`, which runs itself as the child:

=== "Python"

    ```python
    --8<-- "examples/python/supervise.py:supervise"
    ```

=== "C"

    ```c
    --8<-- "examples/c/supervise.c:supervise"
    ```

=== "Rust"

    ```rust
    --8<-- "examples/rust/src/bin/supervise.rs:supervise"
    ```

Run it against a crashing pipeline, with one restart:

=== "Python"

    ```sh
    python3 examples/python/supervise.py --restarts 1 \
        "videotestsrc num-buffers=50 ! faulty num-buffers=5 method=sigsegv ! fakesink"
    ```

=== "C"

    ```sh
    build/examples/c/supervise --restarts 1 \
        "videotestsrc num-buffers=50 ! faulty num-buffers=5 method=sigsegv ! fakesink"
    ```

=== "Rust"

    ```sh
    cargo run --manifest-path examples/rust/Cargo.toml --bin supervise -- --restarts 1 \
        "videotestsrc num-buffers=50 ! faulty num-buffers=5 method=sigsegv ! fakesink"
    ```

```
crashed: SIGSEGV
restarting (1/1)
crashed: SIGSEGV
```

The same split serves a pipeline that hangs (see
[Handling errors](errors.md#stop-with-a-deadline)): the child exits when it
cannot stop, and the parent starts it again. systemd's `Restart=on-failure`
or a container runtime's restart policy is the same supervisor, one level up.

### Say something before dying

The child can still print what it knows as it dies, from a signal handler.
A handler runs on the crashed thread, in the middle of whatever it was doing,
possibly with the allocator's lock held, so it may only call
async-signal-safe functions: `write()`, `raise()`, but not `printf()` or
`malloc()`. Then it re-raises the signal, so the process still dies of it and
the parent, the core dump and the crash report still see it:

=== "Python"

    ```python
    --8<-- "examples/python/supervise.py:child"
    ```

    Python's `faulthandler` prints each thread's Python stack, and since
    Python 3.14 the crashed thread's C stack too:

    ```
    Fatal Python error: Segmentation fault

    Current thread's C stack trace (most recent call first):
      …
      Binary file "/usr/lib/system/libsystem_c.dylib", at raise+0x20 [0x18b57bbbc]
      Binary file ".../build/gst/libgstcrash.dylib", at faulty_signal_sigsegv+0x18 [0x106a779f4]
      Binary file ".../build/gst/libgstcrash.dylib", at faulty_call_method+0x5c [0x106a77970]
      Binary file ".../build/gst/libgstcrash.dylib", at gst_faulty_transform_ip+0x56c [0x106a776e4]
      Binary file ".../libgstbase-1.0.0.dylib", at default_generate_output+0x1c8 [0x10773c39c]
    ```

=== "C"

    ```c
    --8<-- "examples/c/supervise.c:child"
    ```

=== "Rust"

    ```rust
    --8<-- "examples/rust/src/bin/supervise.rs:child"
    ```

    Rust's panic machinery never sees a crash in C code: a SIGSEGV in an
    element is not a panic, and `catch_unwind` cannot stop it.

### Crash reporters

Crash reporters such as Sentry's native SDK, Crashpad and Breakpad do the same
with more care: a handler, or better a separate handler process, writes a
minidump of every thread's stack and registers, and the stack is
symbolicated later on a server, from debug files you upload. To test that
yours works for a GStreamer application:

- Put faulty into the application's pipeline, for example in place of an
  `identity` or right after the source: `… ! faulty num-buffers=100 method=custom ! …`.
  Also try `sigabrt`, which takes the `abort()` path.
- Check that the report names the crashed thread (`videotestsrc0:src` here),
  has `faulty_custom_method` at the top with its line, and has the
  application's other threads too.
- On Linux, run it under the application, not `gst-launch-1.0`, or pass
  `--no-fault`: `gst-launch-1.0`'s own handler catches the signal first.
  Libraries that install handlers can do the same to your reporter, so check
  that it still fires once everything is loaded.

## In the tests

`tests/test_faulty.py` checks each method's exit signal, with
`gst-launch-1.0 --no-fault` in a subprocess so the crash cannot take pytest
down. `tests/test_examples.py` checks that every language's `supervise`
reports the signal and restarts the child.
