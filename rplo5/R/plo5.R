# rplo5 — R interface to the PLO5 equity engine.
#
# Cards are written like "Ah", hands like "AhAdKhKd7s" (case-insensitive,
# spaces allowed). Percentiles: 0 = weakest, 100 = strongest, and are
# conditional on the board when one is given.

.RANKS <- "23456789TJQKA"
.SUITS <- "cdhs"

.card_id <- function(tok) {
  r <- regexpr(toupper(substr(tok, 1, 1)), .RANKS, fixed = TRUE) - 1L
  s <- regexpr(tolower(substr(tok, 2, 2)), .SUITS, fixed = TRUE) - 1L
  if (r < 0 || s < 0) stop("bad card: ", tok)
  r * 4L + s
}

.parse_cards <- function(s) {
  if (is.null(s) || !nzchar(s)) return(integer(0))
  s <- gsub("[ ,]", "", s)
  if (nchar(s) %% 2 != 0) stop("bad card string: ", s)
  vapply(seq_len(nchar(s) / 2),
         function(i) .card_id(substr(s, 2 * i - 1, 2 * i)), integer(1))
}

.card_str <- function(id) {
  paste0(substr(.RANKS, id %/% 4 + 1, id %/% 4 + 1),
         substr(.SUITS, id %% 4 + 1, id %% 4 + 1))
}

.parse_player <- function(spec) {
  spec <- trimws(spec)
  if (tolower(spec) %in% c("random", "*"))
    return(list(type = 1L, cards = integer(5), lo = 0, hi = 100))
  if (grepl("^[0-9.]+-[0-9.]+%?$", spec)) {
    v <- as.numeric(strsplit(sub("%$", "", spec), "-")[[1]])
    return(list(type = 2L, cards = integer(5), lo = v[1], hi = v[2]))
  }
  cds <- .parse_cards(spec)
  if (length(cds) != 5) stop("hand needs 5 cards: ", spec)
  list(type = 0L, cards = as.integer(cds), lo = 0, hi = 100)
}

#' PLO5 equity for 2-6 players.
#' hands: character vector, each "AhAdKhKd7s", "random", or "10-40"
#'        (a percentile band — preflop table if no board, else the
#'        board-conditional distribution, built automatically)
#' board2/double_board: double-board split pot — two boards, each pays
#'        half the pot; best on both scoops. Setting board2 implies
#'        double_board = TRUE.
#' Returns a data.frame with equity/win/tie/ci95 per player
#' (in double-board mode, win = clean scoops).
plo5_equity <- function(hands, board = "", dead = "",
                        board2 = "", double_board = nzchar(board2),
                        trials = 1e6, max_enum = 1e6,
                        seed = 42, threads = parallel::detectCores()) {
  specs <- lapply(hands, .parse_player)
  types <- as.integer(vapply(specs, `[[`, integer(1), "type"))
  cards <- as.integer(unlist(lapply(specs, `[[`, "cards")))
  lows  <- as.numeric(vapply(specs, `[[`, numeric(1), "lo"))
  highs <- as.numeric(vapply(specs, `[[`, numeric(1), "hi"))
  b <- as.integer(.parse_cards(board))
  b2 <- as.integer(.parse_cards(board2))
  d <- as.integer(.parse_cards(dead))
  r <- .Call(C_plo5_equity, types, cards, lows, highs, b, b2,
             isTRUE(double_board), d,
             as.numeric(trials), as.numeric(max_enum),
             as.numeric(seed), as.integer(threads))
  out <- data.frame(hand = hands, equity = r$equity, win = r$win,
                    tie = r$tie, ci95 = r$ci95, stringsAsFactors = FALSE)
  attr(out, "samples") <- r$samples
  attr(out, "exact") <- r$exact
  out
}

#' Load the preflop rank table (plo5rank.bin).
plo5_load_ranks <- function(path = "plo5rank.bin") {
  .Call(C_plo5_ranks_load, path.expand(path))
}

#' Build the preflop rank table (takes a few minutes).
plo5_gen_ranks <- function(path = "plo5rank.bin", trials = 25000,
                           threads = parallel::detectCores(), seed = 20260704) {
  .Call(C_plo5_ranks_generate, path.expand(path), as.numeric(trials),
        as.integer(threads), as.numeric(seed))
}

#' Build the board-conditional ranking for a flop/turn/river.
#' Give board2 as well for the combined double-board distribution.
plo5_rank_board <- function(board, board2 = "", runouts = 100,
                            threads = parallel::detectCores()) {
  b <- as.integer(.parse_cards(board))
  b2 <- as.integer(.parse_cards(board2))
  .Call(C_plo5_board_ranks_build, b, b2, as.integer(runouts),
        as.integer(threads))
}

#' Percentile (0-100) of a hand, optionally conditional on a board
#' (plo5_rank_board must have been called for that board first,
#' or use board = "" for preflop). Give board and board2 for the
#' combined double-board distribution.
plo5_hand_percentile <- function(hand, board = "", board2 = "") {
  h <- as.integer(.parse_cards(hand))
  if (length(h) != 5) stop("hand needs 5 cards")
  .Call(C_plo5_hand_percentile, h, as.integer(.parse_cards(board)),
        as.integer(.parse_cards(board2)))
}

#' Representative hand at a percentile, as a string like "AsAd9h5c2c".
plo5_percentile_hand <- function(pct, board = "", board2 = "") {
  ids <- .Call(C_plo5_percentile_hand, as.numeric(pct),
               as.integer(.parse_cards(board)),
               as.integer(.parse_cards(board2)))
  paste(vapply(sort(ids, decreasing = TRUE), .card_str, character(1)),
        collapse = "")
}
