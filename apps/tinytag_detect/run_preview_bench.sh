#!/bin/sh
# Order-balanced single-core comparison of detector-only, colour RTSP, and
# exact-luma RTSP. Raw logs and a machine-readable TSV are retained.
set -eu

APP_DIR="${TINYTAG_BENCH_APP_DIR:-/app/tinytag_detect}"
RUN_LIVE="${TINYTAG_BENCH_RUN_LIVE:-${APP_DIR}/run_live.sh}"
DURATION="${TINYTAG_BENCH_DURATION:-12}"
REPEATS="${TINYTAG_BENCH_REPEATS:-2}"
# Production preview defaults to nice 10. Set this to 0 for an A/B baseline.
PREVIEW_NICE="${TINYTAG_BENCH_PREVIEW_NICE:-10}"
OUT_DIR="${TINYTAG_BENCH_OUT_DIR:-/tmp/tinytag-preview-bench-$(date +%Y%m%d-%H%M%S)}"
RESULTS="${OUT_DIR}/windows.tsv"

die()
{
    echo "Error: $*" >&2
    exit 1
}

is_uint()
{
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
        *) return 0 ;;
    esac
}

is_uint "${DURATION}" || die "TINYTAG_BENCH_DURATION must be an integer"
is_uint "${REPEATS}" || die "TINYTAG_BENCH_REPEATS must be an integer"
is_uint "${PREVIEW_NICE}" || die "TINYTAG_BENCH_PREVIEW_NICE must be 0..19"
[ "${DURATION}" -ge 5 ] || die "benchmark duration must be at least 5 seconds"
[ "${REPEATS}" -ge 1 ] || die "benchmark repeats must be at least 1"
[ "${PREVIEW_NICE}" -le 19 ] || die "TINYTAG_BENCH_PREVIEW_NICE must be 0..19"
[ -x "${RUN_LIVE}" ] || die "launcher is not executable: ${RUN_LIVE}"
command -v timeout >/dev/null 2>&1 || die "timeout is required"

mkdir -p "${OUT_DIR}"
printf 'round\tmode\twindows\tfps\tloop_ms\tdet_cpu_ms\tproc_cpu_ms\tother_cpu_ms\tcore_pct\tacq_age_ms\tresult_age_ms\trelease_ms\tcrop_ms\n' >"${RESULTS}"

parse_log()
{
    awk -v round="$1" -v mode="$2" '
    $1 == "[camera]" && $3 == "fps" && ($2 + 0) >= 25 {
        fps = $2 + 0
        loop = det = proc = release = crop = age = -1
        for (i = 1; i <= NF; ++i) {
            if ($i == "loop") loop = $(i + 1) + 0
            else if ($i == "cpu") det = $(i + 1) + 0
            else if ($i == "proc-cpu") proc = $(i + 1) + 0
            else if ($i == "release") release = $(i + 1) + 0
            else if ($i == "crop") crop = $(i + 1) + 0
            else if ($i == "age" && $(i + 1) == "mean") age = $(i + 2) + 0
        }
        if (loop >= 0 && det >= 0 && proc >= 0 && release >= 0 && crop >= 0 && age >= 0) {
            n++
            sum_fps += fps
            sum_loop += loop
            sum_det += det
            sum_proc += proc
            sum_release += release
            sum_crop += crop
            sum_age += age
        }
    }
    END {
        if (n == 0) exit 2
        fps = sum_fps / n
        loop = sum_loop / n
        det = sum_det / n
        proc = sum_proc / n
        other = proc - det
        if (other < 0 && other > -0.01) other = 0
        age = sum_age / n
        printf "%d\t%s\t%d\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.1f\t%.2f\t%.2f\t%.2f\t%.2f\n", \
               round, mode, n, fps, loop, det, proc, other, proc * fps / 10.0, \
               age, age + loop, sum_release / n, sum_crop / n
    }' "$3"
}

run_case()
{
    mode="$1"
    round="$2"
    option="$3"
    log="${OUT_DIR}/round-${round}-${mode}.log"

    echo "== round ${round}/${REPEATS}: ${mode} (${DURATION}s, preview nice=${PREVIEW_NICE}) =="
    rc=0
    TINYTAG_LIVE_PREVIEW_NICE="${PREVIEW_NICE}" \
        timeout -s INT "${DURATION}" "${RUN_LIVE}" "${option}" \
        </dev/null >"${log}" 2>&1 || rc=$?
    case "${rc}" in
        0|124) ;;
        *) die "${mode} exited ${rc}; inspect ${log}" ;;
    esac

    grep -q '^\[camera\] stopping' "${log}" ||
        die "${mode} did not shut down cleanly; inspect ${log}"
    row="$(parse_log "${round}" "${mode}" "${log}")" ||
        die "${mode} has no >=25 fps camera windows with proc-cpu; inspect ${log}"
    printf '%s\n' "${row}" >>"${RESULTS}"
    printf '%s\n' "${row}" | awk -F '\t' \
        '{printf "   fps %.2f  loop %.2f ms  detector CPU %.2f ms  process CPU %.2f ms  result age %.2f ms\n", $4, $5, $6, $7, $11}'
}

round=1
while [ "${round}" -le "${REPEATS}" ]; do
    if [ $((round % 2)) -eq 1 ]; then
        run_case no-rtsp "${round}" --no-rtsp
        run_case colour "${round}" --rtsp
        run_case luma "${round}" --rtsp-luma
    else
        run_case luma "${round}" --rtsp-luma
        run_case colour "${round}" --rtsp
        run_case no-rtsp "${round}" --no-rtsp
    fi
    round=$((round + 1))
done

echo
echo "Per-mode means across order-balanced rounds:"
awk -F '\t' '
NR == 1 { next }
{
    mode = $2
    n[mode]++
    for (i = 4; i <= 13; ++i) sum[mode, i] += $i
}
END {
    printf "%-9s %7s %8s %8s %8s %8s %8s %8s %8s %8s %8s\n", \
           "mode", "fps", "loop", "detCPU", "procCPU", "otherCPU", "core%", "acqAge", "resultAge", "release", "crop"
    order[1] = "no-rtsp"; order[2] = "colour"; order[3] = "luma"
    for (j = 1; j <= 3; ++j) {
        mode = order[j]
        if (!n[mode]) continue
        printf "%-9s %7.2f %8.2f %8.2f %8.2f %8.2f %8.1f %8.2f %8.2f %8.2f %8.2f\n", \
               mode, sum[mode,4]/n[mode], sum[mode,5]/n[mode], \
               sum[mode,6]/n[mode], sum[mode,7]/n[mode], \
               sum[mode,8]/n[mode], sum[mode,9]/n[mode], \
               sum[mode,10]/n[mode], sum[mode,11]/n[mode], \
               sum[mode,12]/n[mode], sum[mode,13]/n[mode]
    }
}' "${RESULTS}"

echo
echo "process CPU includes every application thread; other CPU = process - detector."
echo "core% is process CPU consumption as a percentage of one core."
echo "result age is the software approximation: acquisition age + detector loop wall time."
echo "Raw logs and ${RESULTS} are retained under ${OUT_DIR}."
