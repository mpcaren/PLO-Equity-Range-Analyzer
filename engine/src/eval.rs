//! 5-card evaluator and the Omaha combination machinery.
//!
//! Cactus-Kev-style keys: each card is a 32-bit key
//!   bit 16+rank : rank bit   (OR of 5 keys >> 16 = rank mask)
//!   bit 12+suit : suit bit   (AND of 5 keys & 0xF000 != 0 = flush)
//!   bits 0..5   : rank prime (product identifies the rank multiset)
//! Flushes and 5-distinct-rank hands are looked up by the 13-bit rank mask;
//! paired hands by the prime product through a small open-addressing hash.
//! All tables are generated once, on first use. Values are bit-identical to
//! the C engine's (src/plo5.c), so results compare exactly.

use std::sync::OnceLock;

pub const CAT_HIGH: u32 = 0;
pub const CAT_PAIR: u32 = 1;
pub const CAT_TWOPAIR: u32 = 2;
pub const CAT_TRIPS: u32 = 3;
pub const CAT_STRAIGHT: u32 = 4;
pub const CAT_FLUSH: u32 = 5;
pub const CAT_FULL: u32 = 6;
pub const CAT_QUADS: u32 = 7;
pub const CAT_STFLUSH: u32 = 8;
/// number of made-hand categories
pub const NCAT: usize = 9;

const HASH_SIZE: usize = 16384; // 4888 keys -> ~30% load
const HASH_MASK: usize = HASH_SIZE - 1;
const RANK_PRIME: [u32; 13] = [2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41];

pub struct Tables {
    // fixed-size arrays + masked indices: no bounds checks in the hot loop
    flush: Box<[u32; 8192]>,
    uniq5: Box<[u32; 8192]>,
    hkeys: Box<[u32; HASH_SIZE]>,
    hvals: Box<[u32; HASH_SIZE]>,
    pub card_key: [u32; 52],
    pub binom: [[u32; 6]; 53],
    /// all distinct 5-card hand values, ascending (for ordinal lookups)
    pub vals: Vec<u32>,
}

static TABLES: OnceLock<Tables> = OnceLock::new();

/// the shared lookup tables (built on first call)
#[inline]
pub fn tables() -> &'static Tables {
    TABLES.get_or_init(Tables::build)
}

impl Tables {
    fn build() -> Tables {
        let mut binom = [[0u32; 6]; 53];
        for n in 0..53 {
            binom[n][0] = 1;
            for k in 1..6 {
                binom[n][k] = if n == 0 { 0 } else { binom[n - 1][k - 1] + binom[n - 1][k] };
            }
        }

        let mut card_key = [0u32; 52];
        for r in 0..13 {
            for s in 0..4 {
                card_key[r * 4 + s] = (1u32 << (16 + r)) | (1u32 << (12 + s)) | RANK_PRIME[r];
            }
        }

        let mut flush: Box<[u32; 8192]> = vec![0u32; 8192].into_boxed_slice().try_into().unwrap();
        let mut uniq5: Box<[u32; 8192]> = vec![0u32; 8192].into_boxed_slice().try_into().unwrap();
        for a in 0..13 {
            for b in a + 1..13 {
                for c in b + 1..13 {
                    for d in c + 1..13 {
                        for e in d + 1..13 {
                            let m = (1u32 << a) | (1 << b) | (1 << c) | (1 << d) | (1 << e);
                            let hi: i32 = if m == 0x100F {
                                3 // wheel: A2345, 5-high
                            } else if e - a == 4 {
                                e
                            } else {
                                -1
                            };
                            let m = m as usize;
                            if hi >= 0 {
                                uniq5[m] = (CAT_STRAIGHT << 24) | hi as u32;
                                flush[m] = (CAT_STFLUSH << 24) | hi as u32;
                            } else {
                                uniq5[m] = (CAT_HIGH << 24) | m as u32;
                                flush[m] = (CAT_FLUSH << 24) | m as u32;
                            }
                        }
                    }
                }
            }
        }

        let mut hkeys: Box<[u32; HASH_SIZE]> = vec![0u32; HASH_SIZE].into_boxed_slice().try_into().unwrap();
        let mut hvals: Box<[u32; HASH_SIZE]> = vec![0u32; HASH_SIZE].into_boxed_slice().try_into().unwrap();
        let mut put = |key: u32, val: u32| {
            let mut i = (key.wrapping_mul(0x9E37_79B1) >> 18) as usize;
            while hkeys[i] != 0 {
                i = (i + 1) & HASH_MASK;
            }
            hkeys[i] = key;
            hvals[i] = val;
        };
        for a in 0..13usize {
            for b in a..13 {
                for c in b..13 {
                    for d in c..13 {
                        for e in d..13 {
                            let mut cnt = [0u32; 13];
                            for x in [a, b, c, d, e] {
                                cnt[x] += 1;
                            }
                            let maxc = *cnt.iter().max().unwrap();
                            if maxc > 4 || maxc == 1 {
                                continue; // impossible, or handled above
                            }
                            let (mut quad, mut trip, mut phi, mut plo) = (-1i32, -1i32, -1i32, -1i32);
                            let mut kick = 0u32; // mask of singleton ranks
                            for r in (0..13).rev() {
                                match cnt[r] {
                                    4 => quad = r as i32,
                                    3 => trip = r as i32,
                                    2 => {
                                        if phi < 0 { phi = r as i32 } else { plo = r as i32 }
                                    }
                                    1 => kick |= 1 << r,
                                    _ => {}
                                }
                            }
                            let low_kick = || kick.trailing_zeros();
                            let val = if quad >= 0 {
                                (CAT_QUADS << 24) | ((quad as u32) << 4) | low_kick()
                            } else if trip >= 0 && phi >= 0 {
                                (CAT_FULL << 24) | ((trip as u32) << 4) | phi as u32
                            } else if trip >= 0 {
                                (CAT_TRIPS << 24) | ((trip as u32) << 13) | kick
                            } else if plo >= 0 {
                                (CAT_TWOPAIR << 24) | ((phi as u32) << 8) | ((plo as u32) << 4) | low_kick()
                            } else {
                                (CAT_PAIR << 24) | ((phi as u32) << 13) | kick
                            };
                            let key = RANK_PRIME[a] * RANK_PRIME[b] * RANK_PRIME[c] * RANK_PRIME[d] * RANK_PRIME[e];
                            put(key, val);
                        }
                    }
                }
            }
        }

        let mut vals: Vec<u32> = Vec::with_capacity(7680);
        for i in 0..8192 {
            if uniq5[i] != 0 {
                vals.push(uniq5[i]);
            }
            if flush[i] != 0 {
                vals.push(flush[i]);
            }
        }
        for i in 0..HASH_SIZE {
            if hkeys[i] != 0 {
                vals.push(hvals[i]);
            }
        }
        vals.sort_unstable();
        vals.dedup();

        Tables { flush, uniq5, hkeys, hvals, card_key, binom, vals }
    }

    #[inline(always)]
    fn hash_get(&self, key: u32) -> u32 {
        let mut i = (key.wrapping_mul(0x9E37_79B1) >> 18) as usize & HASH_MASK;
        while self.hkeys[i] != key {
            i = (i + 1) & HASH_MASK;
        }
        self.hvals[i]
    }

    /// value of a 5-card poker hand; higher = stronger, category = value >> 24
    pub fn eval5(&self, c: [usize; 5]) -> u32 {
        let k = c.map(|x| self.card_key[x]);
        let orv = k[0] | k[1] | k[2] | k[3] | k[4];
        let andv = k[0] & k[1] & k[2] & k[3] & k[4];
        let q = (orv >> 16) as usize & 8191;
        if andv & 0xF000 != 0 {
            return self.flush[q];
        }
        let u = self.uniq5[q];
        if u != 0 {
            return u;
        }
        self.hash_get((k[0] & 0xFF) * (k[1] & 0xFF) * (k[2] & 0xFF) * (k[3] & 0xFF) * (k[4] & 0xFF))
    }

    /// best of the 100 (2 from hand, 3 from board) five-card hands
    #[inline(always)]
    pub fn best_val(&self, hp: &[Combo; 10], bt: &[Combo; 10]) -> u32 {
        let mut best = 0u32;
        for b in bt {
            for h in hp {
                let q = ((h.o | b.o) >> 16) as usize & 8191;
                let v = if h.a & b.a & 0xF000 != 0 {
                    self.flush[q]
                } else {
                    let u = self.uniq5[q];
                    if u != 0 { u } else { self.hash_get(h.p.wrapping_mul(b.p)) }
                };
                if v > best {
                    best = v;
                }
            }
        }
        best
    }

    /// ordinal of a hand value among all distinct values (lower bound)
    #[inline]
    pub fn val_ord(&self, v: u32) -> u16 {
        self.vals.partition_point(|&x| x < v) as u16
    }

    /// colex index of a strictly ascending 5-card set
    #[inline]
    pub fn colex5(&self, c: [usize; 5]) -> u32 {
        self.binom[c[0]][1] + self.binom[c[1]][2] + self.binom[c[2]][3] + self.binom[c[3]][4] + self.binom[c[4]][5]
    }

    #[inline(always)]
    pub fn pairs(&self, cards: &[u8; 5]) -> [Combo; 10] {
        let mut k = [0u32; 5];
        for i in 0..5 {
            k[i] = self.card_key[cards[i] as usize];
        }
        make_pairs(&k)
    }
}

/// AND, OR and prime product of a 2- or 3-card subset
#[derive(Clone, Copy, Default, Debug)]
pub struct Combo {
    pub a: u32,
    pub o: u32,
    pub p: u32,
}

const PAIR_IX: [[usize; 2]; 10] = [[0, 1], [0, 2], [0, 3], [0, 4], [1, 2], [1, 3], [1, 4], [2, 3], [2, 4], [3, 4]];
const TRIP_IX: [[usize; 3]; 10] = [
    [0, 1, 2], [0, 1, 3], [0, 1, 4], [0, 2, 3], [0, 2, 4],
    [0, 3, 4], [1, 2, 3], [1, 2, 4], [1, 3, 4], [2, 3, 4],
];

// plain loops: `array::map` generated noticeably slower code here, and this
// runs once per holding per runout when building board rankings
#[inline(always)]
pub fn make_pairs(k: &[u32; 5]) -> [Combo; 10] {
    let mut out = [Combo::default(); 10];
    for (o, &[i, j]) in out.iter_mut().zip(PAIR_IX.iter()) {
        let (x, y) = (k[i], k[j]);
        *o = Combo { a: x & y, o: x | y, p: (x & 0xFF) * (y & 0xFF) };
    }
    out
}

#[inline(always)]
pub fn make_trips(k: &[u32; 5]) -> [Combo; 10] {
    let mut out = [Combo::default(); 10];
    for (o, &[i, j, l]) in out.iter_mut().zip(TRIP_IX.iter()) {
        let (x, y, z) = (k[i], k[j], k[l]);
        *o = Combo { a: x & y & z, o: x | y | z, p: (x & 0xFF) * (y & 0xFF) * (z & 0xFF) };
    }
    out
}

/// sort up to 5 small ints ascending (insertion sort, like the C helper)
#[inline]
pub fn sort_small<T: Copy + PartialOrd>(c: &mut [T]) {
    for i in 1..c.len() {
        let v = c[i];
        let mut j = i;
        while j > 0 && c[j - 1] > v {
            c[j] = c[j - 1];
            j -= 1;
        }
        c[j] = v;
    }
}

pub fn choose64(n: i64, k: i64) -> u64 {
    if k < 0 || k > n {
        return 0;
    }
    let mut r: u64 = 1;
    for i in 1..=k {
        r = r * (n - k + i) as u64 / i as u64;
    }
    r
}
