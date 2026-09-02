#!/usr/bin/env bash
# Deterministic 86R.5C.3-S2 terminal intersection gate.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build_root=${P4_AUDIO86_PCM_TERMINAL_BUILD_ROOT:-"${repo_root}/firmware"}
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/p4-audio86-pcm-terminal.XXXXXX")
trap 'rm -rf "${work_dir}"' EXIT
readonly esp_emu="${ESP_EMU:-${HOME}/.local/bin/esp-emu}"

cd "${repo_root}"
[[ -x "${esp_emu}" ]] || { echo "esp-emu executable not found" >&2; exit 1; }
python3 tools/emu/test_p4_audio86_pcm_terminal_validator.py
# shellcheck source=tools/emu/activate-idf.sh
source tools/emu/activate-idf.sh

merge_image() {
    local source_dir=$1 output=$2
    python3 "${IDF_PATH}/components/esptool_py/esptool/esptool.py" \
        --chip esp32p4 merge_bin --output "${output}" --format raw \
        --flash_mode dio --flash_freq 80m --flash_size 8MB \
        --fill-flash-size 8MB \
        0x2000 "${source_dir}/bootloader/bootloader.bin" \
        0x8000 "${source_dir}/partition_table/partition-table.bin" \
        0x10000 "${source_dir}/esp_np2kai.bin" >/dev/null
}

run_case() {
    local scenario=$1 image=$2 log=$3 batch=${4:-}
    local batch_args=()
    [[ -z "${batch}" ]] || batch_args=(--batch-size "${batch}")
    timeout --foreground 25s "${esp_emu}" --chip esp32p4 \
        --firmware "${image}" --exit-on 'main_task: Returned from app_main()' \
        --timeout 20s "${batch_args[@]}" --log-color never >"${log}" 2>&1
    python3 tools/emu/validate_p4_audio86_pcm_terminal_log.py \
        --scenario "${scenario}" --log "${log}" >/dev/null
}

scenarios=(
    reset-full-stop reset-full-fatal reset-full-consumer-fatal
    partial-stop partial-fatal partial-consumer-fatal
    post-done-consumer-fatal finish-fatal
)
for scenario in "${scenarios[@]}"; do
    # A single build tree preserves the expensive IDF object graph; each
    # selector still causes CMake to rebuild and relink the profile-dependent
    # objects before its immutable merged image is copied into work_dir.
    build_dir="${build_root}/build-p4-v3x-audio86-pcm-reset-full-stop"
    tools/emu/build-production.sh --variant p4-v3x --board generic \
        --audio86-pcm-lifecycle "${scenario}" --esp-emu-test \
        --build-dir "${build_dir}"
    image="${work_dir}/${scenario}.bin"
    merge_image "${build_dir}" "${image}"
    first_evidence=""
    for run in $(seq 1 20); do
        log="${work_dir}/${scenario}-${run}.log"
        run_case "${scenario}" "${image}" "${log}"
        evidence=$(rg '^(P4_AUDIO86_PCM_LIFECYCLE |P4_AUDIO86_PCM_S2_CUTPOINT |P4_AUDIO86_PCM_ACCOUNTING |P4_AUDIO86_PCM_RESET_TERMINAL |P4_AUDIO86_PCM_FINISH_TERMINAL |P4_AUDIO86_REAL_GUEST_RESULT=|P4_NANO_AUDIO86_REAL_GUEST_STATUS=)' \
            "${log}" | sha256sum | awk '{print $1}')
        if [[ -z "${first_evidence}" ]]; then
            first_evidence=${evidence}
        elif [[ "${evidence}" != "${first_evidence}" ]]; then
            echo "ERROR: S2 evidence differs: ${scenario} run ${run}" >&2
            exit 1
        fi
    done
    printf '%s_REPEAT=20/20_PASS\n' "$(tr '[:lower:]-' '[:upper:]_' <<<"${scenario}")"
done

for scenario in reset-full-consumer-fatal partial-consumer-fatal post-done-consumer-fatal; do
    image="${work_dir}/${scenario}.bin"
    for batch in 1000 50000 500000; do
        run_case "${scenario}" "${image}" \
            "${work_dir}/${scenario}-batch-${batch}.log" "${batch}"
    done
done

printf '5C3S2_SCHEDULER_PERTURBATION=PASS\n'
printf 'RESET_FULL_RING_CUTPOINT=PASS\n'
printf 'RESET_REQUIRES_PRE_PCM_RING_DURABILITY=PASS\n'
printf 'RESET_FULL_STOP=PASS\n'
printf 'RESET_FULL_FATAL_HEALTHY_DRAIN=PASS\n'
printf 'RESET_FULL_CONSUMER_FATAL=PASS\n'
printf 'RESET_FORCED_ABORT_RESIDUAL_MODEL=PASS\n'
printf 'SUBPREFILL_PARTIAL_CUTPOINT=PASS\n'
printf 'SUBPREFILL_PARTIAL_STOP=PASS\n'
printf 'SUBPREFILL_PARTIAL_FATAL_HEALTHY_DRAIN=PASS\n'
printf 'SUBPREFILL_PARTIAL_CONSUMER_FATAL=PASS\n'
printf 'PARTIAL_FORCED_ABORT_ACCOUNTING=PASS\n'
printf 'POST_PCM_DONE_RESIDUAL_CUTPOINT=PASS\n'
printf 'POST_PCM_DONE_RESIDUAL_CONSUMER_FATAL=PASS\n'
printf 'LATE_FAILURE_NOT_MASKED_BY_PCM_DONE=PASS\n'
printf 'FINISH_FATAL_PRE_ACK_CUTPOINT=PASS\n'
printf 'SINK_FINISH_FATAL=PASS\n'
printf 'FINISH_FATAL_TERMINALIZATION=PASS\n'
printf 'PCM_TOTAL_ACCOUNTING_IDENTITY=PASS\n'
printf 'PCM_ABANDONMENT_CLASSES_DISJOINT=PASS\n'
printf 'RESET_TERMINAL_RESIDUALS=PASS\n'
printf '5C3S2_FAILURE_MATRIX=8/8_PASS\n'
printf '5C3S2_VALIDATOR=PASS\n'
printf '5C3S2_VALIDATOR_MUTATIONS=20_ALL_REJECTED\n'

# Each named gate below is backed by the per-run validator, repeatability
# comparison, or scheduler-perturbation executions above. Keep the explicit
# list so additions and removals change the formal matrix count mechanically.
s2_gates=(
    scope accounting_identity accounting_bytes abandonment_disjoint
    reset_cutpoint_occupancy reset_cutpoint_partial reset_cutpoint_rendered_tail
    reset_guest_linearized reset_worker_not_applied_at_cutpoint
    reset_stop_no_forced_abort reset_stop_no_abandonment reset_stop_worker_applied
    reset_stop_transport_closed reset_stop_first_error_zero
    reset_fatal_first_error reset_fatal_first_error_immutable
    reset_fatal_no_forced_abort reset_fatal_no_abandonment reset_fatal_worker_applied
    reset_consumer_forced_abort reset_consumer_publication_order
    reset_consumer_worker_not_applied reset_consumer_ack_not_published
    reset_consumer_abandoned reset_consumer_event_residual_closed
    reset_consumer_horizon_residual_closed reset_consumer_transport_residual_closed
    reset_consumer_published_accounted reset_consumer_rendered_tail_accounted
    partial_cutpoint_occupancy partial_cutpoint_nonzero_partial
    partial_stop_final_partial partial_stop_no_padding partial_stop_no_abandonment
    partial_stop_prefill_release partial_fatal_first_error
    partial_fatal_final_partial partial_fatal_no_abandonment
    partial_consumer_forced_abort partial_consumer_partial_not_published
    partial_consumer_partial_accounted partial_consumer_accepted_unchanged
    partial_consumer_no_tail_advance partial_identity_nonzero_p
    post_done_cutpoint_done post_done_cutpoint_residual
    post_done_consumer_forced_abort post_done_consumer_published_accounted
    post_done_consumer_accepted_unchanged post_done_no_success_eos
    finish_cutpoint_done finish_cutpoint_empty finish_cutpoint_pre_ack
    finish_fatal_first_error finish_fatal_no_success_ack finish_fatal_not_finished
    finish_fatal_no_abandonment finish_fatal_abort_once
    reset_full_stop_repeat reset_full_fatal_repeat reset_full_consumer_fatal_repeat
    partial_stop_repeat partial_fatal_repeat partial_consumer_fatal_repeat
    post_done_consumer_fatal_repeat finish_fatal_repeat
    scheduler_reset_consumer scheduler_partial_consumer scheduler_post_done_consumer
    validator_mutations
)
for gate in "${s2_gates[@]}"; do
    printf '86R5C3S2_GATE=%s:PASS\n' "${gate}"
done
(( ${#s2_gates[@]} >= 50 ))
printf '86R5C3S2_MATRIX=%d/%d_PASS\n' \
    "${#s2_gates[@]}" "${#s2_gates[@]}"
