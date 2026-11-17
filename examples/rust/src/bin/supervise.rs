//! Run a pipeline in a child process, so a crash kills the child and not the
//! application, and restart it when it dies.
//!
//!     supervise [--restarts N] PIPELINE
//!
//! Exit status: 0 when the child reaches EOS, 1 when it fails or crashes once
//! more than --restarts allows.
use std::io::Write;
use std::os::unix::process::ExitStatusExt;
use std::process::{Command, ExitCode};

use gst::prelude::*;
use gstreamer as gst;

fn say(line: impl std::fmt::Display) {
    println!("{line}");
    std::io::stdout().flush().unwrap();
}

/// The name of a signal, as Python's signal.Signals gives it.
fn signal_name(signal: i32) -> String {
    let name = match signal {
        libc::SIGHUP => "SIGHUP",
        libc::SIGINT => "SIGINT",
        libc::SIGQUIT => "SIGQUIT",
        libc::SIGILL => "SIGILL",
        libc::SIGTRAP => "SIGTRAP",
        libc::SIGABRT => "SIGABRT",
        libc::SIGBUS => "SIGBUS",
        libc::SIGFPE => "SIGFPE",
        libc::SIGKILL => "SIGKILL",
        libc::SIGSEGV => "SIGSEGV",
        libc::SIGPIPE => "SIGPIPE",
        libc::SIGALRM => "SIGALRM",
        libc::SIGTERM => "SIGTERM",
        _ => return format!("signal {signal}"),
    };
    name.to_string()
}

// A crash in C code, such as a GStreamer element, never reaches Rust's panic
// machinery: no panic hook runs, nothing unwinds, the kernel just kills the
// process with a signal. This handler is the most the process can do for
// itself, and it must stay that small. A signal can arrive while the process
// holds the allocator's lock or is halfway through a write to stdout, so the
// handler may only call async-signal-safe functions: no allocation, no
// println!, and not std::backtrace, which allocates and takes locks. A crash
// reporter does the real work outside the crashed process: an out-of-process
// handler such as crashpad takes a minidump from a watchdog that stays
// healthy (sentry-rust's native integration does this), or the supervisor
// below picks up the core dump.
extern "C" fn on_fatal_signal(signal: libc::c_int) {
    let line: &[u8] = match signal {
        libc::SIGSEGV => b"fatal signal: SIGSEGV\n",
        libc::SIGABRT => b"fatal signal: SIGABRT\n",
        libc::SIGBUS => b"fatal signal: SIGBUS\n",
        libc::SIGFPE => b"fatal signal: SIGFPE\n",
        libc::SIGILL => b"fatal signal: SIGILL\n",
        _ => b"fatal signal\n",
    };
    unsafe {
        libc::write(libc::STDERR_FILENO, line.as_ptr().cast(), line.len());
        // restore the default action (SA_RESETHAND should have, but on macOS
        // the handler ran again and again for SIGSEGV), then raise the
        // signal again, to die of it as before
        libc::signal(signal, libc::SIG_DFL);
        libc::raise(signal);
    }
}

fn install_fatal_signal_handler() {
    for signal in [
        libc::SIGSEGV,
        libc::SIGBUS,
        libc::SIGABRT,
        libc::SIGFPE,
        libc::SIGILL,
    ] {
        unsafe {
            let mut action: libc::sigaction = std::mem::zeroed();
            action.sa_sigaction = on_fatal_signal as extern "C" fn(libc::c_int) as usize;
            action.sa_flags = libc::SA_RESETHAND;
            libc::sigemptyset(&mut action.sa_mask);
            libc::sigaction(signal, &action, std::ptr::null_mut());
        }
    }
}

// --8<-- [start:child]
/// Play the pipeline until EOS (exit 0) or an error (exit 1).
fn child(description: &str) -> ExitCode {
    // on SIGSEGV, SIGBUS, SIGABRT, SIGFPE or SIGILL, write the signal's name
    // to stderr, then die of the signal as before
    install_fatal_signal_handler();
    gst::init().unwrap();
    let pipeline = match gst::parse::launch(description) {
        Ok(pipeline) => pipeline,
        Err(err) => {
            eprintln!("error: could not build the pipeline: {}", err.message());
            return ExitCode::from(1);
        }
    };
    let _ = pipeline.set_state(gst::State::Playing);
    let msg = pipeline.bus().unwrap().timed_pop_filtered(
        gst::ClockTime::NONE,
        &[gst::MessageType::Eos, gst::MessageType::Error],
    );
    let _ = pipeline.set_state(gst::State::Null);
    match msg {
        Some(msg) if matches!(msg.view(), gst::MessageView::Eos(_)) => ExitCode::SUCCESS,
        _ => ExitCode::from(1),
    }
}
// --8<-- [end:child]

// --8<-- [start:supervise]
fn supervise(description: &str, restarts: u32) -> ExitCode {
    for attempt in 0..=restarts {
        if attempt > 0 {
            say(format!("restarting ({attempt}/{restarts})"));
        }
        let status = Command::new(std::env::current_exe().unwrap())
            .args(["--child", description])
            .status()
            .unwrap();
        if status.success() {
            say("eos");
            return ExitCode::SUCCESS;
        }
        if let Some(signal) = status.signal() {
            // killed by a signal: here is where a crash reporter would pick
            // up the core dump or minidump
            say(format!("crashed: {}", signal_name(signal)));
        } else {
            say(format!("failed: exit {}", status.code().unwrap_or(-1)));
        }
    }
    ExitCode::from(1)
}
// --8<-- [end:supervise]

fn usage() -> ExitCode {
    eprintln!("usage: supervise [--restarts N] PIPELINE");
    ExitCode::from(2)
}

fn main() -> ExitCode {
    let mut restarts = 0;
    let mut is_child = false;
    let mut pipeline = None;
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--restarts" => match args.next().and_then(|v| v.parse().ok()) {
                Some(n) => restarts = n,
                None => return usage(),
            },
            "--child" => is_child = true,
            _ if pipeline.is_none() => pipeline = Some(arg),
            _ => return usage(),
        }
    }
    let Some(description) = pipeline else {
        return usage();
    };
    if is_child {
        return child(&description);
    }
    supervise(&description, restarts)
}
