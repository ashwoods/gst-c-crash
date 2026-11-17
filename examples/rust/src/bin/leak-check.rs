//! Play a pipeline twice and report the GStreamer objects the second run
//! left alive, with the leaks tracer.
//!
//!     GST_TRACERS=leaks leak-check PIPELINE
//!
//! Exit status: 0 when nothing leaked, 1 when something did, 2 when it cannot
//! check: no leaks tracer, or a pipeline that cannot be built or started.
use std::collections::BTreeMap;
use std::io::Write;
use std::process::ExitCode;

use gst::prelude::*;
use gstreamer as gst;

fn say(line: impl std::fmt::Display) {
    println!("{line}");
    std::io::stdout().flush().unwrap();
}

/// Play the pipeline to EOS or an error. Return false if it cannot start.
fn play(pipeline: &gst::Element) -> bool {
    if pipeline.set_state(gst::State::Playing).is_err() {
        say("error: could not start the pipeline");
        let _ = pipeline.set_state(gst::State::Null);
        return false;
    }
    pipeline.bus().unwrap().timed_pop_filtered(
        gst::ClockTime::NONE,
        &[gst::MessageType::Eos, gst::MessageType::Error],
    );
    let _ = pipeline.set_state(gst::State::Null);
    true
}

/// The type names of the structures in a list field of the checkpoint.
fn type_names(checkpoint: &gst::Structure, field: &str) -> Vec<String> {
    checkpoint
        .get::<gst::List>(field)
        .unwrap()
        .iter()
        .map(|value| {
            let structure = value.get::<gst::Structure>().unwrap();
            structure.get::<String>("type-name").unwrap()
        })
        .collect()
}

fn main() -> ExitCode {
    let Some(description) = std::env::args().nth(1) else {
        eprintln!("usage: GST_TRACERS=leaks leak-check PIPELINE");
        return ExitCode::from(2);
    };
    gst::init().unwrap();
    // --8<-- [start:tracer]
    // the tracer exists only when GST_TRACERS names it before gst::init()
    let leaks = gst::active_tracers()
        .into_iter()
        .find(|t| t.type_().name() == "GstLeaksTracer");
    let Some(leaks) = leaks else {
        say("error: no leaks tracer: run with GST_TRACERS=leaks");
        return ExitCode::from(2);
    };
    // --8<-- [end:tracer]
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

    // --8<-- [start:checkpoint]
    // the first run creates the objects GStreamer keeps for good (the
    // system clock, caps and pad templates): leave them out
    if !play(&pipeline) {
        return ExitCode::from(2);
    }
    leaks.emit_by_name::<()>("activity-start-tracking", &[]);
    play(&pipeline);
    let checkpoint = leaks.emit_by_name::<gst::Structure>("activity-get-checkpoint", &[]);
    leaks.emit_by_name::<()>("activity-stop-tracking", &[]);

    // alive = created since tracking started, minus removed since
    let mut alive: BTreeMap<String, i64> = BTreeMap::new();
    for name in type_names(&checkpoint, "objects-created-list") {
        *alive.entry(name).or_default() += 1;
    }
    for name in type_names(&checkpoint, "objects-removed-list") {
        *alive.entry(name).or_default() -= 1;
    }
    let leaked: Vec<_> = alive.iter().filter(|(_, n)| **n > 0).collect();
    for (name, n) in &leaked {
        say(format!("leaked: {n} {name}"));
    }
    // --8<-- [end:checkpoint]
    if leaked.is_empty() {
        say("no leaks");
    }
    ExitCode::from(!leaked.is_empty() as u8)
}
