//! Where things live on disk, and the small JSON state file (settings and
//! the last calculator setup). Every read is forgiving: a missing or
//! malformed file just means defaults.

use std::path::{Path, PathBuf};

use plo5::api::Presets;
use serde::{Deserialize, Serialize};

use crate::calc::Saved;

#[derive(Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct Settings {
    pub precision: usize,
    pub auto: bool,
    pub build_ranks: bool,
    pub four_color: bool,
    pub reduced_motion: bool,
    pub view: usize,
    pub deck_open: bool,
}

impl Default for Settings {
    fn default() -> Self {
        Settings { precision: 1, auto: true, build_ranks: true, four_color: true, reduced_motion: false, view: 0, deck_open: true }
    }
}

#[derive(Clone, Default, Serialize, Deserialize)]
#[serde(default)]
pub struct TrainerStats {
    pub eq_n: u32,
    pub eq_sum: f64,
    pub eq_best: Option<f64>,
    pub spr_n: u32,
    pub spr_right: u32,
    pub spr_streak: u32,
    pub spr_best: u32,
}

#[derive(Default, Serialize, Deserialize)]
#[serde(default)]
pub struct State {
    pub settings: Settings,
    pub calc: Option<Saved>,
    pub stats: TrainerStats,
}

pub struct Paths {
    pub data_dir: PathBuf,
    pub state: PathBuf,
    pub ranks: PathBuf,
    pub config: Option<PathBuf>,
}

pub fn resolve() -> Paths {
    let data_dir = dirs::data_dir()
        .unwrap_or_else(std::env::temp_dir)
        .join("PLO5 Equity Lab");
    let _ = std::fs::create_dir_all(&data_dir);
    let exe_dir = std::env::current_exe().ok().and_then(|p| p.parent().map(Path::to_path_buf));
    // the repository root, where the C tools keep their copies (dev builds)
    let repo = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("..");
    let dev = cfg!(debug_assertions);

    let mut ranks: Vec<PathBuf> = std::env::var_os("PLO5_RANKS").map(PathBuf::from).into_iter().collect();
    if dev {
        ranks.push(repo.join("plo5rank.bin"));
    }
    ranks.push(data_dir.join("plo5rank.bin"));
    if let Some(d) = &exe_dir {
        ranks.push(d.join("plo5rank.bin"));
    }
    if !dev {
        ranks.push(repo.join("plo5rank.bin"));
    }
    let rank = ranks.iter().find(|p| p.is_file()).cloned().unwrap_or_else(|| data_dir.join("plo5rank.bin"));

    let mut cfgs: Vec<PathBuf> = std::env::var_os("PLO5_CONFIG").map(PathBuf::from).into_iter().collect();
    if dev {
        cfgs.push(repo.join("plo5config.ini"));
    }
    cfgs.push(data_dir.join("plo5config.ini"));
    if let Some(d) = &exe_dir {
        cfgs.push(d.join("plo5config.ini"));
    }
    let config = cfgs.into_iter().find(|p| p.is_file());

    let clean = |p: PathBuf| std::fs::canonicalize(&p).unwrap_or(p);
    Paths { state: data_dir.join("state.json"), ranks: clean(rank), config: config.map(clean), data_dir }
}

pub fn load_state(p: &Path) -> State {
    std::fs::read_to_string(p).ok().and_then(|s| serde_json::from_str(&s).ok()).unwrap_or_default()
}

pub fn save_state(p: &Path, s: &State) {
    if let Ok(text) = serde_json::to_string_pretty(s) {
        let tmp = p.with_extension("json.tmp");
        if std::fs::write(&tmp, text).is_ok() {
            let _ = std::fs::rename(&tmp, p);
        }
    }
}

/// plo5config.ini (written by plo5setup): threads + trial presets
pub fn read_config(path: Option<&Path>) -> (usize, Presets) {
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
