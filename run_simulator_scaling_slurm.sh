#!/bin/bash
#SBATCH --job-name=sim_scaling
#SBATCH --output=slurm_logs/sim_scaling_%j.out
#SBATCH --error=slurm_logs/sim_scaling_%j.err
#SBATCH --time=12:00:00
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=64
#SBATCH --mem=32G
#SBATCH --partition=compute

# ==============================================================================
# Slurm Job Script for Simulator-Only Scaling Benchmarks
# ==============================================================================
#
# Runs proxy_dd/main directly across increasing object counts, object sizes, and
# worker counts. This does not run the real workflow.
#
# Usage:
#   sbatch run_simulator_scaling_slurm.sh
#
# Common overrides:
#   OBJECTS_LIST="1 10 100 1000 10000 100000" \
#   SIZE_MB_LIST="1 10 100" \
#   WORKERS_LIST="1 2 4 8 16 32" \
#   REPEATS=3 \
#   sbatch run_simulator_scaling_slurm.sh
#
# Dry run:
#   DRY_RUN=1 bash run_simulator_scaling_slurm.sh
# ==============================================================================

set -u
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" &> /dev/null && pwd)"
cd "$SCRIPT_DIR" || exit 1

mkdir -p slurm_logs

BASE_CONFIG="${BASE_CONFIG:-proxy_dd/config_distributed_example.json}"
SIMULATOR_DIR="${SIMULATOR_DIR:-proxy_dd}"
SIMULATOR_EXE="${SIMULATOR_EXE:-main}"
OUT_DIR="${OUT_DIR:-simulator_scaling_results}"

# Powers of ten by default. Override OBJECTS_LIST for larger or custom sweeps.
OBJECTS_LIST="${OBJECTS_LIST:-1 10 100 1000 10000 100000}"
SIZE_MB_LIST="${SIZE_MB_LIST:-1 10 100 1000}"
WORKERS_LIST="${WORKERS_LIST:-1 2 4 8 16 32 64 128}"
REPEATS="${REPEATS:-1}"

SERVICE_TIME_MODEL="${SERVICE_TIME_MODEL:-linear}"
CONTAINER_PLATFORM="${CONTAINER_PLATFORM:-}"
QUEUE_CONTAINER_IMAGE="${QUEUE_CONTAINER_IMAGE:-}"
STOP_ON_ERROR="${STOP_ON_ERROR:-0}"
SKIP_BUILD="${SKIP_BUILD:-0}"
DRY_RUN="10"

normalize_list() {
    printf "%s" "${1//,/ }"
}

if [ -z "$CONTAINER_PLATFORM" ]; then
    if command -v apptainer >/dev/null 2>&1; then
        CONTAINER_PLATFORM="apptainer"
    elif command -v singularity >/dev/null 2>&1; then
        CONTAINER_PLATFORM="singularity"
    else
        CONTAINER_PLATFORM="docker"
    fi
fi

if [ "$CONTAINER_PLATFORM" = "apptainer" ] || [ "$CONTAINER_PLATFORM" = "singularity" ]; then
    QUEUE_CONTAINER_IMAGE="${QUEUE_CONTAINER_IMAGE:-$SCRIPT_DIR/stages/single_queue.sif}"
    if [ ! -f "$QUEUE_CONTAINER_IMAGE" ]; then
        echo "Queue container image not found: $QUEUE_CONTAINER_IMAGE"
        echo "Set QUEUE_CONTAINER_IMAGE to a valid .sif path or use CONTAINER_PLATFORM=docker."
        exit 1
    fi
else
    QUEUE_CONTAINER_IMAGE="${QUEUE_CONTAINER_IMAGE:-single:queue}"
fi

if [[ "$SIMULATOR_DIR" = /* ]]; then
    SIMULATOR_DIR_PATH="$SIMULATOR_DIR"
else
    SIMULATOR_DIR_PATH="$SCRIPT_DIR/$SIMULATOR_DIR"
fi

if [[ "$SIMULATOR_EXE" = /* ]]; then
    SIMULATOR_PATH="$SIMULATOR_EXE"
else
    SIMULATOR_PATH="$SIMULATOR_DIR_PATH/$SIMULATOR_EXE"
fi

if [[ "$BASE_CONFIG" = /* ]]; then
    BASE_CONFIG_PATH="$BASE_CONFIG"
else
    BASE_CONFIG_PATH="$SCRIPT_DIR/$BASE_CONFIG"
fi
if [[ "$OUT_DIR" = /* ]]; then
    RUN_ROOT="$OUT_DIR/$(date +%Y%m%d_%H%M%S)"
else
    RUN_ROOT="$SCRIPT_DIR/$OUT_DIR/$(date +%Y%m%d_%H%M%S)"
fi
SUMMARY_CSV="$RUN_ROOT/simulator_scaling_summary.csv"

if [ ! -f "$BASE_CONFIG_PATH" ]; then
    echo "Base simulator config not found: $BASE_CONFIG_PATH"
    exit 1
fi

if [ "$SKIP_BUILD" != "1" ] && [ "$DRY_RUN" != "1" ]; then
    echo "Building simulator in $SIMULATOR_DIR ..."
    make -C "$SIMULATOR_DIR_PATH" || exit 1
fi

if [ ! -x "$SIMULATOR_PATH" ] && [ "$DRY_RUN" != "1" ]; then
    echo "Simulator executable not found or not executable: $SIMULATOR_PATH"
    exit 1
fi

read -r -a OBJECTS_VALUES <<< "$(normalize_list "$OBJECTS_LIST")"
read -r -a SIZE_MB_VALUES <<< "$(normalize_list "$SIZE_MB_LIST")"
read -r -a WORKER_VALUES <<< "$(normalize_list "$WORKERS_LIST")"

mkdir -p "$RUN_ROOT"
cat > "$SUMMARY_CSV" <<CSV
timestamp,run_id,repeat,objects,size_mb,size_bytes,workers,status,wall_seconds,simulated_pipeline_seconds,simulated_max_stage_seconds,stage_count,config_path,results_dir,stdout_path,stderr_path
CSV

echo "Starting simulator-only scaling benchmark"
echo "Job ID: ${SLURM_JOB_ID:-local}"
echo "Node: ${SLURMD_NODENAME:-$(hostname)}"
echo "Base config: $BASE_CONFIG_PATH"
echo "Simulator: $SIMULATOR_PATH"
echo "Service-time model: $SERVICE_TIME_MODEL"
echo "Container platform: $CONTAINER_PLATFORM"
echo "Queue image: $QUEUE_CONTAINER_IMAGE"
echo "Objects: ${OBJECTS_VALUES[*]}"
echo "Sizes MB: ${SIZE_MB_VALUES[*]}"
echo "Workers: ${WORKER_VALUES[*]}"
echo "Repeats: $REPEATS"
echo "Output: $RUN_ROOT"
echo "Summary: $SUMMARY_CSV"
echo "-----------------------------------------------------------------"

run_id=0
for objects in "${OBJECTS_VALUES[@]}"; do
    for size_mb in "${SIZE_MB_VALUES[@]}"; do
        size_label="${size_mb//./p}"
        size_bytes="$(python3 -c 'import sys; print(int(float(sys.argv[1]) * 1048576))' "$size_mb")"

        for workers in "${WORKER_VALUES[@]}"; do
            for repeat in $(seq 1 "$REPEATS"); do
                run_id=$((run_id + 1))
                run_dir="$RUN_ROOT/objects_${objects}/size_${size_label}mb/workers_${workers}/run_${repeat}"
                config_path="$run_dir/config.json"
                stdout_path="$run_dir/simulator_stdout.txt"
                stderr_path="$run_dir/simulator_stderr.txt"
                results_dir="$run_dir/results"

                mkdir -p "$run_dir"

                python3 - "$BASE_CONFIG_PATH" "$config_path" "$objects" "$size_mb" "$workers" "$SERVICE_TIME_MODEL" "$CONTAINER_PLATFORM" "$QUEUE_CONTAINER_IMAGE" <<'PY'
import copy
import json
import sys
from pathlib import Path

base_config_path = Path(sys.argv[1])
config_path = Path(sys.argv[2])
objects = int(sys.argv[3])
size_mb = float(sys.argv[4])
workers = int(sys.argv[5])
service_time_model = sys.argv[6]
container_platform = sys.argv[7]
queue_container_image = sys.argv[8]

with base_config_path.open("r", encoding="utf-8") as fp:
    config = json.load(fp)

config = copy.deepcopy(config)
config["workers"] = workers
config["traces_number"] = 1

base_traces = config.get("traces") if isinstance(config.get("traces"), list) else []
if base_traces:
    trace = copy.deepcopy(base_traces[0])
else:
    trace = {
        "MUESTRAS": 100,
        "inter_arrival": 0.18,
        "DISTRIBUTION": 3,
        "mean": 15.0,
        "stddev": 0.6,
        "SIZE": 10485760,
        "stddevS": 0.5,
        "Concurrency": 1,
    }

trace["MUESTRAS"] = objects
trace["SIZE"] = int(size_mb * 1048576)
config["traces"] = [trace]
config.pop("traces_fileName", None)

config["service_time_model"] = service_time_model
config["container_platform"] = container_platform
config["queue_container_image"] = queue_container_image

config_path.parent.mkdir(parents=True, exist_ok=True)
with config_path.open("w", encoding="utf-8") as fp:
    json.dump(config, fp, indent=2)
    fp.write("\n")
PY

                echo "[$run_id] objects=$objects size_mb=$size_mb workers=$workers repeat=$repeat"

                if [ "$DRY_RUN" = "1" ]; then
                    printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n" \
                        "$(date -Iseconds)" "$run_id" "$repeat" "$objects" "$size_mb" "$size_bytes" "$workers" \
                        "dry_run" "" "" "" "" "$config_path" "$results_dir" "$stdout_path" "$stderr_path" >> "$SUMMARY_CSV"
                    continue
                fi

                start_ns="$(date +%s%N)"
                (
                    cd "$run_dir" || exit 1
                    "$SIMULATOR_PATH" "$config_path" "$SERVICE_TIME_MODEL" "$CONTAINER_PLATFORM" "$QUEUE_CONTAINER_IMAGE"
                ) > "$stdout_path" 2> "$stderr_path"
                status=$?
                end_ns="$(date +%s%N)"
                wall_seconds="$(python3 -c 'import sys; print(f"{(int(sys.argv[2]) - int(sys.argv[1])) / 1_000_000_000:.9f}")' "$start_ns" "$end_ns")"

                if [ "$status" -eq 0 ] && [ -f "$results_dir/stage_totals_by_workers.csv" ]; then
                    metrics="$(python3 - "$results_dir/stage_totals_by_workers.csv" <<'PY'
import csv
import sys
from pathlib import Path

path = Path(sys.argv[1])
pipeline_total = 0.0
max_stage = 0.0
stage_count = 0

with path.open(newline="", encoding="utf-8") as fp:
    reader = csv.DictReader(fp)
    for row in reader:
        try:
            total = float(row.get("total_seconds") or 0.0)
        except ValueError:
            total = 0.0
        pipeline_total += total
        max_stage = max(max_stage, total)
        stage_count += 1

print(f"{pipeline_total:.9f},{max_stage:.9f},{stage_count}")
PY
)"
                    IFS=, read -r simulated_pipeline_seconds simulated_max_stage_seconds stage_count <<< "$metrics"
                    run_status="ok"
                else
                    simulated_pipeline_seconds=""
                    simulated_max_stage_seconds=""
                    stage_count=""
                    run_status="failed:$status"
                fi

                printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n" \
                    "$(date -Iseconds)" "$run_id" "$repeat" "$objects" "$size_mb" "$size_bytes" "$workers" \
                    "$run_status" "$wall_seconds" "$simulated_pipeline_seconds" "$simulated_max_stage_seconds" "$stage_count" \
                    "$config_path" "$results_dir" "$stdout_path" "$stderr_path" >> "$SUMMARY_CSV"

                if [ "$status" -ne 0 ]; then
                    echo "Run failed with exit code $status. See $stderr_path"
                    if [ "$STOP_ON_ERROR" = "1" ]; then
                        exit "$status"
                    fi
                fi
            done
        done
    done
done

echo "-----------------------------------------------------------------"
echo "Simulator-only scaling benchmark complete."
echo "Summary written to: $SUMMARY_CSV"
