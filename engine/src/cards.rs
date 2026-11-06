//! Card text helpers. Card id = rank*4 + suit (rank 0='2'..12='A';
//! suit 0=c 1=d 2=h 3=s).

const RANK_CH: &[u8; 13] = b"23456789TJQKA";
const SUIT_CH: &[u8; 4] = b"cdhs";

/// "Ah" -> card id (case-insensitive), or None
pub fn parse_card(s: &str) -> Option<usize> {
    let b = s.as_bytes();
    if b.len() < 2 {
        return None;
    }
    let r = RANK_CH.iter().position(|&c| c == b[0].to_ascii_uppercase())?;
    let u = SUIT_CH.iter().position(|&c| c == b[1].to_ascii_lowercase())?;
    Some(r * 4 + u)
}

/// card id -> "Ah"
pub fn card_str(id: usize) -> String {
    if id > 51 {
        return "??".into();
    }
    let mut s = String::with_capacity(2);
    s.push(RANK_CH[id >> 2] as char);
    s.push(SUIT_CH[id & 3] as char);
    s
}

/// "AhKd 2c,3c" -> ids; whitespace and commas ignored; None if malformed
/// or longer than `max`
pub fn parse_cards(s: &str, max: usize) -> Option<Vec<usize>> {
    let mut out = Vec::new();
    let mut buf = String::new();
    for ch in s.chars() {
        if ch.is_whitespace() || ch == ',' {
            continue;
        }
        buf.push(ch);
        if buf.chars().count() == 2 {
            if out.len() >= max {
                return None;
            }
            out.push(parse_card(&buf)?);
            buf.clear();
        }
    }
    if buf.is_empty() { Some(out) } else { None }
}

pub fn cards_str(ids: &[usize]) -> String {
    ids.iter().map(|&c| card_str(c)).collect()
}

/// cards as text, strongest first (for display)
pub fn hand_str_sorted(ids: &[usize]) -> String {
    let mut v = ids.to_vec();
    v.sort_unstable_by(|a, b| b.cmp(a));
    cards_str(&v)
}

pub const CATEGORY_NAMES: [&str; 9] = [
    "High Card", "Pair", "Two Pair", "Trips", "Straight", "Flush", "Full House", "Quads", "Straight Flush",
];
