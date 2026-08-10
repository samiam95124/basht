#!/bin/bash
#
# record-demo.sh -- record a screen region with the webcam as a
# picture-in-picture in the lower right corner, plus microphone.
# Made for recording the basht README demo.
#
# Usage:
#   tools/record-demo.sh [region] [output.mp4] [options]
#
# region:
#   window        click a window to record it (default)
#   screenN       monitor N (1-based, xrandr order: screen1 = DP-0)
#   NAME          an xrandr output name, e.g. DP-0, HDMI-0
#   full          the whole virtual screen (all monitors)
#   WxH+X+Y       explicit geometry, e.g. 1280x720+100+80
#
# options:
#   --cam DEV     camera device            (default /dev/video0)
#   --camw N      PiP width in pixels      (default: output width / 4)
#   --mic NAME    pulseaudio source name   (default: default source;
#                 list with: pactl list short sources)
#   --no-audio    no microphone track
#   --fps N       capture framerate        (default 30)
#   --outw N      scale the video to N pixels wide (default 1920
#                 when the region is wider than that; use the
#                 region's own width with --outw 0)
#   --cpu         force libx264 (default: h264_nvenc if available)
#   --duration N  stop after N seconds (default: run until q)
#
# Stop recording by pressing q in this terminal (or Ctrl+C).

set -u

DISPLAY="${DISPLAY:-:0}"
export DISPLAY

region="window"
out=""
cam="/dev/video0"
camw=""
mic="default"
audio=1
fps=30
duration=""
outw=""
cpu=0

while [ $# -gt 0 ]; do
    case "$1" in
        --cam)      cam="$2"; shift 2 ;;
        --camw)     camw="$2"; shift 2 ;;
        --mic)      mic="$2"; shift 2 ;;
        --no-audio) audio=0; shift ;;
        --fps)      fps="$2"; shift 2 ;;
        --outw)     outw="$2"; shift 2 ;;
        --cpu)      cpu=1; shift ;;
        --duration) duration="$2"; shift 2 ;;
        -*)         echo "unknown option: $1" >&2; exit 2 ;;
        *)  if [ -z "$out" ] && [ "$region" != "window" -o -n "${got_region:-}" ]; then
                out="$1"
            else
                region="$1"; got_region=1
            fi
            shift ;;
    esac
done
[ -n "$out" ] || out="demo-$(date +%Y%m%d-%H%M%S).mp4"

# ---- resolve the capture region ------------------------------------
mon_geom () {   # $1 = 1-based index or output name -> "W H X Y"
    xrandr --listmonitors | awk -v want="$1" '
        NR > 1 {
            idx = idx + 1
            name = $NF
            g = $3
            if (want == idx"" || want == name) {
                split(g, a, /[\/x+]/)
                # WxH+X+Y with /mm sizes: W a[1], H a[3], X a[5], Y a[6]
                print a[1], a[3], a[5], a[6]
            }
        }'
}

case "$region" in
    full)
        geom=$(xdpyinfo | awk '/dimensions:/ {print $2}')
        W=${geom%x*}; H=${geom#*x}; X=0; Y=0
        ;;
    screen[0-9]*|DP-*|HDMI-*|DVI-*|VGA-*|eDP-*)
        sel="${region#screen}"
        read -r W H X Y <<<"$(mon_geom "$sel")"
        if [ -z "${W:-}" ]; then
            echo "no such monitor: $region -- available:" >&2
            xrandr --listmonitors >&2
            exit 1
        fi
        ;;
    window)
        echo "Click the window to record..."
        info=$(xwininfo -frame) || exit 1
        X=$(awk '/Absolute upper-left X/ {print $NF}' <<<"$info")
        Y=$(awk '/Absolute upper-left Y/ {print $NF}' <<<"$info")
        W=$(awk '/Width:/  {print $NF}' <<<"$info")
        H=$(awk '/Height:/ {print $NF}' <<<"$info")
        ;;
    *)
        # WxH+X+Y
        W=$(sed 's/x.*//'          <<<"$region")
        H=$(sed 's/^[0-9]*x\([0-9]*\).*/\1/' <<<"$region")
        X=$(sed 's/^[0-9]*x[0-9]*+\([0-9]*\)+.*/\1/' <<<"$region")
        Y=$(sed 's/.*+//'          <<<"$region")
        ;;
esac

# h264 wants even dimensions
W=$((W - W % 2)); H=$((H - H % 2))

# output scaling: 6K captures encode and embed badly; default to
# 1920 wide unless asked otherwise (--outw 0 keeps native)
if [ -z "$outw" ]; then
    if [ "$W" -gt 1920 ]; then outw=1920; else outw=0; fi
fi
scaled_w=$W
[ "$outw" -gt 0 ] 2>/dev/null && [ "$outw" -lt "$W" ] && scaled_w=$outw
scaled_w=$((scaled_w - scaled_w % 2))

[ -n "$camw" ] || camw=$((scaled_w / 4))
[ "$camw" -ge 160 ] || camw=160
camw=$((camw - camw % 2))

# encoder: NVENC offloads the encode to the GPU when present
venc=( -c:v libx264 -preset veryfast -crf 20 )
if [ $cpu -eq 0 ] && ffmpeg -hide_banner -encoders 2>/dev/null \
     | grep -q h264_nvenc; then
    venc=( -c:v h264_nvenc -preset fast -cq 23 )
fi

echo "Recording ${W}x${H}+${X}+${Y} -> $out (${scaled_w} wide, ${venc[1]})"
echo "Camera: $cam (PiP width $camw, lower right)"
[ $audio -eq 1 ] && echo "Mic: $mic" || echo "Mic: off"
echo
# reclaim the camera from a stale recorder (a killed script can
# leave its ffmpeg behind, holding the device)
if ! timeout 1 fuser -s "$cam" 2>/dev/null; then :; else
    for p in $(fuser "$cam" 2>/dev/null); do
        if [ "$(ps -o comm= -p "$p" 2>/dev/null)" = "ffmpeg" ] &&
           [ "$(ps -o user= -p "$p" 2>/dev/null)" = "$(id -un)" ]; then
            echo "reclaiming $cam from stale ffmpeg (pid $p)"
            kill -INT "$p" 2>/dev/null
            sleep 1
            kill -KILL "$p" 2>/dev/null
        fi
    done
    if fuser -s "$cam" 2>/dev/null; then
        echo "$cam is in use by:" >&2
        fuser -v "$cam" >&2
        exit 1
    fi
fi

# whatever happens to this script, its recorder dies with it
ffpid=""
warmpid=""
cleanup () {
    [ -n "$warmpid" ] && kill "$warmpid" 2>/dev/null
    if [ -n "$ffpid" ] && kill -0 "$ffpid" 2>/dev/null; then
        kill -INT "$ffpid" 2>/dev/null
        # give ffmpeg time to finalize the mp4 (faststart rewrites
        # the file on shutdown) before resorting to KILL
        for _i in $(seq 20); do
            kill -0 "$ffpid" 2>/dev/null || break
            sleep 0.5
        done
        kill -KILL "$ffpid" 2>/dev/null
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 143' TERM

# warm the camera through the countdown: the first seconds off a
# cold webcam are black while auto-exposure settles
ffmpeg -hide_banner -loglevel error -f v4l2 -input_format mjpeg \
       -video_size 640x480 -i "$cam" -t 3 -f null - 2>/dev/null &
warmpid=$!
for s in 3 2 1; do printf '  %d...\r' $s; sleep 1; done
wait $warmpid 2>/dev/null
warmpid=""
echo "  recording -- press q here to stop."

# ---- assemble the ffmpeg command -----------------------------------
# input 0: screen region   input 1: camera   input 2: microphone
args=(
    -thread_queue_size 512
    -f x11grab -framerate "$fps" -video_size "${W}x${H}"
        -i "${DISPLAY}+${X},${Y}"
    -thread_queue_size 512
    -f v4l2 -input_format mjpeg -framerate 30 -video_size 640x480
        -i "$cam"
)
if [ $audio -eq 1 ]; then
    args+=( -thread_queue_size 512 -f pulse -i "$mic" )
fi
if [ "$scaled_w" -lt "$W" ]; then
    mainsrc="[0:v]scale=${scaled_w}:-2[scr];"
    mainpad="[scr]"
else
    mainsrc=""
    mainpad="[0:v]"
fi
args+=(
    -filter_complex
    "${mainsrc}[1:v]scale=${camw}:-2[cam];${mainpad}[cam]overlay=main_w-overlay_w-16:main_h-overlay_h-16"
    "${venc[@]}" -pix_fmt yuv420p
)
if [ $audio -eq 1 ]; then
    args+=( -c:a aac -b:a 128k )
fi
if [ -n "$duration" ]; then
    args+=( -t "$duration" )
fi
args+=( -movflags +faststart "$out" )

# background + explicit tty stdin: `q' still reaches ffmpeg, and the
# EXIT trap can kill it if this script is killed from elsewhere.
if [ -t 0 ]; then
    ffmpeg -y -hide_banner -loglevel warning "${args[@]}" < /dev/tty &
else
    ffmpeg -y -hide_banner -loglevel warning -nostdin "${args[@]}" &
fi
ffpid=$!
wait $ffpid
rc=$?
ffpid=""

if [ $rc -eq 0 ] || [ $rc -eq 255 ]; then    # 255 = stopped with q
    echo
    echo "Wrote $out"
    ffprobe -v error -show_entries format=duration,size \
            -of default=noprint_wrappers=1 "$out" 2>/dev/null
else
    echo "ffmpeg failed ($rc)" >&2
fi
exit 0
