#!/bin/bash
# Build ../custom/load_w_ea-16x9.png: the start-up loading screen of Patch 1.09 (Gandalf riding up to Minas Tirith)
# extended from 4:3 to 16:9 with the real film frame it was made from, then enhanced with Real-ESRGAN.
#
#   build.sh <game folder> <RealESRGAN_x4plus.pth> <film frame> [more frames of the same shot ...]
#
# You supply (nothing of this is in the repository):
#   - one or more stills of that shot from The Return of the King, at least 1920 px wide (your own Blu-ray capture);
#   - RealESRGAN_x4plus.pth from the Real-ESRGAN project's releases (github.com/xinntao/Real-ESRGAN).
# Afterwards run ../make-art.py again; it packs custom/load_w_ea-16x9.png into the art archives.
# The numbers in blend.py (where the legal text and the rider are) belong to this one picture.
set -e
here="$(cd "$(dirname "$0")" && pwd)"; game="${1:?game folder}"; weights="${2:?RealESRGAN_x4plus.pth}"; shift 2
[ $# -ge 1 ] || { echo "give at least one film frame"; exit 2; }
work="$(mktemp -d -t bfme2-loadscreen)"; cv="uv run -q --python 3.12 --with opencv-python-headless --with numpy python"
uv run -q --with pillow python "$here/../make-art.py" --extract 'art\compiledtextures\lo\load_w_ea.jpg' "$game" "$work/load_w_ea.jpg"
$cv "$here/film.py" "$work/load_w_ea.jpg" "$work" "$@"
$cv "$here/blend.py" "$work/load_w_ea.jpg" "$work" "$work/blend.png"
uv run -q --python 3.12 --with torch --with spandrel --with pillow --with numpy python "$here/upscale.py" "$weights" "$work/blend.png" "$work/x4.png"
mkdir -p "$here/../custom"
uv run -q --with pillow python -c 'import sys; from PIL import Image; Image.open(sys.argv[1]).resize((2736, 1536), Image.LANCZOS).save(sys.argv[2])' "$work/x4.png" "$here/../custom/load_w_ea-16x9.png"
echo "wrote $here/../custom/load_w_ea-16x9.png (intermediate pictures in $work)"
