#!/usr/bin/env bash
set -euo pipefail

DEVICE="${DEVICE:-/dev/video0}"
HOST="${HOST:-}"
PORT="${PORT:-5000}"
WIDTH="${WIDTH:-1280}"
HEIGHT="${HEIGHT:-720}"
FPS="${FPS:-60}"
BITRATE="${BITRATE:-20000000}"
SRC_WIDTH="${SRC_WIDTH:-1920}"
SRC_HEIGHT="${SRC_HEIGHT:-1080}"
SRC_FPS="${SRC_FPS:-240}"
SRC_FORMAT="${SRC_FORMAT:-BGR}"
V4L2_IO_MODE="${V4L2_IO_MODE:-auto}"
V4L2_NUM_BUFFERS="${V4L2_NUM_BUFFERS:-0}"
JPEG_FORMAT="${JPEG_FORMAT:-I420}"
MPP_JPEG_FORMAT="${MPP_JPEG_FORMAT:-NV12}"
TAINAI_JPEG_ENCODER="${TAINAI_JPEG_ENCODER:-auto}"
TAINAI_CENTER_CROP="${TAINAI_CENTER_CROP:-1}"
TAINAI_HW_CROP="${TAINAI_HW_CROP:-0}"
TAINAI_UDP_CHUNK="${TAINAI_UDP_CHUNK:-0}"
CPP_SWAP_RB="${CPP_SWAP_RB:-0}"
CPP_NO_GST_CROP="${CPP_NO_GST_CROP:-0}"
V4L2_BLOCKING="${V4L2_BLOCKING:-0}"
FFMPEG_PKT_SIZE="${FFMPEG_PKT_SIZE:-1316}"
JPEG_IDCT_METHOD="${JPEG_IDCT_METHOD:-1}"

usage() {
  cat <<EOF
Usage:
  HOST=<pc_ip> sudo -E bash scripts/hdmirx_net_stream.sh h264 [width height fps bitrate]
  HOST=<pc_ip> sudo -E bash scripts/hdmirx_net_stream.sh mjpeg [width height fps quality]
  HOST=<pc_ip> PORT=<port> sudo -E bash scripts/hdmirx_net_stream.sh tainai-jpeg-udp [width height fps quality]
  HOST=<pc_ip> PORT=<port> sudo -E bash scripts/hdmirx_net_stream.sh tainai-jpeg-tcp [width height fps quality]
  HOST=<pc_ip> PORT=<port> sudo -E bash scripts/hdmirx_net_stream.sh tainai-udp [width height fps bitrate]
  HOST=<pc_ip> PORT=<port> sudo -E bash scripts/hdmirx_net_stream.sh tainai-tcp [width height fps bitrate]
  HOST=<pc_ip> PORT=<port> sudo -E bash scripts/hdmirx_net_stream.sh cpp-jpeg-udp [width height fps quality]
  HOST=<pc_ip> PORT=<port> sudo -E bash scripts/hdmirx_net_stream.sh v4l2-jpeg-udp [width height fps quality]
  HOST=<pc_ip> PORT=<port> sudo -E bash scripts/hdmirx_net_stream.sh ffmpeg-mjpeg-udp [width height fps quality]
  bash scripts/hdmirx_net_stream.sh fps-test [width height fps quality]
  bash scripts/hdmirx_net_stream.sh fps-raw-test [width height fps]
  bash scripts/hdmirx_net_stream.sh count-test raw|jpeg [width height fps quality frames timeout_seconds]
  bash scripts/hdmirx_net_stream.sh crop-info
  bash scripts/hdmirx_net_stream.sh clear-crop
  bash scripts/hdmirx_net_stream.sh diagnose
  bash scripts/hdmirx_net_stream.sh receiver-h264 [port]
  bash scripts/hdmirx_net_stream.sh receiver-mjpeg [port]

Examples:
  HOST=192.168.1.20 sudo -E bash scripts/hdmirx_net_stream.sh h264 1280 720 60 20000000
  HOST=192.168.1.20 sudo -E bash scripts/hdmirx_net_stream.sh mjpeg 1280 720 60 75
  HOST=192.168.1.20 PORT=9999 sudo -E bash scripts/hdmirx_net_stream.sh tainai-jpeg-udp 640 640 60 70
  HOST=192.168.1.20 PORT=9999 sudo -E bash scripts/hdmirx_net_stream.sh tainai-jpeg-tcp 640 640 60 70
  HOST=192.168.1.20 PORT=9999 sudo -E bash scripts/hdmirx_net_stream.sh tainai-udp 1280 720 60 20000000
  HOST=192.168.1.20 PORT=9999 sudo -E bash scripts/hdmirx_net_stream.sh tainai-tcp 1280 720 60 20000000
  HOST=192.168.1.20 PORT=9999 sudo -E bash scripts/hdmirx_net_stream.sh cpp-jpeg-udp 416 416 60 70
  HOST=192.168.1.20 PORT=9999 sudo -E bash scripts/hdmirx_net_stream.sh v4l2-jpeg-udp 416 416 60 70
  HOST=192.168.1.20 PORT=9999 sudo -E bash scripts/hdmirx_net_stream.sh ffmpeg-mjpeg-udp 416 416 240 95

Receiver:
  On the PC, install GStreamer or ffplay. receiver-* prints matching low-latency commands.
EOF
}

need_host() {
  [ -n "$HOST" ] || {
    echo "HOST is required. Example: HOST=192.168.1.20 sudo -E bash $0 h264" >&2
    exit 1
  }
}

diagnose() {
  echo "===== HDMI RX device ====="
  v4l2-ctl --list-devices 2>/dev/null || ls -l /dev/video* 2>/dev/null || true
  echo
  echo "===== HDMI RX timings ====="
  v4l2-ctl -d "$DEVICE" --query-dv-timings 2>/dev/null || true
  echo
  echo "===== HDMI RX formats ====="
  v4l2-ctl -d "$DEVICE" --list-formats-ext 2>/dev/null || true
  echo
  echo "===== HDMI RX crop/selection ====="
  v4l2-ctl -d "$DEVICE" --get-selection=target=crop 2>/dev/null || true
  v4l2-ctl -d "$DEVICE" --get-selection=target=crop_bounds 2>/dev/null || true
  v4l2-ctl -d "$DEVICE" --get-selection=target=compose 2>/dev/null || true
  echo
  echo "===== GStreamer encoders ====="
  gst-inspect-1.0 2>/dev/null | grep -Ei "mpp.*enc|v4l2.*enc|x264enc|jpegenc|rtp(h264|jpeg)|mpegtsmux" || true
  echo
  echo "===== mppjpegenc properties ====="
  gst-inspect-1.0 mppjpegenc 2>/dev/null | sed -n '/Element Properties:/,/^$/p' || true
  echo
  echo "===== Current defaults ====="
  echo "DEVICE=$DEVICE SRC_FORMAT=$SRC_FORMAT SRC_WIDTH=$SRC_WIDTH SRC_HEIGHT=$SRC_HEIGHT SRC_FPS=$SRC_FPS V4L2_IO_MODE=$V4L2_IO_MODE"
  echo "JPEG_FORMAT=$JPEG_FORMAT MPP_JPEG_FORMAT=$MPP_JPEG_FORMAT TAINAI_JPEG_ENCODER=$TAINAI_JPEG_ENCODER TAINAI_HW_CROP=$TAINAI_HW_CROP"
}

crop_info() {
  echo "===== HDMI RX crop/selection ====="
  v4l2-ctl -d "$DEVICE" --get-selection=target=crop 2>/dev/null || true
  v4l2-ctl -d "$DEVICE" --get-selection=target=crop_bounds 2>/dev/null || true
  v4l2-ctl -d "$DEVICE" --get-selection=target=compose 2>/dev/null || true
}

apply_hw_crop() {
  local width="$1"
  local height="$2"
  local left top
  left=$(((SRC_WIDTH - width) / 2))
  top=$(((SRC_HEIGHT - height) / 2))
  echo "Trying V4L2 hardware crop: left=$left top=$top width=$width height=$height" >&2
  v4l2-ctl -d "$DEVICE" --set-selection=target=crop,left="$left",top="$top",width="$width",height="$height" >/dev/null
  SRC_WIDTH="$width"
  SRC_HEIGHT="$height"
  TAINAI_CENTER_CROP=0
}

clear_crop() {
  echo "Trying to reset V4L2 crop to full frame ${SRC_WIDTH}x${SRC_HEIGHT}."
  v4l2-ctl -d "$DEVICE" --set-selection=target=crop,left=0,top=0,width="$SRC_WIDTH",height="$SRC_HEIGHT" 2>/dev/null || true
  crop_info
}

video_chain() {
  local width="$1"
  local height="$2"
  local fps="$3"
  local center_crop="${4:-0}"
  local out_format="${5:-NV12}"

  local src_props="device=$DEVICE do-timestamp=true"
  if [ "$V4L2_IO_MODE" != "auto" ] && [ -n "$V4L2_IO_MODE" ]; then
    src_props="$src_props io-mode=$V4L2_IO_MODE"
  fi
  if [ "$V4L2_NUM_BUFFERS" -gt 0 ]; then
    src_props="$src_props num-buffers=$V4L2_NUM_BUFFERS"
  fi
  printf '%s' "v4l2src $src_props ! "
  local src_format="$SRC_FORMAT"
  # v4l2-ctl reports the HDMI RX fourcc as BGR3, while GStreamer caps call it BGR.
  [ "$src_format" = "BGR3" ] && src_format="BGR"

  local caps="video/x-raw"
  if [ "$src_format" != "auto" ] && [ -n "$src_format" ]; then
    caps="$caps,format=$src_format"
  fi
  caps="$caps,width=$SRC_WIDTH,height=$SRC_HEIGHT"
  if [ "$SRC_FPS" != "auto" ] && [ -n "$SRC_FPS" ]; then
    caps="$caps,framerate=$SRC_FPS/1"
  fi
  printf '%s' "$caps ! "
  printf '%s' "queue max-size-buffers=1 max-size-time=0 max-size-bytes=0 leaky=downstream ! "
  if [ "$center_crop" = "1" ] && [ "$width" -le "$SRC_WIDTH" ] && [ "$height" -le "$SRC_HEIGHT" ]; then
    local crop_left crop_right crop_top crop_bottom
    crop_left=$(((SRC_WIDTH - width) / 2))
    crop_right=$((SRC_WIDTH - width - crop_left))
    crop_top=$(((SRC_HEIGHT - height) / 2))
    crop_bottom=$((SRC_HEIGHT - height - crop_top))
    printf '%s' "videocrop left=$crop_left right=$crop_right top=$crop_top bottom=$crop_bottom ! "
  fi
  printf '%s' "videorate drop-only=true ! video/x-raw,framerate=$fps/1 ! "
  printf '%s' "videoscale ! video/x-raw,width=$width,height=$height ! "
  printf '%s' "videoconvert n-threads=2 ! video/x-raw,format=$out_format"
}

jpeg_encoder_for_tainai() {
  local quality="$1"
  if [ "$TAINAI_JPEG_ENCODER" != "software" ] && gst-inspect-1.0 mppjpegenc >/dev/null 2>&1; then
    printf '%s\n' "mppjpegenc"
  else
    printf '%s\n' "jpegenc quality=$quality"
  fi
}

jpeg_format_for_tainai() {
  if [ "$TAINAI_JPEG_ENCODER" != "software" ] && gst-inspect-1.0 mppjpegenc >/dev/null 2>&1; then
    printf '%s\n' "$MPP_JPEG_FORMAT"
  else
    printf '%s\n' "$JPEG_FORMAT"
  fi
}

run_h264() {
  need_host
  local width="${1:-$WIDTH}"
  local height="${2:-$HEIGHT}"
  local fps="${3:-$FPS}"
  local bitrate="${4:-$BITRATE}"

  if gst-inspect-1.0 mpph264enc >/dev/null 2>&1; then
    echo "Using Rockchip mpph264enc hardware encoder."
    exec sh -c "gst-launch-1.0 -e \
      $(video_chain "$width" "$height" "$fps" 0 NV12) ! \
      mpph264enc bps="$bitrate" ! \
      h264parse config-interval=1 ! \
      rtph264pay pt=96 mtu=1200 config-interval=1 ! \
      udpsink host="$HOST" port="$PORT" sync=false async=false"
  fi

  if gst-inspect-1.0 v4l2h264enc >/dev/null 2>&1; then
    echo "Using v4l2h264enc hardware encoder."
    exec sh -c "gst-launch-1.0 -e \
      $(video_chain "$width" "$height" "$fps" 0 NV12) ! \
      v4l2h264enc extra-controls="controls,video_bitrate=${bitrate};" ! \
      h264parse config-interval=1 ! \
      rtph264pay pt=96 mtu=1200 config-interval=1 ! \
      udpsink host="$HOST" port="$PORT" sync=false async=false"
  fi

  echo "No hardware H.264 encoder found. Install Rockchip/GStreamer MPP plugins first." >&2
  exit 1
}

run_mjpeg() {
  need_host
  local width="${1:-$WIDTH}"
  local height="${2:-$HEIGHT}"
  local fps="${3:-$FPS}"
  local quality="${4:-75}"

  exec sh -c "gst-launch-1.0 -e \
    $(video_chain "$width" "$height" "$fps" 0 "$JPEG_FORMAT") ! \
    jpegenc quality="$quality" ! \
    rtpjpegpay pt=26 ! \
    udpsink host="$HOST" port="$PORT" sync=false async=false"
}

run_tainai_mpegts() {
  need_host
  local transport="$1"
  shift
  local width="${1:-$WIDTH}"
  local height="${2:-$HEIGHT}"
  local fps="${3:-$FPS}"
  local bitrate="${4:-$BITRATE}"

  local sink
  if [ "$transport" = "udp" ]; then
    sink="udpsink host=$HOST port=$PORT sync=false async=false"
  else
    sink="tcpclientsink host=$HOST port=$PORT sync=false"
  fi

  if gst-inspect-1.0 mpph264enc >/dev/null 2>&1; then
    echo "Using Rockchip mpph264enc -> MPEG-TS over ${transport} for TAINAI."
    exec sh -c "gst-launch-1.0 -e \
      $(video_chain "$width" "$height" "$fps" "$TAINAI_CENTER_CROP" NV12) ! \
      mpph264enc bps="$bitrate" ! \
      h264parse config-interval=1 ! \
      mpegtsmux alignment=7 ! \
      $sink"
  fi

  if gst-inspect-1.0 v4l2h264enc >/dev/null 2>&1; then
    echo "Using v4l2h264enc -> MPEG-TS over ${transport} for TAINAI."
    exec sh -c "gst-launch-1.0 -e \
      $(video_chain "$width" "$height" "$fps" "$TAINAI_CENTER_CROP" NV12) ! \
      v4l2h264enc extra-controls="controls,video_bitrate=${bitrate};" ! \
      h264parse config-interval=1 ! \
      mpegtsmux alignment=7 ! \
      $sink"
  fi

  if gst-inspect-1.0 x264enc >/dev/null 2>&1; then
    echo "No hardware encoder found; using x264enc software encoder -> MPEG-TS over ${transport}."
    exec sh -c "gst-launch-1.0 -e \
      $(video_chain "$width" "$height" "$fps" "$TAINAI_CENTER_CROP" I420) ! \
      x264enc tune=zerolatency speed-preset=ultrafast bitrate="$((bitrate / 1000))" key-int-max="$fps" ! \
      h264parse config-interval=1 ! \
      mpegtsmux alignment=7 ! \
      $sink"
  fi

  echo "No H.264 encoder found. Install Rockchip MPP or x264 GStreamer plugins first." >&2
  exit 1
}

run_tainai_jpeg_udp() {
  need_host
  local width="${1:-640}"
  local height="${2:-640}"
  local fps="${3:-60}"
  local quality="${4:-70}"

  if [ "$TAINAI_HW_CROP" = "1" ]; then
    apply_hw_crop "$width" "$height"
  fi

  local encoder encoder_format
  encoder="$(jpeg_encoder_for_tainai "$quality")"
  encoder_format="$(jpeg_format_for_tainai)"
  if [ "$encoder" = "mppjpegenc" ]; then
    echo "Using mppjpegenc hardware JPEG -> raw JPEG UDP for TAINAI."
  else
    echo "Using jpegenc software JPEG -> raw JPEG UDP for TAINAI."
  fi

  local chunk_filter=""
  if [ "$TAINAI_UDP_CHUNK" -gt 0 ]; then
    chunk_filter="rndbuffersize min=$TAINAI_UDP_CHUNK max=$TAINAI_UDP_CHUNK !"
  fi
  exec sh -c "gst-launch-1.0 -e \
    $(video_chain "$width" "$height" "$fps" "$TAINAI_CENTER_CROP" "$encoder_format") ! \
    $encoder ! \
    queue max-size-buffers=1 max-size-time=0 max-size-bytes=0 leaky=downstream ! \
    $chunk_filter \
    udpsink host=$HOST port=$PORT sync=false async=false"
}

run_tainai_jpeg_tcp() {
  need_host
  local width="${1:-640}"
  local height="${2:-640}"
  local fps="${3:-60}"
  local quality="${4:-70}"

  if [ "$TAINAI_HW_CROP" = "1" ]; then
    apply_hw_crop "$width" "$height"
  fi

  local encoder encoder_format
  encoder="$(jpeg_encoder_for_tainai "$quality")"
  encoder_format="$(jpeg_format_for_tainai)"
  if [ "$encoder" = "mppjpegenc" ]; then
    echo "Using mppjpegenc hardware JPEG -> raw JPEG TCP for TAINAI."
  else
    echo "Using jpegenc software JPEG -> raw JPEG TCP for TAINAI."
  fi

  exec sh -c "gst-launch-1.0 -e \
    $(video_chain "$width" "$height" "$fps" "$TAINAI_CENTER_CROP" "$encoder_format") ! \
    $encoder ! \
    queue max-size-buffers=1 max-size-time=0 max-size-bytes=0 leaky=downstream ! \
    tcpclientsink host=$HOST port=$PORT sync=false"
}

run_fps_test() {
  local width="${1:-416}"
  local height="${2:-416}"
  local fps="${3:-60}"
  local quality="${4:-70}"
  local encoder encoder_format

  if [ "$TAINAI_HW_CROP" = "1" ]; then
    apply_hw_crop "$width" "$height"
  fi

  encoder="$(jpeg_encoder_for_tainai "$quality")"
  encoder_format="$(jpeg_format_for_tainai)"

  echo "Testing local HDMI RX -> center crop -> JPEG encode FPS."
  echo "SRC_FORMAT=$SRC_FORMAT encoder=$encoder encoder_format=$encoder_format"
  exec sh -c "gst-launch-1.0 -e -m \
    $(video_chain "$width" "$height" "$fps" "$TAINAI_CENTER_CROP" "$encoder_format") ! \
    $encoder ! \
    fpsdisplaysink video-sink=fakesink text-overlay=false sync=false silent=false fps-update-interval=1000"
}

run_fps_raw_test() {
  local width="${1:-416}"
  local height="${2:-416}"
  local fps="${3:-60}"
  local out_format="${4:-NV12}"

  if [ "$TAINAI_HW_CROP" = "1" ]; then
    apply_hw_crop "$width" "$height"
  fi

  echo "Testing local HDMI RX -> center crop -> raw-frame FPS."
  echo "SRC_FORMAT=$SRC_FORMAT out_format=$out_format"
  exec sh -c "gst-launch-1.0 -e -m \
    $(video_chain "$width" "$height" "$fps" "$TAINAI_CENTER_CROP" "$out_format") ! \
    fpsdisplaysink video-sink=fakesink text-overlay=false sync=false silent=false fps-update-interval=1000"
}

run_count_test() {
  local mode="${1:-raw}"
  local width="${2:-416}"
  local height="${3:-416}"
  local fps="${4:-60}"
  local quality="${5:-70}"
  local frames="${6:-120}"
  local timeout_seconds="${7:-15}"
  local encoder encoder_format pipeline

  if [ "$TAINAI_HW_CROP" = "1" ]; then
    apply_hw_crop "$width" "$height"
  fi

  V4L2_NUM_BUFFERS="$frames"

  if [ "$mode" = "jpeg" ]; then
    encoder="$(jpeg_encoder_for_tainai "$quality")"
    encoder_format="$(jpeg_format_for_tainai)"
    pipeline="$(video_chain "$width" "$height" "$fps" "$TAINAI_CENTER_CROP" "$encoder_format") ! $encoder ! fakesink sync=false async=false"
  else
    encoder_format="${MPP_JPEG_FORMAT:-NV12}"
    pipeline="$(video_chain "$width" "$height" "$fps" "$TAINAI_CENTER_CROP" "$encoder_format") ! fakesink sync=false async=false"
  fi

  echo "Counting $frames frames through $mode pipeline, timeout=${timeout_seconds}s."
  echo "SRC_FORMAT=$SRC_FORMAT SRC_FPS=$SRC_FPS V4L2_IO_MODE=$V4L2_IO_MODE out_format=$encoder_format"
  local start_ns end_ns elapsed_ms measured_fps
  start_ns="$(date +%s%N)"
  if timeout "$timeout_seconds" sh -c "gst-launch-1.0 -q -e $pipeline"; then
    end_ns="$(date +%s%N)"
    elapsed_ms=$(((end_ns - start_ns) / 1000000))
    measured_fps="$(awk -v frames="$frames" -v ms="$elapsed_ms" 'BEGIN { if (ms > 0) printf "%.1f", frames * 1000 / ms; else printf "inf" }')"
    echo "OK: received $frames frames in ${elapsed_ms}ms, approx ${measured_fps} fps"
  else
    echo "FAIL: did not receive $frames frames within ${timeout_seconds}s."
    echo "Try stopping the engine/web preview first, then test SRC_FORMAT=auto or SRC_FPS=auto."
    return 1
  fi
}

run_cpp_jpeg_udp() {
  need_host
  local width="${1:-416}"
  local height="${2:-416}"
  local fps="${3:-60}"
  local quality="${4:-70}"
  local bin="./build/hdmirx_jpeg_udp"

  if [ ! -x "$bin" ]; then
    echo "$bin not found. Build it first:" >&2
    echo "  cd /home/orangepi/aimbot_cpp/build && cmake .. && make -j4 hdmirx_jpeg_udp" >&2
    exit 1
  fi

  local swap_args=()
  if [ "$CPP_SWAP_RB" = "1" ]; then
    swap_args+=(--swap-rb)
  fi
  if [ "$CPP_NO_GST_CROP" = "1" ]; then
    swap_args+=(--no-gst-crop)
  fi

  exec "$bin" \
    --host "$HOST" \
    --port "$PORT" \
    --device "$DEVICE" \
    --width "$width" \
    --height "$height" \
    --fps "$fps" \
    --quality "$quality" \
    --src-width "$SRC_WIDTH" \
    --src-height "$SRC_HEIGHT" \
    --src-fps "$SRC_FPS" \
    --src-format "$SRC_FORMAT" \
    "${swap_args[@]}"
}

run_v4l2_jpeg_udp() {
  need_host
  local width="${1:-416}"
  local height="${2:-416}"
  local fps="${3:-60}"
  local quality="${4:-70}"
  local bin="./build/hdmirx_v4l2_jpeg_udp"

  if [ ! -x "$bin" ]; then
    echo "$bin not found. Build it first:" >&2
    echo "  cd /home/orangepi/aimbot_cpp/build && cmake .. && make -j4 hdmirx_v4l2_jpeg_udp" >&2
    exit 1
  fi

  local swap_args=()
  if [ "$CPP_SWAP_RB" = "1" ]; then
    swap_args+=(--swap-rb)
  fi
  if [ "$V4L2_BLOCKING" = "1" ]; then
    swap_args+=(--blocking)
  fi

  exec "$bin" \
    --host "$HOST" \
    --port "$PORT" \
    --device "$DEVICE" \
    --width "$width" \
    --height "$height" \
    --quality "$quality" \
    --src-width "$SRC_WIDTH" \
    --src-height "$SRC_HEIGHT" \
    --src-fps "$SRC_FPS" \
    "${swap_args[@]}"
}

run_ffmpeg_mjpeg_udp() {
  need_host
  local width="${1:-416}"
  local height="${2:-416}"
  local fps="${3:-240}"
  local quality="${4:-95}"

  if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "ffmpeg not found. Install ffmpeg first." >&2
    exit 1
  fi

  local crop_left crop_right crop_top crop_bottom
  crop_left=$(((SRC_WIDTH - width) / 2))
  crop_right=$((SRC_WIDTH - width - crop_left))
  crop_top=$(((SRC_HEIGHT - height) / 2))
  crop_bottom=$((SRC_HEIGHT - height - crop_top))

  echo "Using GStreamer jpegenc pipe -> ffmpeg MJPEG UDP."
  echo "crop=${width}x${height} left=$crop_left right=$crop_right top=$crop_top bottom=$crop_bottom fps=$fps quality=$quality pkt_size=$FFMPEG_PKT_SIZE"

  gst-launch-1.0 -q \
    v4l2src device="$DEVICE" ! \
    video/x-raw,format="$SRC_FORMAT",width="$SRC_WIDTH",height="$SRC_HEIGHT" ! \
    videocrop top="$crop_top" left="$crop_left" right="$crop_right" bottom="$crop_bottom" ! \
    jpegenc idct-method="$JPEG_IDCT_METHOD" quality="$quality" ! \
    fdsink fd=1 sync=false | \
    ffmpeg -loglevel warning -fflags nobuffer -flags low_delay -y -f mjpeg -framerate "$fps" -i pipe:0 \
      -c copy -flush_packets 1 \
      -f mjpeg "udp://${HOST}:${PORT}?pkt_size=${FFMPEG_PKT_SIZE}"
}

receiver_h264() {
  local port="${1:-$PORT}"
  cat <<EOF
GStreamer receiver:
  gst-launch-1.0 -e udpsrc port=${port} caps="application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000" ! rtph264depay ! h264parse ! avdec_h264 ! autovideosink sync=false

ffplay receiver:
  Create stream.sdp with:
    v=0
    c=IN IP4 0.0.0.0
    m=video ${port} RTP/AVP 96
    a=rtpmap:96 H264/90000
    a=fmtp:96 packetization-mode=1

  Then run:
    ffplay -fflags nobuffer -flags low_delay -framedrop -an -probesize 32 -analyzeduration 0 -protocol_whitelist file,udp,rtp -i stream.sdp
EOF
}

receiver_mjpeg() {
  local port="${1:-$PORT}"
  cat <<EOF
GStreamer receiver:
  gst-launch-1.0 -e udpsrc port=${port} caps="application/x-rtp,media=video,encoding-name=JPEG,payload=26,clock-rate=90000" ! rtpjpegdepay ! jpegdec ! autovideosink sync=false
EOF
}

cmd="${1:-}"
shift || true

case "$cmd" in
  h264)
    run_h264 "$@"
    ;;
  mjpeg)
    run_mjpeg "$@"
    ;;
  tainai-udp)
    run_tainai_mpegts udp "$@"
    ;;
  tainai-jpeg-udp)
    run_tainai_jpeg_udp "$@"
    ;;
  tainai-jpeg-tcp)
    run_tainai_jpeg_tcp "$@"
    ;;
  tainai-tcp)
    run_tainai_mpegts tcp "$@"
    ;;
  cpp-jpeg-udp)
    run_cpp_jpeg_udp "$@"
    ;;
  v4l2-jpeg-udp)
    run_v4l2_jpeg_udp "$@"
    ;;
  ffmpeg-mjpeg-udp)
    run_ffmpeg_mjpeg_udp "$@"
    ;;
  fps-test)
    run_fps_test "$@"
    ;;
  fps-raw-test)
    run_fps_raw_test "$@"
    ;;
  count-test)
    run_count_test "$@"
    ;;
  crop-info)
    crop_info
    ;;
  clear-crop)
    clear_crop
    ;;
  diagnose)
    diagnose
    ;;
  receiver-h264)
    receiver_h264 "$@"
    ;;
  receiver-mjpeg)
    receiver_mjpeg "$@"
    ;;
  *)
    usage
    exit 1
    ;;
esac
