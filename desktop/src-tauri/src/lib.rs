//! PLO5 Equity Lab — Tauri backend.
//!
//! The Rust engine (`plo5` crate) runs in-process on one dedicated thread
//! that owns all its caches; the page talks to it through a few commands:
//!
//! - `engine_call(endpoint, params, onProgress)` — any engine endpoint
//!   (same parameters and JSON as the HTTP API of plo5web), with progress
//!   streamed over a channel for slow steps
//! - `engine_cancel()` — stop the running job and drop queued ones
//! - `engine_info()`, `ranks_import()`, `app_reveal(which)`
//!
//! Jobs run one at a time, in order. Cancelling bumps a generation counter
//! and raises the engine's cancel flag: the running job stops at its next
//! check and anything queued before the cancel is answered `cancelled`.

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{mpsc, Arc, Mutex};

use plo5::api::{self, Presets};
use plo5::Engine;
use serde_json::{json, Map, Value};
use tauri::ipc::Channel;
use tauri::{AppHandle, Manager, State, WebviewUrl, WebviewWindowBuilder};
use tauri_plugin_dialog::DialogExt;
use tauri_plugin_opener::OpenerExt;

/// the rank table is built with these, like the C tools (same file)
const RANK_TRIALS: u32 = 25_000;
const RANK_SEED: u64 = 20_260_704;

const ENDPOINTS: &[&str] = &[
    "/api/equity", "/api/lookup", "/api/rankof", "/api/findeq", "/api/random",
    "/api/sprquiz", "/api/status", "/svc/info", "/svc/genranks",
];

enum Reply {
    Ok { status: u16, body: String },
    Cancelled,
    Error(String),
}

enum Task {
    Api { endpoint: String, query: String },
    LoadRanks(PathBuf),
}

struct Job {
    task: Task,
    epoch: u64,
    progress: Option<Channel<Value>>,
    reply: mpsc::Sender<Reply>,
}

struct Paths {
    ranks: PathBuf,
    config: Option<PathBuf>,
    user_data: PathBuf,
}

struct EngineHost {
    tx: Mutex<mpsc::Sender<Job>>,
    epoch: Arc<AtomicU64>,
    cancel: Arc<AtomicBool>,
    paths: Paths,
    threads: usize,
}

/* ---------------- config + paths ---------------- */

/// plo5config.ini (written by plo5setup): threads + trial presets
fn read_config(path: Option<&Path>) -> (usize, Presets) {
    let mut threads = 0usize;
    let mut presets = Presets::default();
    if let Some(text) = path.and_then(|p| std::fs::read_to_string(p).ok()) {
        for line in text.lines() {
            let line = line.trim();
            if line.starts_with('#') || line.starts_with(';') {
                continue;
            }
            let Some((k, v)) = line.split_once('=') else { continue };
            let v = v.trim();
            let num = || v.parse::<f64>().ok().filter(|x| *x > 0.0).map(|x| x as u64);
            match k.trim() {
                "threads" => threads = v.parse().unwrap_or(0),
                "trials_fast" => presets.0[0] = num().unwrap_or(presets.0[0]),
                "trials_balanced" => presets.0[1] = num().unwrap_or(presets.0[1]),
                "trials_precise" => presets.0[2] = num().unwrap_or(presets.0[2]),
                _ => {}
            }
        }
    }
    (threads, presets)
}

fn resolve_paths(app: &AppHandle) -> Paths {
    let user_data = app.path().app_data_dir().unwrap_or_else(|_| std::env::temp_dir().join("plo5-equity-lab"));
    let _ = std::fs::create_dir_all(&user_data);
    let exe_dir = std::env::current_exe().ok().and_then(|p| p.parent().map(Path::to_path_buf));
    // the repository root, where the C tools keep their copies (dev builds)
    let repo = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("..").join("..");
    let dev = cfg!(debug_assertions);

    let mut ranks: Vec<PathBuf> = std::env::var_os("PLO5_RANKS").map(PathBuf::from).into_iter().collect();
    if dev {
        ranks.push(repo.join("plo5rank.bin"));
    }
    ranks.push(user_data.join("plo5rank.bin"));
    if let Some(d) = &exe_dir {
        ranks.push(d.join("plo5rank.bin"));
    }
    if !dev {
        ranks.push(repo.join("plo5rank.bin"));
    }
    let rank = ranks.iter().find(|p| p.is_file()).cloned().unwrap_or_else(|| user_data.join("plo5rank.bin"));

    let mut cfgs: Vec<PathBuf> = std::env::var_os("PLO5_CONFIG").map(PathBuf::from).into_iter().collect();
    if dev {
        cfgs.push(repo.join("plo5config.ini"));
    }
    cfgs.push(user_data.join("plo5config.ini"));
    if let Some(d) = &exe_dir {
        cfgs.push(d.join("plo5config.ini"));
    }
    let config = cfgs.into_iter().find(|p| p.is_file());

    let clean = |p: PathBuf| std::fs::canonicalize(&p).unwrap_or(p);
    Paths { ranks: clean(rank), config: config.map(clean), user_data }
}

/* ---------------- the engine thread ---------------- */

fn spawn_engine(threads: usize, presets: Presets, rank_path: PathBuf, epoch: Arc<AtomicU64>, cancel_out: &mut Option<Arc<AtomicBool>>) -> mpsc::Sender<Job> {
    let (tx, rx) = mpsc::channel::<Job>();
    let mut engine = Engine::new(threads);
    let _ = engine.ranks_load(&rank_path); // missing is fine: built on request
    let cancel = engine.cancel_flag();
    *cancel_out = Some(cancel.clone());

    std::thread::Builder::new()
        .name("plo5-engine".into())
        .spawn(move || {
            for job in rx {
                // reset first, then check the generation: a cancel landing in
                // between is caught by one or the other
                cancel.store(false, Ordering::SeqCst);
                if job.epoch != epoch.load(Ordering::SeqCst) {
                    let _ = job.reply.send(Reply::Cancelled);
                    continue;
                }
                let progress = job.progress;
                let mut report = |what: &str, done: u32, total: u32| {
                    if let Some(ch) = &progress {
                        let _ = ch.send(json!({ "what": what, "done": done, "total": total }));
                    }
                };
                let res = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| match &job.task {
                    Task::LoadRanks(p) => match engine.ranks_load(p) {
                        Ok(()) => Reply::Ok { status: 200, body: json!({ "ranks": true }).to_string() },
                        Err(e) => Reply::Error(e.message().into()),
                    },
                    Task::Api { endpoint, query } => match endpoint.as_str() {
                        "/svc/info" => Reply::Ok {
                            status: 200,
                            body: json!({ "ranks": engine.ranks_loaded(), "ncpu": engine.threads }).to_string(),
                        },
                        "/svc/genranks" => gen_ranks(&mut engine, &rank_path, &mut report),
                        _ => match api::call(&mut engine, &presets, endpoint, query, &mut report) {
                            Ok((status, body)) => Reply::Ok { status, body },
                            Err(plo5::Error::Cancelled) => Reply::Cancelled,
                            Err(e) => Reply::Error(e.message().into()),
                        },
                    },
                }));
                let reply = res.unwrap_or_else(|_| Reply::Error("internal engine error".into()));
                let _ = job.reply.send(reply);
            }
        })
        .expect("spawn engine thread");
    tx
}

/// build the preflop table, save it next to the old one atomically
fn gen_ranks(engine: &mut Engine, path: &Path, report: &mut dyn FnMut(&str, u32, u32)) -> Reply {
    let t0 = std::time::Instant::now();
    match engine.ranks_generate(RANK_TRIALS, RANK_SEED, &mut |d, t| report("preflop rank table", d, t)) {
        Err(plo5::Error::Cancelled) => return Reply::Cancelled,
        Err(e) => return Reply::Error(e.message().into()),
        Ok(()) => {}
    }
    if let Some(dir) = path.parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    // write to a temp file and move it into place, so a crash never leaves
    // a truncated table behind
    let part = path.with_extension("bin.part");
    let saved = engine.ranks_save(&part, RANK_TRIALS).is_ok() && std::fs::rename(&part, path).is_ok();
    if !saved {
        let _ = std::fs::remove_file(&part);
    }
    Reply::Ok {
        status: 200,
        body: json!({ "ranks": engine.ranks_loaded(), "saved": saved, "seconds": t0.elapsed().as_secs_f64() }).to_string(),
    }
}

impl EngineHost {
    fn submit(&self, task: Task, progress: Option<Channel<Value>>) -> mpsc::Receiver<Reply> {
        let (reply, rx) = mpsc::channel();
        let epoch = self.epoch.load(Ordering::SeqCst);
        let job = Job { task, epoch, progress, reply };
        let _ = self.tx.lock().unwrap().send(job);
        rx
    }
}

async fn wait(rx: mpsc::Receiver<Reply>) -> Value {
    let reply = tauri::async_runtime::spawn_blocking(move || rx.recv())
        .await
        .ok()
        .and_then(Result::ok)
        .unwrap_or_else(|| Reply::Error("engine stopped".into()));
    match reply {
        Reply::Ok { status, body } => {
            let body: Value = serde_json::from_str(&body).unwrap_or(Value::Null);
            json!({ "status": status, "body": body })
        }
        Reply::Cancelled => json!({ "cancelled": true }),
        Reply::Error(e) => json!({ "error": e }),
    }
}

/// params -> url-encoded query (keys are restricted, values percent-encoded)
fn encode_params(params: &Map<String, Value>) -> String {
    let enc = |s: &str| {
        s.bytes()
            .map(|b| match b {
                b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'.' | b'_' | b'~' => (b as char).to_string(),
                _ => format!("%{b:02X}"),
            })
            .collect::<String>()
    };
    params
        .iter()
        .filter(|(k, _)| !k.is_empty() && k.len() <= 24 && k.bytes().all(|b| b.is_ascii_alphanumeric()))
        .filter_map(|(k, v)| {
            let v = match v {
                Value::String(s) => s.chars().take(2000).collect(),
                Value::Number(n) => n.to_string(),
                Value::Bool(b) => (if *b { "1" } else { "0" }).to_string(),
                _ => return None,
            };
            Some(format!("{k}={}", enc(&v)))
        })
        .collect::<Vec<_>>()
        .join("&")
}

/* ---------------- commands ---------------- */

#[tauri::command]
async fn engine_call(
    host: State<'_, EngineHost>,
    endpoint: String,
    params: Option<Map<String, Value>>,
    progress: bool,
    on_progress: Channel<Value>,
) -> Result<Value, ()> {
    if !ENDPOINTS.contains(&endpoint.as_str()) {
        return Ok(json!({ "error": "unknown endpoint" }));
    }
    let query = encode_params(&params.unwrap_or_default());
    let rx = host.submit(Task::Api { endpoint, query }, progress.then_some(on_progress));
    Ok(wait(rx).await)
}

#[tauri::command]
fn engine_cancel(host: State<'_, EngineHost>) -> bool {
    host.epoch.fetch_add(1, Ordering::SeqCst);
    host.cancel.store(true, Ordering::SeqCst);
    true
}

#[tauri::command]
async fn engine_info(app: AppHandle, host: State<'_, EngineHost>) -> Result<Value, ()> {
    let ready = wait(host.submit(Task::Api { endpoint: "/svc/info".into(), query: String::new() }, None)).await;
    Ok(json!({
        "found": true,
        "engine": format!("Rust, built in ({} threads)", host.threads),
        "ranks": host.paths.ranks,
        "cfg": host.paths.config,
        "userData": host.paths.user_data,
        "platform": std::env::consts::OS,
        "version": app.package_info().version.to_string(),
        "ready": ready.get("body").cloned(),
        "error": ready.get("error").cloned(),
    }))
}

/// pick an existing plo5rank.bin (e.g. built by plo5setup), copy it to the
/// app's data folder and load it
#[tauri::command]
async fn ranks_import(app: AppHandle, host: State<'_, EngineHost>) -> Result<Value, ()> {
    let picked = app
        .dialog()
        .file()
        .set_title("Locate plo5rank.bin")
        .add_filter("PLO5 rank table", &["bin"])
        .blocking_pick_file();
    let Some(src) = picked.and_then(|f| f.into_path().ok()) else {
        return Ok(json!({ "canceled": true }));
    };
    let dest = host.paths.user_data.join("plo5rank.bin");
    if std::fs::canonicalize(&src).ok() != std::fs::canonicalize(&dest).ok() {
        if let Err(e) = std::fs::copy(&src, &dest) {
            return Ok(json!({ "error": e.to_string() }));
        }
    }
    let r = wait(host.submit(Task::LoadRanks(dest), None)).await;
    if r.get("error").is_some() {
        return Ok(json!({ "error": "that file is not a valid PLO5 rank table" }));
    }
    Ok(json!({ "ok": true }))
}

#[tauri::command]
fn app_reveal(app: AppHandle, host: State<'_, EngineHost>, which: String) {
    let p = match which.as_str() {
        "ranks" => host.paths.ranks.clone(),
        "config" => match &host.paths.config {
            Some(c) => c.clone(),
            None => return,
        },
        _ => return,
    };
    if p.exists() {
        let _ = app.opener().reveal_item_in_dir(&p);
    } else if let Some(dir) = p.parent() {
        let _ = app.opener().open_path(dir.to_string_lossy(), None::<&str>);
    }
}

/* ---------------- app ---------------- */

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_opener::init())
        .setup(|app| {
            let handle = app.handle().clone();
            let paths = resolve_paths(&handle);
            let (threads, presets) = read_config(paths.config.as_deref());
            let epoch = Arc::new(AtomicU64::new(0));
            let mut cancel = None;
            let threads = if threads == 0 { plo5::available_threads() } else { threads };
            let tx = spawn_engine(threads, presets, paths.ranks.clone(), epoch.clone(), &mut cancel);
            app.manage(EngineHost { tx: Mutex::new(tx), epoch, cancel: cancel.unwrap(), paths, threads });

            // roomy by default: most of the screen, capped so it does not sprawl
            let (mut w, mut h) = (1400.0, 900.0);
            if let Ok(Some(m)) = app.primary_monitor() {
                let s = m.size().to_logical::<f64>(m.scale_factor());
                w = (s.width * 0.92).clamp(1080.0, 1520.0);
                h = (s.height * 0.92).clamp(700.0, 1060.0);
            }
            let builder = WebviewWindowBuilder::new(app, "main", WebviewUrl::default())
                .title("PLO5 Equity Lab")
                .inner_size(w, h)
                .min_inner_size(1080.0, 700.0)
                .center()
                .background_color(tauri::window::Color(9, 13, 11, 255));
            // custom title bar: native traffic lights on macOS, our own
            // controls (see TitleBar in App.jsx) elsewhere
            #[cfg(target_os = "macos")]
            let builder = builder.title_bar_style(tauri::TitleBarStyle::Overlay).hidden_title(true);
            #[cfg(not(target_os = "macos"))]
            let builder = builder.decorations(false);
            builder.build()?;
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![engine_call, engine_cancel, engine_info, ranks_import, app_reveal])
        .run(tauri::generate_context!())
        .expect("error while running PLO5 Equity Lab");
}
