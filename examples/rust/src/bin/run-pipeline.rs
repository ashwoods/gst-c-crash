//! Run a pipeline the way an application should: check every state change,
//! watch the bus, retry runtime errors, notice a stall and stop with a deadline.
//!
//!     run-pipeline [--retries N] [--stall-timeout S] PIPELINE
//!
//! Exit status: 0 at EOS, 1 on an error, 2 for a pipeline that cannot be built,
//! 3 when the pipeline is stuck and cannot be stopped.
use std::io::Write;
use std::process::ExitCode;
use std::sync::mpsc;
use std::thread;
use std::time::Duration;

use gst::prelude::*;
use gstreamer as gst;

const STOP_TIMEOUT: Duration = Duration::from_secs(2);

fn say(line: impl std::fmt::Display) {
    println!("{line}");
    std::io::stdout().flush().unwrap();
}

/// The path of the element that posted the message.
fn source(msg: &gst::Message) -> String {
    msg.src()
        .map(|s| s.path_string().to_string())
        .unwrap_or_default()
}

// --8<-- [start:stop]
/// Set the pipeline to NULL, giving up after STOP_TIMEOUT.
///
/// A blocked streaming thread holds its pad's stream lock, and the state
/// change waits for that lock forever, so never wait for it unbounded.
fn stop(pipeline: &gst::Element) -> bool {
    let (sender, receiver) = mpsc::channel();
    let stopping = pipeline.clone();
    thread::spawn(move || {
        let _ = sender.send(stopping.set_state(gst::State::Null));
    });
    match receiver.recv_timeout(STOP_TIMEOUT) {
        Err(_) => {
            say(format!(
                "error: the pipeline did not stop within {} s",
                STOP_TIMEOUT.as_secs()
            ));
            // the stuck thread cannot be freed: leave it to the supervisor
            std::process::exit(3);
        }
        Ok(Err(_)) => {
            say("error: could not stop the pipeline");
            false
        }
        Ok(Ok(_)) => true,
    }
}
// --8<-- [end:stop]

#[derive(PartialEq)]
enum Outcome {
    Eos,
    Error,
    Stall,
}

// --8<-- [start:bus]
/// Play until EOS, an error or a stall.
fn play(pipeline: &gst::Element, stall_timeout: u32) -> Outcome {
    let bus = pipeline.bus().unwrap();
    let mut last_position = None;
    let mut idle = 0;
    loop {
        let msg = bus.timed_pop_filtered(
            gst::ClockTime::SECOND,
            &[
                gst::MessageType::Eos,
                gst::MessageType::Error,
                gst::MessageType::Warning,
            ],
        );
        let Some(msg) = msg else {
            // no message for a second: is data still flowing?
            let position = pipeline.query_position::<gst::ClockTime>();
            idle = if position == last_position {
                idle + 1
            } else {
                0
            };
            last_position = position;
            if idle >= stall_timeout {
                say(format!("stalled: no data for {stall_timeout} s"));
                return Outcome::Stall;
            }
            continue;
        };
        match msg.view() {
            gst::MessageView::Eos(_) => {
                say("eos");
                return Outcome::Eos;
            }
            gst::MessageView::Warning(warning) => {
                say(format!(
                    "warning: from {}: {}",
                    source(&msg),
                    warning.error().message()
                ));
            }
            gst::MessageView::Error(error) => {
                // the first error is the cause; the ones after it are its echo
                say(format!(
                    "error: from {}: {}",
                    source(&msg),
                    error.error().message()
                ));
                say(format!(
                    "debug: {}",
                    error.debug().as_deref().unwrap_or("None")
                ));
                return Outcome::Error;
            }
            _ => unreachable!(),
        }
    }
}
// --8<-- [end:bus]

// --8<-- [start:retry]
fn run(pipeline: &gst::Element, retries: u32, stall_timeout: u32) -> ExitCode {
    for attempt in 0..=retries {
        if attempt > 0 {
            say(format!("retrying ({attempt}/{retries})"));
        }
        if pipeline.set_state(gst::State::Playing).is_err() {
            // an element refused to start: retrying will not help. It may
            // have posted why on the bus, or not
            say("error: could not start the pipeline");
            let bus = pipeline.bus().unwrap();
            if let Some(msg) = bus.pop_filtered(&[gst::MessageType::Error]) {
                if let gst::MessageView::Error(error) = msg.view() {
                    say(format!(
                        "error: from {}: {}",
                        source(&msg),
                        error.error().message()
                    ));
                }
            }
            stop(pipeline);
            return ExitCode::from(1);
        }
        let outcome = play(pipeline, stall_timeout);
        if !stop(pipeline) {
            return ExitCode::from(1);
        }
        if outcome == Outcome::Eos {
            return ExitCode::SUCCESS;
        }
    }
    ExitCode::from(1)
}
// --8<-- [end:retry]

fn usage() -> ExitCode {
    eprintln!("usage: run-pipeline [--retries N] [--stall-timeout S] PIPELINE");
    ExitCode::from(2)
}

fn main() -> ExitCode {
    let mut retries = 0;
    let mut stall_timeout = 5;
    let mut pipeline = None;
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        let number = |value: Option<String>| value.and_then(|v| v.parse().ok());
        match arg.as_str() {
            "--retries" => match number(args.next()) {
                Some(n) => retries = n,
                None => return usage(),
            },
            "--stall-timeout" => match number(args.next()) {
                Some(n) => stall_timeout = n,
                None => return usage(),
            },
            _ if pipeline.is_none() => pipeline = Some(arg),
            _ => return usage(),
        }
    }
    let Some(description) = pipeline else {
        return usage();
    };

    gst::init().unwrap();
    // --8<-- [start:build]
    let pipeline = match gst::parse::launch(&description) {
        Ok(pipeline) => pipeline,
        Err(err) => {
            say(format!(
                "error: could not build the pipeline: {}",
                err.message()
            ));
            return ExitCode::from(2);
        }
    };
    // --8<-- [end:build]

    run(&pipeline, retries, stall_timeout)
}
