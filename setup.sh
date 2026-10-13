#!/bin/bash
# Run this once to install required system packages and configure the build.
# Requires sudo for apt packages.
set -e

echo "=== Installing system dependencies ==="
sudo apt-get install -y \
    libsdl2-dev \
    radare2 \
    ninja-build \
    binutils-mipsel-linux-gnu \
    libgl-dev \
    ffmpeg

echo ""
echo "=== Installing Python analysis tools ==="
pip install capstone pyelftools

echo ""
echo "=== Configuring build ==="
mkdir -p build
cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Debug
cd ..

echo ""
echo "=== Setup complete! ==="
echo ""
echo "Next steps:"
echo "  1. Install Ghidra from https://ghidra-sre.org"
echo "     then install the EE R5900 processor module:"
echo "     https://github.com/beardypig/ghidra-ps2-ee  (or search 'ghidra ps2 ee')"
echo ""
echo "  2. Load extracted/SLUS_208.93 into Ghidra:"
echo "     - New Project → Import File → SLUS_208.93"
echo "     - Language: MIPS:LE:32:R5900 (after PS2 plugin install)"
echo "     - Run auto-analysis"
echo ""
echo "  3. Build the native skeleton:"
echo "     cd build && ninja"
echo ""
echo "  4. Configure PCSX2 with the ISO as a reference:"
echo "     flatpak run net.pcsx2.PCSX2"
