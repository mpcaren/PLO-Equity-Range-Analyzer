//! Stack-off thresholds across SPRs (port of src/spr.c).

use crate::{Engine, Error, Player, Result, MAX_PLAYERS};

pub fn stack(spr: f64, pot: f64) -> f64 {
    spr * pot
}

pub fn pot_final(spr: f64, pot: f64, nway: usize) -> f64 {
    pot + nway as f64 * stack(spr, pot)
}

/// minimum defence frequency vs a pot-sized shove at this SPR
pub fn mdf(spr: f64) -> f64 {
    1.0 / (1.0 + spr)
}

/// equity needed to stack off with no fold equity: SPR / (1 + nway*SPR)
pub fn eq_needed(spr: f64, nway: usize) -> f64 {
    spr / (1.0 + nway as f64 * spr)
}

/// fold frequency needed when hero_eq < eq_needed (clamped to [0,1])
pub fn breakeven_fold(eq_needed: f64, hero_eq: f64) -> f64 {
    if hero_eq >= eq_needed || hero_eq >= 1.0 {
        return 0.0;
    }
    ((eq_needed - hero_eq) / (1.0 - hero_eq)).clamp(0.0, 1.0)
}

/// keep the top `mdf` fraction of the band lo..hi -> (trim_lo, trim_hi)
pub fn mdf_trim_band(lo: f64, hi: f64, mdf: f64) -> (f64, f64) {
    let lo = lo.max(0.0);
    let hi = hi.min(100.0).max(lo);
    let mdf = mdf.clamp(0.0, 1.0);
    (hi - mdf * (hi - lo), hi)
}

#[derive(Clone, Debug, Default)]
pub struct SprRow {
    pub spr: f64,
    pub stack: f64,
    pub pot_final: f64,
    pub mdf: f64,
    pub trim_lo: f64,
    pub trim_hi: f64,
    pub eq_needed: f64,
    pub hero_eq: f64,
    pub ci95: f64,
    pub profitable_no_fold: bool,
    pub breakeven_fold: f64,
    pub band_widened: bool,
}

impl Engine {
    /// hero vs `nopp` villains each on the trimmed band, at one SPR
    #[allow(clippy::too_many_arguments)]
    pub fn spr_row_eval(
        &mut self,
        hero: [usize; 5],
        board: &[usize],
        trim_lo: f64,
        trim_hi: f64,
        nopp: usize,
        pot: f64,
        spr: f64,
        trials: u64,
        seed: u64,
    ) -> Result<SprRow> {
        if pot <= 0.0 || spr < 0.0 {
            return Err(Error::Arg);
        }
        if !(1..MAX_PLAYERS).contains(&nopp) {
            return Err(Error::Players);
        }
        let nway = nopp + 1;
        let mut players = vec![Player::Fixed(hero)];
        players.extend((0..nopp).map(|_| Player::Range { lo: trim_lo, hi: trim_hi, chain: vec![] }));
        let r = self.equity(&players, board, None, &[], trials, 0, seed)?;
        let need = eq_needed(spr, nway);
        Ok(SprRow {
            spr,
            stack: stack(spr, pot),
            pot_final: pot_final(spr, pot, nway),
            mdf: mdf(spr),
            trim_lo,
            trim_hi,
            eq_needed: need,
            hero_eq: r.equity[0],
            ci95: r.ci95[0],
            profitable_no_fold: r.equity[0] >= need,
            breakeven_fold: breakeven_fold(need, r.equity[0]),
            band_widened: false,
        })
    }
}
