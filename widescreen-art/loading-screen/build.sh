#!/bin/bash
# Build the two start-up pictures of Patch 1.09 (Gandalf riding up to Minas Tirith) as real 16:9 pictures:
#   ../custom/load_w_ea-16x9.png                 the first loading screen (with the legal text)
#   ../custom/titlescreenuserinterface-16x9.png  the second one (ornament band with the socket of the spinning ring)
# Each is extended from 4:3 with the real film frame it was made from, then enhanced with Real-ESRGAN.
#
#   build.sh <game folder> <RealESRGAN_x4plus.pth> <film frame> [more frames of the same shot ...]
#
# You supply (nothing of this is in the repository):
#   - one or more stills of that shot from The Return of the King, at least 1920 px wide (your own Blu-ray capture);
#   - RealESRGAN_x4plus.pth from the Real-ESRGAN project's releases (github.com/xinntao/Real-ESRGAN).
# Afterwards run ../make-art.py again for each picture shape; it packs the custom pictures into the art archives.
# The numbers below and in blend.py (legal text rows, the band's rows and pattern period, where the rider is) belong
# to these two pictures.
set -e
here="$(cd "$(dirname "$0")" && pwd)"; game="${1:?game folder}"; weights="${2:?RealESRGAN_x4plus.pth}"; shift 2
[ $# -ge 1 ] || { echo "give at least one film frame"; exit 2; }
work="$(mktemp -d -t bfme2-loadscreen)"; cv="uv run -q --python 3.12 --with opencv-python-headless --with numpy python"
up="uv run -q --python 3.12 --with torch --with spandrel --with pillow --with numpy python"
one() {   # one <texture name> <blend.py options ...>
  local name="$1"; shift; local w="$work/$name"; mkdir -p "$w"
  uv run -q --with pillow python "$here/../make-art.py" --extract "art\\compiledtextures\\${name:0:2}\\$name.jpg" "$game" "$w/src.jpg"
  $cv "$here/film.py" "$w/src.jpg" "$w" "${frames[@]}"
  $cv "$here/blend.py" "$w/src.jpg" "$w" "$w/blend.png" "$@"
  $up "$here/upscale.py" "$weights" "$w/blend.png" "$w/x4.png"
  uv run -q --with pillow python -c 'import sys; from PIL import Image; Image.open(sys.argv[1]).resize((2736, 1536), Image.LANCZOS).save(sys.argv[2])' "$w/x4.png" "$here/../custom/$name-16x9.png"
  echo "wrote custom/$name-16x9.png"
}
frames=("$@"); mkdir -p "$here/../custom"
one load_w_ea --text-y 630
one titlescreenuserinterface --text-y 0 --band 645 709 42
echo "intermediate pictures in $work"
