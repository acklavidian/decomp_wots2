#pragma once
#include <functional>
#include <string>

// Finding the disc's files before the game starts (native, not decompiled):
// a directory that already holds them, or a disc image — bare or inside an
// archive — that is unpacked first.  See "The launcher" in the project notes.

namespace launcher {

// Progress of an extraction: what is being done and how far along (0…1).
// Answering false stops it.
using Progress = std::function<bool(const char* what, double fraction)>;

// Unpacks the game's files from `source` (an .iso, or a .zip / .7z / .rar /
// .tar.* holding either the image or the files themselves) into `dest`.
// On failure → false with `error` set, and `dest` is left as it was.
bool extract(const std::string& source, const std::string& dest, const Progress& progress,
             std::string* error);

// True if `dir` holds what the game needs to start.
bool has_game(const std::string& dir);

// Works out where the disc's files are from the command line — unpacking
// them, and asking for the image in a window if nothing was given — and
// answers the directory.  Does not come back if there is nothing to run.
std::string locate(int argc, char* argv[]);

}  // namespace launcher
