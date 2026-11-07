//! The engine on its own thread. Jobs run one at a time, in order; their
//! progress and replies are posted back to the UI thread. Cancelling bumps
//! a generation counter and raises the engine's cancel flag: the running
//! job stops at its next check and anything queued before the cancel is
//! answered `Cancelled` without running.

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{mpsc, Arc};

use plo5::api::{self, Presets};
use plo5::Engine;
use serde_json::Value;

/// the rank table is built with these, like the C tools (same file)
pub const RANK_TRIALS: u32 = 25_000;
pub const RANK_SEED: u64 = 20_260_704;

pub enum Reply {
    Ok(Value),
    /// the engine answered with {"error": ..}
    Failed(String),
    Cancelled,
}

pub type Progress = Box<dyn Fn(&str, u32, u32) + Send>;
pub type Done = Box<dyn FnOnce(Reply) + Send>;

enum Task {
    Api { endpoint: String, query: String },
    GenRanks,
    LoadRanks(PathBuf),
}

struct Job {
    task: Task,
    epoch: u64,
    progress: Option<Progress>,
    done: Done,
}

pub struct EngineHost {
    tx: mpsc::Sender<Job>,
    epoch: Arc<AtomicU64>,
    cancel: Arc<AtomicBool>,
    pub threads: usize,
    pub ranks_loaded_at_start: bool,
}

impl EngineHost {
    pub fn start(threads: usize, presets: Presets, rank_path: PathBuf) -> EngineHost {
        let (tx, rx) = mpsc::channel::<Job>();
        let mut engine = Engine::new(threads);
        let ranks_loaded_at_start = engine.ranks_load(&rank_path).is_ok();
        let threads = engine.threads;
        let cancel = engine.cancel_flag();
        let epoch = Arc::new(AtomicU64::new(0));
        let (ep, cf) = (epoch.clone(), cancel.clone());
        std::thread::Builder::new()
            .name("plo5-engine".into())
            .spawn(move || {
                for job in rx {
                    // reset first, then check the generation: a cancel landing
                    // in between is caught by one or the other
                    cf.store(false, Ordering::SeqCst);
                    if job.epoch != ep.load(Ordering::SeqCst) {
                        (job.done)(Reply::Cancelled);
                        continue;
                    }
                    let progress = job.progress;
                    let mut report = |what: &str, d: u32, t: u32| {
                        if let Some(p) = &progress {
                            p(what, d, t);
                        }
                    };
                    let reply = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                        run(&mut engine, &presets, &rank_path, job.task, &mut report)
                    }))
                    .unwrap_or_else(|_| Reply::Failed("internal engine error".into()));
                    (job.done)(reply);
                }
            })
            .expect("spawn engine thread");
        EngineHost { tx, epoch, cancel, threads, ranks_loaded_at_start }
    }

    fn submit(&self, task: Task, progress: Option<Progress>, done: Done) {
        let epoch = self.epoch.load(Ordering::SeqCst);
        let _ = self.tx.send(Job { task, epoch, progress, done });
    }

    /// any /api/... endpoint (see engine/src/api.rs); `done` runs on the
    /// engine thread — hop to the UI with slint::invoke_from_event_loop
    pub fn call(&self, endpoint: &str, query: String, progress: Option<Progress>, done: Done) {
        self.submit(Task::Api { endpoint: endpoint.into(), query }, progress, done);
    }

    pub fn gen_ranks(&self, progress: Progress, done: Done) {
        self.submit(Task::GenRanks, Some(progress), done);
    }

    pub fn load_ranks(&self, path: PathBuf, done: Done) {
        self.submit(Task::LoadRanks(path), None, done);
    }

    /// stop the running job and drop everything queued
    pub fn cancel_all(&self) {
        self.epoch.fetch_add(1, Ordering::SeqCst);
        self.cancel.store(true, Ordering::SeqCst);
    }
}

fn run(e: &mut Engine, presets: &Presets, rank_path: &Path, task: Task, report: &mut dyn FnMut(&str, u32, u32)) -> Reply {
    match task {
        Task::Api { endpoint, query } => match api::call(e, presets, &endpoint, &query, report) {
            Ok((_, body)) => {
                let v: Value = serde_json::from_str(&body).unwrap_or(Value::Null);
                match v.get("error").and_then(Value::as_str) {
                    Some(err) => Reply::Failed(err.to_string()),
                    None => Reply::Ok(v),
                }
            }
            Err(plo5::Error::Cancelled) => Reply::Cancelled,
            Err(err) => Reply::Failed(err.message().into()),
        },
        Task::LoadRanks(p) => match e.ranks_load(&p) {
            Ok(()) => Reply::Ok(Value::Bool(true)),
            Err(_) => Reply::Failed("that file is not a valid PLO5 rank table".into()),
        },
        Task::GenRanks => {
            let t0 = std::time::Instant::now();
            match e.ranks_generate(RANK_TRIALS, RANK_SEED, &mut |d, t| report("preflop rank table", d, t)) {
                Err(plo5::Error::Cancelled) => return Reply::Cancelled,
                Err(err) => return Reply::Failed(err.message().into()),
                Ok(()) => {}
            }
            if let Some(dir) = rank_path.parent() {
                let _ = std::fs::create_dir_all(dir);
            }
            // temp file + rename: a crash never leaves a truncated table
            let part = rank_path.with_extension("bin.part");
            let saved = e.ranks_save(&part, RANK_TRIALS).is_ok() && std::fs::rename(&part, rank_path).is_ok();
            if !saved {
                let _ = std::fs::remove_file(&part);
            }
            Reply::Ok(serde_json::json!({ "saved": saved, "seconds": t0.elapsed().as_secs_f64() }))
        }
    }
}
