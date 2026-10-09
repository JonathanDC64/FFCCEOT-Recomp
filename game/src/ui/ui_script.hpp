// Testing: a script that drives the UI (--ui-script). It sends the same events a player would (keys, gamepad buttons,
// the mouse, text, a chosen file), waits for things to appear, changes settings and saves screenshots, so that the
// prelaunch screen, the menu and the settings windows can be exercised and looked at without anyone at the keyboard.
//
// One step per word, run one per frame, in the order written (blank lines and # comments are ignored; a word with
// spaces in it, such as a file name, is put in double quotes):
//   wait:N                  N frames pass
//   sleep:MS                MS milliseconds pass (the game may run many frames in them)
//   key:Down                a key goes down, then up (SDL key names: Return, Escape, F1, 1, ...; key:Shift+F1 holds a modifier)
//   keydown:X / keyup:X     the key goes down / up alone
//   pad:south               a gamepad button goes down, then up (south east west north start back guide dpup dpdown
//                           dpleft dpright lshoulder rshoulder lstick rstick); padhold:X / padup:X for one half
//   axis:righttrigger:32000 a gamepad axis moves (leftx lefty rightx righty lefttrigger righttrigger; -32768 to 32767)
//   hover:SELECTOR          the mouse moves onto the first element matching the RCSS selector
//   click:SELECTOR          the mouse moves onto it and clicks
//                           (SELECTOR@0.8 is a point 80% along the element's width, to land on a slider)
//   close                   the window's X button is pressed
//   mousedown / mouseup     the left button goes down / up where the mouse is (hover first, then hover elsewhere to drag)
//   wheel:N                 the mouse wheel turns N notches (negative: the other way)
//   text:abc                text is typed
//   choose:PATH             answers the file dialog with this file (the real dialog is not shown while scripted)
//   drop:PATH               a file is dropped on the window
//   set:key=value          changes a setting (a settings.ini key), as --set-at-frame does
//   resize:WxH              resizes the window
//   timeout:SECONDS         the longest a wait may take from here on (default 30; exit code 3 when it runs out)
//   until:CONDITION         waits for: game (a game runs), idle (no ROM check or build), frame=N (N game frames),
//                           sel=SELECTOR (an element is shown), nosel=SELECTOR (no such element is shown)
//   shot:name               saves the window as <shots folder>/name.png a few frames later
//   quit                    ends the program
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace ui::script {

struct Hooks {
    std::function<bool()> game_running;
    std::function<std::uint64_t()> frames;                   // game frames so far
    std::function<bool(const std::string&)> set;             // "key=value"
    std::function<void(int, int)> resize;
    std::function<void(const std::string&)> shot;            // the next presented frame is saved here
    std::function<void()> quit;
    std::filesystem::path shot_dir;
};

bool load(const std::filesystem::path& path, std::string& error);
bool active() noexcept;
// Runs the next step. The program calls it once per presented frame, before it handles the frame's events.
void tick(const Hooks& hooks) noexcept;
// A wait ran out of time (the program then exits with code 3).
bool timed_out() noexcept;

}  // namespace ui::script
