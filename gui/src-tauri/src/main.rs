// No console window behind the GUI on Windows release builds.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    minidrive_gui_lib::run()
}
