#!/usr/bin/env bash

VISION_DIR="${VISION_DIR:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)}"
RUNTIME_DIR="$VISION_DIR/run"
LOG_DIR="$VISION_DIR/log"
BIN_DIR="$VISION_DIR/bin"
CONFIG_DIR="$VISION_DIR/config"
# 原始下载地址: https://pixabay.com/sound-effects/nature-tranquil-stream-387678/
BACKGROUND_MUSIC_FILE="$CONFIG_DIR/audio.opus"
MEDIAMTX_BIN="$BIN_DIR/mediamtx"
MEDIAMTX_CONFIG="$CONFIG_DIR/mediamtx.yml"
EXPECTED_MEDIAMTX_SHA256=5BDAF5BA8BCB8E2A502F5CF96EE8BAF39D11CFC257F12EBAC304F4B0307C7C92
VISION_CONFIG="${VISION_CONFIG:-$CONFIG_DIR/vision.env}"
LEGACY_TRACKING_CONFIG="$CONFIG_DIR/tracking.env"
LEGACY_LLM_CONFIG="$CONFIG_DIR/llm_bridge.env"
MODEL_FILE="$VISION_DIR/yolo-models/yolo26s_6_pcq_a733.nb"
YOLO_BIN="$BIN_DIR/yolo26_a733"
ISP3A_BIN="$BIN_DIR/isp3a-daemon"
NV12_NORMALIZER_BIN="$BIN_DIR/vin-nv12-normalizer"
HW_ENCODER_BIN="${HW_ENCODER_BIN:-$BIN_DIR/aw-h264-encoder}"
LLM_BRIDGE_PY="$VISION_DIR/src/src-llm/llm_bridge.py"
LLM_BRIDGE_LOG="$LOG_DIR/llm_bridge.log"
LLM_BRIDGE_PID_FILE="$RUNTIME_DIR/llm_bridge.pid"
DASHBOARD_PY="$VISION_DIR/src/src-llm/vision_dashboard.py"
DASHBOARD_LOG="$LOG_DIR/dashboard.log"
DASHBOARD_PID_FILE="$RUNTIME_DIR/dashboard.pid"

MEDIA_DEVICE=/dev/media0
VIDEO_DEVICE=/dev/video0
UDP_PORT=8891

# 默认日志检查周期, 单位为秒. 手动修改 DEFAULT_LOG_CHECK_INTERVAL_SECONDS 即可调整检查频率; 必须使用正整数.
DEFAULT_LOG_CHECK_INTERVAL_SECONDS=18000
LOG_CHECK_INTERVAL_SECONDS="${LOG_CHECK_INTERVAL_SECONDS:-$DEFAULT_LOG_CHECK_INTERVAL_SECONDS}"
case "$LOG_CHECK_INTERVAL_SECONDS" in
    ''|*[!0-9]*|0) echo "LOG_CHECK_INTERVAL_SECONDS 必须是正整数. " >&2; return 2 2>/dev/null || exit 2 ;;
esac

# 普通日志超过 10 MiB 时轮换, .1 只保留最后 5 MiB
LOG_ROTATE_THRESHOLD_BYTES=$((10 * 1024 * 1024))
LOG_BACKUP_BYTES=$((5 * 1024 * 1024))

VISION_SIZE="${VISION_SIZE:-1280x720}"
VISION_FPS="${VISION_FPS:-30}"
VISION_MODE="${VISION_MODE:-camera}"
VISION_ENCODER="${VISION_ENCODER:-hardware}"
VISION_ROTATE="${VISION_ROTATE:-180}"
VISION_AUDIO_ENABLED="${VISION_AUDIO_ENABLED:-auto}"
VISION_SRT_CLOUD_HOST="${VISION_SRT_CLOUD_HOST:-}"
VISION_SRT_PASSPHRASE="${VISION_SRT_PASSPHRASE:-}"
VISION_SRT_USERNAME="${VISION_SRT_USERNAME:-}"
VISION_SRT_PASSWORD="${VISION_SRT_PASSWORD:-}"
VISION_SRT_LATENCY_MS="${VISION_SRT_LATENCY_MS:-}"
case "$VISION_SIZE" in
    640x360) CAPTURE_WIDTH=640; CAPTURE_HEIGHT=360; VIN_FRAME_BYTES=353280; VIDEO_BITRATE_BPS=1000000 ;;
    1280x720) CAPTURE_WIDTH=1280; CAPTURE_HEIGHT=720; VIN_FRAME_BYTES=1382400; VIDEO_BITRATE_BPS=2500000 ;;
    1920x1080) CAPTURE_WIDTH=1920; CAPTURE_HEIGHT=1080; VIN_FRAME_BYTES=3133440; VIDEO_BITRATE_BPS=4000000 ;;
    *) echo "不支持的画面尺寸: $VISION_SIZE" >&2; return 2 2>/dev/null || exit 2 ;;
esac
[[ "$VISION_FPS" =~ ^([1-9]|[12][0-9]|30)$ ]] || {
    echo "不支持的帧率: $VISION_FPS (必须是 1～30 的整数)" >&2
    return 2 2>/dev/null || exit 2
}
OUTPUT_FPS=$VISION_FPS
case "$VISION_MODE" in
    camera|yolo) ;;
    *) echo "不支持的运行模式: $VISION_MODE" >&2; return 2 2>/dev/null || exit 2 ;;
esac
case "$VISION_ENCODER" in
    hardware|software) ;;
    *) echo "不支持的编码方式: $VISION_ENCODER" >&2; return 2 2>/dev/null || exit 2 ;;
esac
case "$VISION_ROTATE" in
    0) ROTATION_CODE=0; GST_ROTATION=identity ;;
    90) ROTATION_CODE=1; GST_ROTATION=90r ;;
    180) ROTATION_CODE=2; GST_ROTATION=180 ;;
    270) ROTATION_CODE=3; GST_ROTATION=90l ;;
    *) echo "不支持的旋转角度: $VISION_ROTATE" >&2; return 2 2>/dev/null || exit 2 ;;
esac
if [[ "$VISION_ROTATE" == 90 || "$VISION_ROTATE" == 270 ]]; then
    OUTPUT_WIDTH=$CAPTURE_HEIGHT
    OUTPUT_HEIGHT=$CAPTURE_WIDTH
else
    OUTPUT_WIDTH=$CAPTURE_WIDTH
    OUTPUT_HEIGHT=$CAPTURE_HEIGHT
fi
X264_BITRATE_KBPS=$((VIDEO_BITRATE_BPS / 1000))

NV12_FRAME_BYTES=$((CAPTURE_WIDTH * CAPTURE_HEIGHT * 3 / 2))
if ((VIN_FRAME_BYTES == NV12_FRAME_BYTES)); then
    NV12_NORMALIZER_MODE=bypass
else
    NV12_NORMALIZER_MODE=enabled
fi
mkdir -p "$RUNTIME_DIR" "$LOG_DIR"

maintain_log_file() {
    local file=$1 size temporary
    [[ -f "$file" ]] || return 0
    size=$(wc -c < "$file" 2>/dev/null) || return 0
    ((size > LOG_ROTATE_THRESHOLD_BYTES)) || return 0
    temporary="$file.1.tmp.$$"
    if tail -c "$LOG_BACKUP_BYTES" -- "$file" > "$temporary" 2>/dev/null &&
       mv -f -- "$temporary" "$file.1"; then
        : > "$file"
    else
        rm -f -- "$temporary"
        echo "日志轮换失败: $file" >&2
        return 1
    fi
}

maintain_logs() {
    local file result=0
    mkdir -p "$RUNTIME_DIR" "$LOG_DIR"
    for file in "$LOG_DIR"/*.log; do
        [[ -e "$file" ]] || continue
        maintain_log_file "$file" || result=1
    done
    return "$result"
}

log_maintenance_loop() {
    while sleep "$LOG_CHECK_INTERVAL_SECONDS"; do maintain_logs || true; done
}

init_camera_pipeline() {
    sudo modprobe vin_v4l2
    # media-ctl -d /dev/media0 -p  # 查看拓扑
    sudo media-ctl -d "$MEDIA_DEVICE" -V '"imx219":0 [fmt:SRGGB10_1X10/1920x1080 field:none]'
    sudo media-ctl -d "$MEDIA_DEVICE" -l '"imx219":0         -> "sunxi_mipi.0":0 [1]'
    sudo media-ctl -d "$MEDIA_DEVICE" -l '"sunxi_mipi.0":1   -> "sunxi_csi.0":0 [1]'
    sudo media-ctl -d "$MEDIA_DEVICE" -l '"sunxi_csi.0":1    -> "sunxi_tdm_rx.0":0 [1]'
    sudo media-ctl -d "$MEDIA_DEVICE" -l '"sunxi_tdm_rx.0":1 -> "sunxi_isp.0":0 [1]'
    sudo media-ctl -d "$MEDIA_DEVICE" -l '"sunxi_isp.0":2    -> "sunxi_scaler.0":0 [1]'
    sudo media-ctl -d "$MEDIA_DEVICE" -l '"sunxi_scaler.0":1 -> "vin_cap.0":0 [1]'
    sudo media-ctl -d "$MEDIA_DEVICE" -l '"vin_cap.0":1      -> "vin_video0":0 [1]'
}

capture_nv12() {
    v4l2-ctl -d "$VIDEO_DEVICE" --set-input=0 \
        --set-fmt-video=width=${CAPTURE_WIDTH},height=${CAPTURE_HEIGHT},pixelformat=NV12 \
        --stream-mmap=4 --stream-poll --stream-count=0 --stream-to=/dev/stdout --silent
}

normalize_nv12() {
    if [[ "$NV12_NORMALIZER_MODE" == bypass ]]; then
        cat
    else
        "$NV12_NORMALIZER_BIN" --width "$CAPTURE_WIDTH" --height "$CAPTURE_HEIGHT" \
            --input-frame-bytes "$VIN_FRAME_BYTES"
    fi
}

background_music_enabled() {
    [[ "$VISION_AUDIO_ENABLED" != no &&
       -r "$BACKGROUND_MUSIC_FILE" && -s "$BACKGROUND_MUSIC_FILE" ]]
}

load_vision_config() {
    if [[ -r "$VISION_CONFIG" ]]; then
        set -a
        # shellcheck disable=SC1090
        source "$VISION_CONFIG"
        set +a
        return 0
    fi

    # Temporary migration support. Remove after all boards use vision.env.
    if [[ -r "$LEGACY_TRACKING_CONFIG" || -r "$LEGACY_LLM_CONFIG" ]]; then
        echo "未找到 config/vision.env，使用旧配置；请迁移到统一配置。" >&2
        set -a
        [[ ! -r "$LEGACY_TRACKING_CONFIG" ]] || source "$LEGACY_TRACKING_CONFIG"
        [[ ! -r "$LEGACY_LLM_CONFIG" ]] || source "$LEGACY_LLM_CONFIG"
        set +a
        [[ -z "${LLM_API_KEY:-}" ]] || VISION_LLM_ENABLED=yes
    fi
}

srt_is_configured() {
    [[ -n "$VISION_SRT_CLOUD_HOST" && -n "$VISION_SRT_PASSPHRASE" &&
       -n "$VISION_SRT_USERNAME" && -n "$VISION_SRT_PASSWORD" &&
       -n "$VISION_SRT_LATENCY_MS" ]]
}

validate_srt_config() {
    local value
    if ! srt_is_configured; then
        if [[ -n "$VISION_SRT_CLOUD_HOST$VISION_SRT_PASSPHRASE$VISION_SRT_USERNAME$VISION_SRT_PASSWORD$VISION_SRT_LATENCY_MS" ]]; then
            echo "SRT 配置必须同时填写云端主机、AES 密码、用户名、密码和延迟。" >&2
            return 1
        fi
        return 0
    fi
    [[ "$VISION_SRT_CLOUD_HOST" =~ ^[A-Za-z0-9][A-Za-z0-9.-]*$ ]] || {
        echo "SRT 云端主机仅允许域名或 IPv4 地址。" >&2; return 1;
    }
    [[ "$VISION_SRT_LATENCY_MS" =~ ^[1-9][0-9]*$ ]] || {
        echo "SRT 延迟必须是正整数毫秒。" >&2; return 1;
    }
    for value in "$VISION_SRT_PASSPHRASE" "$VISION_SRT_USERNAME" "$VISION_SRT_PASSWORD"; do
        [[ "$value" =~ ^[A-Za-z0-9_-]{16,32}$ ]] || {
            echo "SRT 密码和用户名必须为 16–32 位字母、数字、_ 或 -。" >&2; return 1;
        }
    done
}

select_output_fps() {
    if ((OUTPUT_FPS == 30)); then
        cat
        return
    fi
    gst-launch-1.0 -q fdsrc ! rawvideoparse format=nv12 \
        width="$CAPTURE_WIDTH" height="$CAPTURE_HEIGHT" framerate=30/1 ! \
        videorate ! \
        video/x-raw,format=NV12,width="$CAPTURE_WIDTH",height="$CAPTURE_HEIGHT",framerate="$OUTPUT_FPS"/1 ! \
        fdsink fd=1 sync=false
}

rotate_nv12_for_software() {
    if [[ "$VISION_ROTATE" == 0 ]]; then
        cat
        return
    fi
    gst-launch-1.0 -q fdsrc ! rawvideoparse format=nv12 \
        width="$CAPTURE_WIDTH" height="$CAPTURE_HEIGHT" framerate="$OUTPUT_FPS"/1 ! \
        videoflip video-direction="$GST_ROTATION" ! \
        video/x-raw,format=NV12,width="$OUTPUT_WIDTH",height="$OUTPUT_HEIGHT",framerate="$OUTPUT_FPS"/1 ! \
        fdsink fd=1 sync=false
}

h264_level() {
    if [[ "$VISION_SIZE" == 1920x1080 ]]; then
        printf '40\n'
    else
        printf '31\n'
    fi
}

encode_h264_hardware() {
    local width=$1 height=$2 rotation_code=$3
    "$HW_ENCODER_BIN" \
        --width "$width" --height "$height" \
        --fps "$OUTPUT_FPS" --bitrate "$VIDEO_BITRATE_BPS" \
        --gop "$OUTPUT_FPS" --level "$(h264_level)" \
        --rotate "$rotation_code" --frames 0 \
        2> >(grep -v -E '^(DEBUG|INFO)[[:space:]]*:' >&2)
}

encode_h264_software() {
    local width=$1 height=$2
    gst-launch-1.0 -q fdsrc ! rawvideoparse format=nv12 \
        width="$width" height="$height" framerate="$OUTPUT_FPS"/1 ! \
        x264enc tune=zerolatency speed-preset=ultrafast \
        bitrate="$X264_BITRATE_KBPS" key-int-max="$OUTPUT_FPS" \
        byte-stream=true ! video/x-h264,profile=baseline,stream-format=byte-stream ! \
        h264parse config-interval=-1 ! fdsink fd=1 sync=false
}

mux_h264_stream() {
    local local_output mux_format mux_options
    local_output="udp://127.0.0.1:$UDP_PORT?pkt_size=1316"
    if srt_is_configured; then
        local srt_output
        srt_output="[f=fifo:fifo_format=mpegts:onfail=ignore:queue_size=512:attempt_recovery=1:recover_any_error=1:recovery_wait_time=5:drop_pkts_on_overflow=1:restart_with_keyframe=1]srt://${VISION_SRT_CLOUD_HOST}:8890?mode=caller&latency=${VISION_SRT_LATENCY_MS}&passphrase=${VISION_SRT_PASSPHRASE}&pbkeylen=16&streamid=publish:vision:${VISION_SRT_USERNAME}:${VISION_SRT_PASSWORD}"
        mux_format="tee"
        mux_options="[f=mpegts:onfail=ignore]$local_output|$srt_output"
        echo "SRT 转发已启用；远端不可用时将自动重试，本地 WebRTC 不受影响。" >&2
    else
        mux_format="mpegts"
        mux_options="$local_output"
    fi
    if background_music_enabled; then
        ffmpeg -hide_banner -loglevel warning \
            -use_wallclock_as_timestamps 1 \
            -thread_queue_size 1024 -f h264 -i pipe:0 \
            -stream_loop -1 -i "$BACKGROUND_MUSIC_FILE" \
            -map 0:v:0 -map 1:a:0 \
            -c:v copy -c:a copy \
            -shortest -f "$mux_format" "$mux_options"
    else
        ffmpeg -hide_banner -loglevel warning \
            -use_wallclock_as_timestamps 1 \
            -thread_queue_size 1024 -f h264 -i pipe:0 \
            -map 0:v:0 -c:v copy \
            -f "$mux_format" "$mux_options"
    fi
}

run_camera_publisher() {
    set -o pipefail
    if [[ "$VISION_ENCODER" == hardware ]]; then
        capture_nv12 | normalize_nv12 | select_output_fps | \
            encode_h264_hardware "$CAPTURE_WIDTH" "$CAPTURE_HEIGHT" \
                "$ROTATION_CODE" | \
            mux_h264_stream
    else
        capture_nv12 | normalize_nv12 | select_output_fps | \
            rotate_nv12_for_software | \
            encode_h264_software "$OUTPUT_WIDTH" "$OUTPUT_HEIGHT" | \
            mux_h264_stream
    fi
}

run_yolo_nv12() {
    YOLO_DETECTION_LOG="${YOLO_DETECTION_LOG:-$LOG_DIR/yolo-detections.jsonl}" \
    YOLO_DETECTION_LOG_MAX_BYTES="${YOLO_DETECTION_LOG_MAX_BYTES:-5242880}" \
    YOLO_DETECTION_LOG_BACKUPS="${YOLO_DETECTION_LOG_BACKUPS:-3}" \
    YOLO_DETECTION_INTERVAL_MS="${YOLO_DETECTION_INTERVAL_MS:-1000}" \
    YOLO_TRACKER_METRICS="${YOLO_TRACKER_METRICS:-$RUNTIME_DIR/bytetrack-metrics.json}" \
    YOLO_TRACK_CLASSES="${YOLO_TRACK_CLASSES:-person,cat,dog}" \
    "$YOLO_BIN" -nb "$MODEL_FILE" --pipe --pixel-format nv12 \
        --rotate "$VISION_ROTATE" \
        -W "$CAPTURE_WIDTH" -H "$CAPTURE_HEIGHT"
}

encode_oriented_nv12() {
    if [[ "$VISION_ENCODER" == hardware ]]; then
        encode_h264_hardware "$OUTPUT_WIDTH" "$OUTPUT_HEIGHT" 0
    else
        encode_h264_software "$OUTPUT_WIDTH" "$OUTPUT_HEIGHT"
    fi
}

start_llm_bridge() {
    local jsonl_path="${YOLO_DETECTION_LOG:-$LOG_DIR/yolo-detections.jsonl}"
    if [[ ! -f "$LLM_BRIDGE_PY" ]]; then
        echo "找不到事件 bridge 脚本: $LLM_BRIDGE_PY" >&2
        return 0
    fi
    if [[ "${VISION_LLM_ENABLED:-yes}" == yes && -n "${LLM_API_KEY:-}" ]]; then
        echo "启动事件 bridge + LLM 叙事: jsonl=$jsonl_path log=$LLM_BRIDGE_LOG" >&2
    else
        echo "启动事件 bridge（本地事实模式）: jsonl=$jsonl_path log=$LLM_BRIDGE_LOG" >&2
    fi
    (
        # 等待 JSONL 文件出现（YOLO 启动后才会创建）
        for _ in $(seq 1 30); do
            [[ -f "$jsonl_path" ]] && break
            sleep 1
        done
        if [[ ! -f "$jsonl_path" ]]; then
            echo "LLM bridge: JSONL 文件未出现 ($jsonl_path)，退出。" >&2
            exit 1
        fi
        tail -F "$jsonl_path" 2>/dev/null | python3 "$LLM_BRIDGE_PY"
    ) >> "$LLM_BRIDGE_LOG" 2>&1 &
    echo $! > "$LLM_BRIDGE_PID_FILE"
}

start_dashboard() {
    [[ "${DASHBOARD_ENABLED:-yes}" == yes ]] || return 0
    [[ -f "$DASHBOARD_PY" ]] || { echo "找不到 Dashboard: $DASHBOARD_PY" >&2; return 0; }
    local db_path="${VISION_EVENT_DB:-$RUNTIME_DIR/vision-events.sqlite3}"
    echo "启动 Dashboard: http://0.0.0.0:${DASHBOARD_PORT:-8080}" >&2
    python3 "$DASHBOARD_PY" --db "$db_path" --port "${DASHBOARD_PORT:-8080}" \
        --budget "${LLM_DAILY_TOKEN_BUDGET:-20000}" >> "$DASHBOARD_LOG" 2>&1 &
    echo $! > "$DASHBOARD_PID_FILE"
}

stop_dashboard() {
    local pid
    [[ -r "$DASHBOARD_PID_FILE" ]] || return 0
    read -r pid < "$DASHBOARD_PID_FILE" 2>/dev/null || return 0
    kill "$pid" 2>/dev/null || true
    rm -f "$DASHBOARD_PID_FILE"
}

stop_llm_bridge() {
    local pid
    [[ -r "$LLM_BRIDGE_PID_FILE" ]] || return 0
    read -r pid < "$LLM_BRIDGE_PID_FILE" 2>/dev/null || return 0
    kill "$pid" 2>/dev/null || true
    rm -f "$LLM_BRIDGE_PID_FILE"
}

run_yolo_publisher() {
    set -o pipefail
    export LD_LIBRARY_PATH="$VISION_DIR/lib:${LD_LIBRARY_PATH:-}"
    load_vision_config
    start_dashboard
    start_llm_bridge
    capture_nv12 | normalize_nv12 | select_output_fps | \
        run_yolo_nv12 | encode_oriented_nv12 | mux_h264_stream
    stop_llm_bridge
    stop_dashboard
}

run_publisher() {
    case "$VISION_MODE" in
        camera) run_camera_publisher ;;
        yolo) run_yolo_publisher ;;
    esac
}

run_publisher_with_log_maintenance() {
    local maintenance_pid publisher_status
    log_maintenance_loop &
    maintenance_pid=$!
    run_publisher
    publisher_status=$?
    kill "$maintenance_pid" 2>/dev/null || true
    wait "$maintenance_pid" 2>/dev/null || true
    return "$publisher_status"
}
