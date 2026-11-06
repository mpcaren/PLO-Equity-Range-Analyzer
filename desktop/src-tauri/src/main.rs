// keep a console window from opening alongside the app on Windows release builds
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    plo5_equity_lab_lib::run()
}
