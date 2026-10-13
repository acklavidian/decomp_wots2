Way of the Samurai 2, native — Linux (x86-64)

You need your own copy of the game: the North American disc, SLUS-208.93.
Nothing from the disc is in this package.

1. Install the two libraries it uses (Debian / Ubuntu / Pop!_OS / Mint):

       sudo apt install libsdl2-2.0-0 libarchive13

   (Fedora: sudo dnf install SDL2 libarchive.  Arch: sudo pacman -S sdl2 libarchive.)
   ffmpeg is optional: with it the movies play.

2. From this folder, hand it your disc image the first time:

       ./wots2 "Way of the Samurai 2.iso"

   An .iso works, and so does a .zip / .7z / .rar that holds it.  The game's
   files are unpacked once into the folder "extracted" (about 1.5 GB).
   Run ./wots2 without anything and it asks for the file in a window.

3. After that:

       ./wots2

F11 toggles fullscreen.  Saves go in the folder "wots2_card".
The three mods in "mods" (widescreen, resolution, fps unlock) are on; each has
a mod.ini you can edit.  See mods/README.md.

Source and issues: https://github.com/acklavidian/decomp_wots2
