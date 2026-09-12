#!/bin/bash
set -u

EDID="/lib/firmware/240hz.bin"
V4L2_CTL="/usr/bin/v4l2-ctl"

if [ ! -x "$V4L2_CTL" ]; then
  echo "v4l2-ctl not found: $V4L2_CTL" >&2
  exit 2
fi

if [ ! -f "$EDID" ]; then
  echo "EDID file not found: $EDID" >&2
  exit 3
fi

find_hdmirx_node() {
  for dev in /dev/video*; do
    [ -e "$dev" ] || continue
    info="$("$V4L2_CTL" -d "$dev" --all 2>/dev/null || true)"
    name="$(printf '%s\n' "$info" | sed -n "s/.*Card type *: *//p" | head -n 1)"
    bus="$(printf '%s\n' "$info" | sed -n "s/.*Bus info *: *//p" | head -n 1)"

    if printf '%s\n%s\n%s\n' "$dev" "$name" "$bus" | grep -Eiq "hdmirx|hdmi.*rx|rk.*hdmi"; then
      printf '%s\n' "$dev"
      return 0
    fi
  done
  return 1
}

for i in $(seq 1 20); do
  HDMIRX_NODE="$(find_hdmirx_node || true)"
  if [ -n "${HDMIRX_NODE:-}" ]; then
    "$V4L2_CTL" -d "$HDMIRX_NODE" --clear-edid 2>/dev/null || true
    sleep 1

    if "$V4L2_CTL" -d "$HDMIRX_NODE" --set-edid="file=$EDID,format=raw"; then
      sleep 1
      "$V4L2_CTL" -d "$HDMIRX_NODE" --get-edid=file=/tmp/current_edid.bin,format=raw 2>/dev/null || true
      logger -t mouseai-edid "applied $EDID to $HDMIRX_NODE"
      echo "applied $EDID to $HDMIRX_NODE"
      exit 0
    fi
  fi
  sleep 1
done

echo "HDMI RX video node not found or EDID apply failed" >&2
logger -t mouseai-edid "failed: HDMI RX node not found or EDID apply failed"
exit 1
