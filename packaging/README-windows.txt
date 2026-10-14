Way of the Samurai 2, native — Windows (64-bit)

You need your own copy of the game: the North American disc, SLUS-208.93.
Nothing from the disc is in this package.

1. Unzip this folder somewhere you can write to (not Program Files).

2. Double-click wots2.exe.  The first time it asks for your copy of the disc:
   an .iso, or a .zip / .7z / .rar that holds it.  (You can also drop the
   file onto wots2.exe.)  The game's files are unpacked once into the folder
   "extracted" beside the program (about 1.5 GB), and the game starts.

3. After that, wots2.exe starts the game straight away.

Movies (the logos, the opening, the staff roll) need ffmpeg: put ffmpeg.exe
in this folder (https://www.gyan.dev/ffmpeg/builds/ or
https://github.com/BtbN/FFmpeg-Builds/releases), or have it on your PATH.
Without it they are skipped.

F11 toggles fullscreen.  Saves go in the folder "wots2_card".  What the
program prints goes to wots2.log in this folder — attach it to bug reports.
The four mods in "mods" (widescreen, resolution, fps unlock, modern lighting)
are on; each has a mod.ini you can edit.  See mods\README.md.

This build has been tested under Wine, not yet on a real Windows machine.
Reports of how it runs are welcome.

Source and issues: https://github.com/acklavidian/decomp_wots2
