#!/bin/sh
# Compare full-resolution and adaptive ROI decoding on live camera or replay.
set -eu

APP_DIR="${TINYTAG_BENCH_APP_DIR:-/app/tinytag_detect}"
RUN_LIVE="${TINYTAG_BENCH_RUN_LIVE:-${APP_DIR}/run_live.sh}"
BIN="${TINYTAG_BENCH_BIN:-${APP_DIR}/tinytag_detect_live_adaptive_bench}"
DURATION="${TINYTAG_BENCH_DURATION:-20}"
REPEATS="${TINYTAG_BENCH_REPEATS:-2}"
OUT_DIR="${TINYTAG_BENCH_OUT_DIR:-/tmp/tinytag-adaptive-bench-$(date +%Y%m%d-%H%M%S)}"
INPUT="${TINYTAG_BENCH_INPUT:-}"
REPLAY_TIMEOUT="${TINYTAG_BENCH_REPLAY_TIMEOUT:-120}"

case "${DURATION}:${REPEATS}" in
    *[!0-9:]*|:*|*:) echo "duration and repeats must be positive integers" >&2; exit 1 ;;
esac
[ "${DURATION}" -ge 5 ] && [ "${REPEATS}" -ge 1 ] || {
    echo "duration must be at least 5s; repeats at least 1" >&2; exit 1;
}
[ -z "${INPUT}" ] || {
    case "${REPLAY_TIMEOUT}" in
        ''|*[!0-9]*) echo "replay timeout must be a positive integer" >&2; exit 1 ;;
    esac
    [ "${REPLAY_TIMEOUT}" -ge 10 ] || {
        echo "replay timeout must be at least 10s" >&2; exit 1;
    }
}
[ -x "${RUN_LIVE}" ] && [ -x "${BIN}" ] || {
    echo "launcher or adaptive binary is missing" >&2; exit 1;
}
command -v timeout >/dev/null 2>&1 || { echo "timeout is required" >&2; exit 1; }
if [ -n "${INPUT}" ]; then
    [ -r "${INPUT}" ] || { echo "input video is not readable: ${INPUT}" >&2; exit 1; }
fi

mkdir -p "${OUT_DIR}"
RESULTS="${OUT_DIR}/windows.tsv"
printf 'round\tmode\twindows\tfps\tloop_ms\tcrop_ms\tresult_age_ms\tmarkers_per_frame\tlow_attempts\tlow_hits\tfallbacks\taudits\tdeferred\tprocessed_frames\tsource_frames\n' >"${RESULTS}"

run_case()
{
    mode="$1"
    round="$2"
    adaptive="$3"
    log="${OUT_DIR}/${round}-${mode}.log"
    if [ -n "${INPUT}" ]; then
        echo "Running ${round} ${mode} on ${INPUT} in lockstep"
    else
        echo "Running ${round} ${mode} for ${DURATION}s"
    fi
    rc=0
    if [ -n "${INPUT}" ]; then
        TINYTAG_LIVE_BIN="${BIN}" timeout -s INT -k 5 "${REPLAY_TIMEOUT}" \
            "${RUN_LIVE}" --input "${INPUT}" --input-speed 0 \
            --adaptive-decode "${adaptive}" --tag-output 1 \
            </dev/null >"${log}" 2>&1 || rc=$?
        [ "${rc}" -eq 0 ] || {
            echo "${mode} exited ${rc} (timeout is ${REPLAY_TIMEOUT}s): ${log}" >&2; exit 1;
        }
        grep -q '^\[input\] detector processed ' "${log}" || {
            echo "${mode} has no replay completion count: ${log}" >&2; exit 1;
        }
    else
        TINYTAG_LIVE_BIN="${BIN}" timeout -s INT "${DURATION}" \
            "${RUN_LIVE}" --adaptive-decode "${adaptive}" \
            </dev/null >"${log}" 2>&1 || rc=$?
        case "${rc}" in 0|124) ;; *) echo "${mode} exited ${rc}: ${log}" >&2; exit 1 ;; esac
    fi
    grep -q '^\[camera\] stopping' "${log}" || {
        echo "${mode} did not shut down cleanly: ${log}" >&2; exit 1;
    }
    awk -v round="${round}" -v mode="${mode}" '
        $1 == "[camera]" && $3 == "fps" {
            fps=$2+0; loop=crop=age=-1
            for (i=1;i<=NF;i++) {
                if ($i=="loop") loop=$(i+1)+0
                if ($i=="crop") crop=$(i+1)+0
                if ($i=="age" && $(i+1)=="mean") age=$(i+2)+0
            }
            if (loop>=0 && crop>=0 && age>=0) {
                n++; sfps+=fps; sloop+=loop; scrop+=crop; sage+=age+loop
            }
        }
        $1 == "[crop-profile]" && $2 == "per" {
            for (i=1;i<=NF;i++) if ($i=="markers") { smarkers+=$(i+1)+0; marker_n++ }
        }
        $1 == "[adaptive]" && $2 == "per" {
            for (i=1;i<=NF;i++) {
                if ($i=="attempts") low+=$(i+1)+0
                if ($i=="hits") hits+=$(i+1)+0
                if ($i=="fallback") fallback+=$(i+1)+0
                if ($i=="audit") audit+=$(i+1)+0
                if ($i=="deferred") deferred+=$(i+1)+0
            }
        }
        $1 == "[input]" && $2 == "detector" && $3 == "processed" {
            processed=$4+0; source=$6+0
        }
        END {
            if (!n) exit 2
            printf "%d\t%s\t%d\t%.2f\t%.2f\t%.2f\t%.2f\t%.3f\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", \
                round,mode,n,sfps/n,sloop/n,scrop/n,sage/n, \
                marker_n?smarkers/marker_n:0,low,hits,fallback,audit,deferred,processed,source
        }' "${log}" >>"${RESULTS}" || { echo "no valid windows: ${log}" >&2; exit 1; }
}

round=1
while [ "${round}" -le "${REPEATS}" ]; do
    if [ $((round % 2)) -eq 1 ]; then
        run_case baseline "${round}" 0
        run_case adaptive "${round}" 1
    else
        run_case adaptive "${round}" 1
        run_case baseline "${round}" 0
    fi
    round=$((round+1))
done
cat "${RESULTS}"
echo "Raw logs and TSV: ${OUT_DIR}"
