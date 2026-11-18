#!/bin/bash
# render.sh in.svg out.png W H
google-chrome --headless=new --no-sandbox --disable-gpu --hide-scrollbars --force-device-scale-factor=1 \
  --window-size=$3,$4 --screenshot="$2" "file://$(realpath "$1")" >/dev/null 2>&1
