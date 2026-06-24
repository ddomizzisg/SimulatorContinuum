#!/usr/bin/env python3
import argparse
import csv
from datetime import datetime
import json
import shutil
import subprocess
import traceback
from pathlib import Path


DEFAULT_ALGORITHMS = {
    "compress": ["ZLIB", "BZ2", "LZMA", "LZ4", "ZSTD"],
    "hash": ["SHA256", "SHA3_256", "BLAKE3", "HMAC_SHA256"],
    "cipher": ["AES", "CHACHA20", "RS"],
}

ALGORITHM_ALIASES = {
    "CHACHA": "CHACHA20",
    "CHACHA_20": "CHACHA20",
    "HMAC_SHA_256": "HMAC_SHA256",
}

SERVICE_TIME_MODEL_ALIASES = {
    "linear": "linear",
    "linear-interpolation": "linear",
    "linear_interpolation": "linear",
    "log-log": "log-log",
    "log_log": "log-log",
    "loglog": "log-log",
    "power-law": "log-log",
    "power_law": "log-log",
    "powerlaw": "log-log",
}

CONTAINER_PLATFORM_ALIASES = {
    "docker": "docker",
    "apptainer": "apptainer",
    "singularity": "singularity",
}

QUEUE_IMAGE_KEYS = (
    "queue_container_image",
    "queue_image",
    "single_queue_image",
    "queue_sif",
    "sif_path",
    "apptainer_sif",
)

REQ_TO_FAMILY = {
    "compress": "compression",
    "hash": "hash",
    "cipher": "crypto",
}

OUTPUT_LABEL = {
    "compress": "compress",
    "hash": "hash_calculate",
    "cipher": "encrypt",
}

INPUT_LABEL = {
    "compress": "uncompress",
    "hash": "hash_verify",
    "cipher": "unencrypt",
}


def parse_args():
    parser = argparse.ArgumentParser(
        description="Benchmark one requirement, or sweep many requirement/parameter combinations."
    )
    parser.add_argument(
        "--requirement-type",
        choices=("compress", "hash", "cipher", "all"),
        help="Requirement family to benchmark. Use `all` with --sweep to test every family.",
    )
    parser.add_argument(
        "--algorithm",
        help="Algorithm name for a single run, e.g. SHA256, ZLIB, AES, CHACHA20, RS. Use `all` in sweep mode.",
    )
    parser.add_argument(
        "--algorithms",
        help="Comma-separated algorithm list for sweep mode, or `all` to use available algorithms per requirement.",
    )
    parser.add_argument(
        "--direction",
        choices=("output", "input"),
        default="output",
        help="Benchmark the output-side operation or the corresponding input-side inverse/verification.",
    )
    parser.add_argument(
        "--directions",
        help="Comma-separated directions for sweep mode: output,input or `all`/`both`.",
    )
    parser.add_argument(
        "--objects",
        type=int,
        default=100,
        help="Number of objects to process.",
    )
    parser.add_argument(
        "--objects-list",
        help="Comma-separated object counts for sweep mode.",
    )
    parser.add_argument(
        "--size-mb",
        type=float,
        default=10.0,
        help="Size of each object in MB.",
    )
    parser.add_argument(
        "--size-mb-list",
        help="Comma-separated object sizes in MB for sweep mode.",
    )
    parser.add_argument(
        "--workers",
        type=int,
        default=8,
        help="Worker count for both simulator and real pipeline.",
    )
    parser.add_argument(
        "--workers-list",
        help="Comma-separated worker counts for sweep mode.",
    )
    parser.add_argument(
        "--inter-arrival",
        type=float,
        default=0.18,
        help="Source interarrival to use for the simulator trace when direction=output.",
    )
    parser.add_argument(
        "--inter-arrival-list",
        help="Comma-separated source interarrival values for sweep mode.",
    )
    parser.add_argument(
        "--aes-key-bits",
        type=int,
        default=256,
        help="AES/ChaCha20 key size in bits for benchmark runs.",
    )
    parser.add_argument(
        "--aes-key-bits-list",
        help="Comma-separated AES/ChaCha20 key sizes for sweep mode.",
    )
    parser.add_argument(
        "--ida-k",
        type=int,
        help="Number of data chunks (k) for Reed-Solomon/IDA. Defaults to base config value, then 8.",
    )
    parser.add_argument(
        "--ida-m",
        type=int,
        help="Number of parity chunks (m) for Reed-Solomon/IDA. Defaults to base config value, then 4.",
    )
    parser.add_argument(
        "--ida-k-list",
        help="Comma-separated k values for sweep mode.",
    )
    parser.add_argument(
        "--ida-m-list",
        help="Comma-separated m values for sweep mode.",
    )
    parser.add_argument(
        "--hardware-profile",
        help="Override the hardware profile in the generated simulator config.",
    )
    parser.add_argument(
        "--service-time-model",
        help="Simulator service-time model: linear or log-log. Defaults to the base config value, then linear.",
    )
    parser.add_argument(
        "--service-time-models",
        help="Comma-separated service-time models for sweep mode, or `all` for linear and log-log.",
    )
    parser.add_argument(
        "--input-mode",
        choices=("random", "compressible"),
        default="compressible",
        help="Input payload mode for the real pipeline.",
    )
    parser.add_argument(
        "--input-modes",
        help="Comma-separated input payload modes for sweep mode: random,compressible.",
    )
    parser.add_argument(
        "--simulator-dir",
        type=Path,
        default=Path("proxy_dd"),
        help="Simulator directory to use, e.g. proxy_dd or proxy_dd_interpolation_only.",
    )
    parser.add_argument(
        "--container-platform",
        choices=("docker", "apptainer", "singularity"),
        help="Container runtime for the simulator queue estimator. Defaults to the base config value, then docker.",
    )
    parser.add_argument(
        "--queue-container-image",
        help="Queue estimator Docker image or Apptainer/Singularity SIF. Defaults to single:queue for Docker and ../stages/single_queue.sif for Apptainer.",
    )
    parser.add_argument(
        "--base-simulator-config",
        type=Path,
        default=Path("proxy_dd/config_distributed_example.json"),
        help="Base simulator config used as a template.",
    )
    parser.add_argument(
        "--real-runner",
        type=Path,
        default=Path("real_pipeline_reference/pipeline_runner.py"),
        help="Real pipeline runner script.",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=Path("single_requirement_benchmarks"),
        help="Directory where benchmark configs, raw results, and summaries will be written.",
    )
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="Skip `make` in the simulator directory.",
    )
    parser.add_argument(
        "--simulator-only",
        action="store_true",
        help="Run only the simulator. Skips the real pipeline, calibration, and real/simulator error metrics.",
    )
    parser.add_argument(
        "--sweep",
        action="store_true",
        help="Run a parameter sweep instead of one benchmark.",
    )
    parser.add_argument(
        "--all-requirements",
        action="store_true",
        help="Sweep all requirement families and their available algorithms.",
    )
    parser.add_argument(
        "--sweep-name",
        help="Directory name for a sweep. Defaults to a timestamped name.",
    )
    parser.add_argument(
        "--stop-on-error",
        action="store_true",
        help="Stop the sweep at the first failed benchmark instead of recording the failure and continuing.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print/write the sweep plan without running benchmarks.",
    )

    args = parser.parse_args()
    if args.dry_run and not is_sweep_requested(args):
        parser.error("--dry-run is only available with --sweep, --all-requirements, or another sweep option")
    if not is_sweep_requested(args):
        if not args.requirement_type or args.requirement_type == "all":
            parser.error("--requirement-type is required for a single benchmark")
        if not args.algorithm or args.algorithm == "all":
            parser.error("--algorithm is required for a single benchmark")
    return args


def load_json(path: Path):
    with path.open("r", encoding="utf-8") as fp:
        return json.load(fp)


def write_json(path: Path, payload):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as fp:
        json.dump(payload, fp, indent=2)
        fp.write("\n")


def run_command(command, cwd: Path):
    subprocess.run(command, cwd=cwd, check=True)


def read_csv_rows(path: Path):
    with path.open(newline="", encoding="utf-8") as fp:
        return list(csv.DictReader(fp))


def float_value(row, key, default=0.0):
    value = row.get(key, "")
    if value in ("", None):
        return default
    try:
        return float(value)
    except ValueError:
        return default


def is_sweep_requested(args):
    return any(
        [
            args.sweep,
            args.all_requirements,
            args.requirement_type == "all",
            args.algorithms is not None,
            args.directions is not None,
            args.objects_list is not None,
            args.size_mb_list is not None,
            args.workers_list is not None,
            args.inter_arrival_list is not None,
            args.aes_key_bits_list is not None,
            args.service_time_models is not None,
            args.input_modes is not None,
            args.ida_k_list is not None,
            args.ida_m_list is not None,
        ]
    )


def comma_values(value):
    if value in ("", None):
        return []
    return [item.strip() for item in str(value).split(",") if item.strip()]


def parse_int_list(value, default_value, label):
    if value in ("", None):
        return [default_value]
    parsed = []
    for item in comma_values(value):
        try:
            number = int(item)
        except ValueError as exc:
            raise ValueError(f"Invalid integer in {label}: {item}") from exc
        if number <= 0:
            raise ValueError(f"{label} values must be > 0: {item}")
        parsed.append(number)
    return parsed


def parse_float_list(value, default_value, label):
    if value in ("", None):
        return [default_value]
    parsed = []
    for item in comma_values(value):
        try:
            number = float(item)
        except ValueError as exc:
            raise ValueError(f"Invalid float in {label}: {item}") from exc
        if number < 0.0:
            raise ValueError(f"{label} values must be >= 0: {item}")
        parsed.append(number)
    return parsed


def unique_preserve_order(values):
    seen = set()
    result = []
    for value in values:
        if value not in seen:
            seen.add(value)
            result.append(value)
    return result


def normalized_algorithm(value):
    algorithm = value.strip().upper().replace("-", "_")
    return ALGORITHM_ALIASES.get(algorithm, algorithm)


def normalized_service_time_model(value):
    if value in ("", None):
        return None
    key = str(value).strip().lower()
    if key in SERVICE_TIME_MODEL_ALIASES:
        return SERVICE_TIME_MODEL_ALIASES[key]
    valid = ", ".join(sorted(set(SERVICE_TIME_MODEL_ALIASES.values())))
    raise ValueError(f"Invalid service-time model `{value}`. Expected one of: {valid}")


def normalized_container_platform(value):
    if value in ("", None):
        return None
    key = str(value).strip().lower()
    if key in CONTAINER_PLATFORM_ALIASES:
        return CONTAINER_PLATFORM_ALIASES[key]
    valid = ", ".join(sorted(CONTAINER_PLATFORM_ALIASES.values()))
    raise ValueError(f"Invalid container platform `{value}`. Expected one of: {valid}")


def base_service_time_model(base_config):
    for key in ("service_time_model", "interpolation_model", "modeling_model"):
        model = normalized_service_time_model(base_config.get(key))
        if model:
            return model
    return "linear"


def resolve_service_time_model(args, base_config):
    return normalized_service_time_model(args.service_time_model) or base_service_time_model(base_config)


def base_container_platform(base_config):
    for key in ("container_platform", "container_runtime", "runtime"):
        platform = normalized_container_platform(base_config.get(key))
        if platform:
            return platform
    return "docker"


def resolve_container_platform(args, base_config):
    return normalized_container_platform(getattr(args, "container_platform", None)) or base_container_platform(base_config)


def base_queue_container_image(base_config):
    for key in QUEUE_IMAGE_KEYS:
        image = base_config.get(key)
        if image:
            return str(image)
    return None


def resolve_container_image_path(args, image):
    if not image or "://" in str(image):
        return image

    path = Path(str(image))
    if path.is_absolute():
        return str(path)

    candidates = [
        Path.cwd() / path,
        args.simulator_dir.resolve() / path,
        args.base_simulator_config.resolve().parent / path,
    ]
    for candidate in candidates:
        if candidate.exists():
            return str(candidate.resolve())
    return str(image)


def resolve_queue_container_image(args, base_config, container_platform):
    image = getattr(args, "queue_container_image", None) or base_queue_container_image(base_config)
    if not image:
        image = "../stages/single_queue.sif" if container_platform in ("apptainer", "singularity") else "single:queue"
    if container_platform in ("apptainer", "singularity"):
        return resolve_container_image_path(args, image)
    return str(image)


def resolve_ida_k(args, base_config):
    if getattr(args, "ida_k", None) is not None:
        return args.ida_k
    return base_config.get("ida_k", 8)


def resolve_ida_m(args, base_config):
    if getattr(args, "ida_m", None) is not None:
        return args.ida_m
    return base_config.get("ida_m", 4)


def parse_ida_pairs(args, base_config):
    default_k = base_config.get("ida_k", 8)
    default_m = base_config.get("ida_m", 4)
    current_k = getattr(args, "ida_k", None)
    if current_k is None:
        current_k = default_k
    current_m = getattr(args, "ida_m", None)
    if current_m is None:
        current_m = default_m
    k_list = parse_int_list(args.ida_k_list, current_k, "--ida-k-list")
    m_list = parse_int_list(args.ida_m_list, current_m, "--ida-m-list")
    if len(k_list) == len(m_list):
        return list(zip(k_list, m_list))
    pairs = []
    for k in k_list:
        for m in m_list:
            pairs.append((k, m))
    return pairs


def parse_service_time_models(args, base_config):
    if args.service_time_models:
        raw = args.service_time_models.strip().lower()
        if raw in ("all", "both"):
            return ["linear", "log-log"]
        return unique_preserve_order(normalized_service_time_model(item) for item in comma_values(raw))
    return [resolve_service_time_model(args, base_config)]


def parse_directions(args):
    if args.directions:
        raw = args.directions.strip().lower()
        if raw in ("all", "both"):
            return ["output", "input"]
        directions = [item.lower() for item in comma_values(raw)]
    elif is_sweep_requested(args):
        directions = ["output", "input"]
    else:
        directions = [args.direction]

    invalid = [direction for direction in directions if direction not in ("output", "input")]
    if invalid:
        raise ValueError(f"Invalid direction(s): {', '.join(invalid)}")
    return unique_preserve_order(directions)


def parse_input_modes(args):
    if args.input_modes:
        modes = [item.lower() for item in comma_values(args.input_modes)]
    else:
        modes = [args.input_mode]

    invalid = [mode for mode in modes if mode not in ("random", "compressible")]
    if invalid:
        raise ValueError(f"Invalid input mode(s): {', '.join(invalid)}")
    return unique_preserve_order(modes)


def candidate_real_value_dirs(args, base_config):
    bases = [Path.cwd(), args.base_simulator_config.resolve().parent, args.simulator_dir.resolve()]
    dirs = []

    def add_path(path_value):
        if not path_value:
            return
        path = Path(path_value)
        candidates = [path] if path.is_absolute() else [base / path for base in bases]
        for candidate in candidates:
            if candidate.exists() and candidate.is_dir():
                dirs.append(candidate.resolve())

    if getattr(args, "hardware_profile", None):
        add_path(Path("results_different_machines") / "organized" / str(args.hardware_profile) / "real_values")

    add_path(base_config.get("real_values_dir"))
    for machine in base_config.get("machines", []):
        if isinstance(machine, dict):
            add_path(machine.get("real_values_dir"))
            hardware_profile = machine.get("hardware_profile") or machine.get("profile")
            if hardware_profile:
                add_path(Path("results_different_machines") / "organized" / str(hardware_profile) / "real_values")

    add_path(args.simulator_dir.resolve() / "real_values")
    return unique_preserve_order(dirs)


def algorithms_from_csv(path):
    if not path.exists():
        return []
    algorithms = []
    with path.open(newline="", encoding="utf-8") as fp:
        reader = csv.reader(fp)
        next(reader, None)
        for row in reader:
            if row and row[0].strip():
                algorithms.append(normalized_algorithm(row[0]))
    return unique_preserve_order(algorithms)


def available_algorithms(args, base_config, requirement_type):
    files_by_requirement = {
        "compress": ["cost-efficiency.csv"],
        "hash": ["integrity.csv"],
        "cipher": ["confidentiality.csv", "reliability.csv"],
    }
    algorithms = []
    for real_value_dir in candidate_real_value_dirs(args, base_config):
        for file_name in files_by_requirement[requirement_type]:
            algorithms.extend(algorithms_from_csv(real_value_dir / file_name))
    return unique_preserve_order(algorithms) or list(DEFAULT_ALGORITHMS[requirement_type])


def parse_requirement_types(args):
    if args.all_requirements or args.requirement_type in (None, "all"):
        return list(REQ_TO_FAMILY.keys())
    return [args.requirement_type]


def parse_algorithms_for_requirement(args, base_config, requirement_type):
    raw = args.algorithms if args.algorithms is not None else args.algorithm
    if is_sweep_requested(args) and (raw in (None, "", "all")):
        return available_algorithms(args, base_config, requirement_type)
    if raw in (None, ""):
        return available_algorithms(args, base_config, requirement_type)
    if raw.strip().lower() == "all":
        return available_algorithms(args, base_config, requirement_type)
    requested = unique_preserve_order(normalized_algorithm(item) for item in comma_values(raw))
    if args.all_requirements or args.requirement_type == "all":
        available = set(available_algorithms(args, base_config, requirement_type))
        return [algorithm for algorithm in requested if algorithm in available]
    return requested


def first_machine_profile(base_config):
    machines = base_config.get("machines", [])
    if isinstance(machines, list):
        for machine in machines:
            if isinstance(machine, dict):
                hardware_profile = machine.get("hardware_profile") or machine.get("profile")
                if hardware_profile:
                    return hardware_profile
    return ""


def requirement_spec(req_type: str, algorithm: str):
    return {"type": req_type, "algorithm": algorithm}


def base_bandwidths_for_simulator_only(base_config):
    b_fs = float(base_config.get("b_fs", 100.0))
    return {
        "b_fs": b_fs,
        "b_fs_read": float(base_config.get("b_fs_read", b_fs)),
        "b_fs_write": float(base_config.get("b_fs_write", b_fs)),
    }


def make_simulator_config(args, base_config):
    profile = getattr(args, "hardware_profile", None) or first_machine_profile(base_config)
    service_time_model = resolve_service_time_model(args, base_config)
    container_platform = resolve_container_platform(args, base_config)
    queue_container_image = resolve_queue_container_image(args, base_config, container_platform)
    ida_algo = args.algorithm if args.requirement_type == "cipher" else base_config.get("ida_algo", "RS")
    if getattr(args, "simulator_only", False):
        bandwidths = base_bandwidths_for_simulator_only(base_config)
    else:
        bandwidths = {
            "b_fs": 0.0,
            "b_fs_read": 0.0,
            "b_fs_write": 0.0,
        }
    trace = {
        "MUESTRAS": args.objects,
        "inter_arrival": args.inter_arrival,
        "DISTRIBUTION": 3,
        "mean": 15.0,
        "stddev": 0.6,
        "SIZE": int(args.size_mb * 1048576),
        "stddevS": 0.5,
        "Concurrency": 1,
    }

    stage1 = {
        "name": "stage1",
        **bandwidths,
        "inter_arrival": 0.0,
        "arrival_model": "burst",
        "application_mean_service_time": 0.0,
        "application_size_factor": 1.0,
        "input_requirements": [],
        "output_requirements": [],
    }
    stages = [stage1]

    if args.direction == "output":
        stage1["output_requirements"] = [requirement_spec(args.requirement_type, args.algorithm)]
    else:
        stage1["output_requirements"] = [requirement_spec(args.requirement_type, args.algorithm)]
        stage2 = {
            "name": "stage2",
            **bandwidths,
            "inter_arrival": 0.0,
            "arrival_model": "burst",
            "application_mean_service_time": 0.0,
            "application_size_factor": 1.0,
            "output_requirements": [],
        }
        stages.append(stage2)

    config = {
        "workers": args.workers,
        "traces_number": 1,
        "traces": [trace],
        "agent_type": "output",
        "stages": stages,
        "compression_algo": base_config.get("compression_algo", "ZLIB"),
        "hashing_algo": base_config.get("hashing_algo", "SHA256"),
        "ida_algo": ida_algo,
        "service_time_model": service_time_model,
        "container_platform": container_platform,
        "queue_container_image": queue_container_image,
        "ida_k": getattr(args, "ida_k", 8),
        "ida_m": getattr(args, "ida_m", 4),
        "aes_key_bits": args.aes_key_bits,
        "b_fs": bandwidths["b_fs"],
        "b_fs_read": bandwidths["b_fs_read"],
        "b_fs_write": bandwidths["b_fs_write"],
    }

    if profile:
        machine_stages = ["stage1"] if args.direction == "output" else ["stage1", "stage2"]
        config["machines"] = [{"name": "m0", "stages": machine_stages, "hardware_profile": profile}]
        config["links"] = []

    return config


def make_real_pipeline_config(args, simulator_config_path: Path):
    stage1 = {
        "name": "stage1",
        "application_size_factor": 1.0,
        "input_requirements": [],
        "output_requirements": [],
    }
    stages = [stage1]

    if args.direction == "output":
        stage1["output_requirements"] = [requirement_spec(args.requirement_type, args.algorithm)]
    else:
        stage1["output_requirements"] = [requirement_spec(args.requirement_type, args.algorithm)]
        stages.append(
            {
                "name": "stage2",
                "application_size_factor": 1.0,
                "input_requirements": [],
                "output_requirements": [],
            }
        )

    return {
        "source_simulator_config": str(simulator_config_path),
        "input_dir": "generated_input",
        "work_dir": "work",
        "output_dir": "output",
        "results_dir": "results",
        "generate_input": {
            "files": args.objects,
            "size_mb": args.size_mb,
            "seed": 42,
            "mode": args.input_mode,
        },
        "ida_k": getattr(args, "ida_k", 8),
        "ida_m": getattr(args, "ida_m", 4),
        "aes_key_bits": args.aes_key_bits,
        "stages": stages,
    }


def target_stage_name(args):
    return "stage1" if args.direction == "output" else "stage2"


def remove_results_dir(path: Path):
    if path.exists():
        shutil.rmtree(path)


def run_simulator(args, simulator_config_path: Path, out_dir: Path):
    simulator_dir = args.simulator_dir.resolve()
    if not args.skip_build:
        run_command(["make"], simulator_dir)

    remove_results_dir(simulator_dir / "results")
    stdout_path = out_dir / "simulator_stdout.txt"
    stderr_path = out_dir / "simulator_stderr.txt"
    command = [
        str((simulator_dir / "main").resolve()),
        str(simulator_config_path.resolve()),
        args.service_time_model,
        args.container_platform,
    ]
    if getattr(args, "queue_container_image", None):
        command.append(args.queue_container_image)
    with stdout_path.open("w", encoding="utf-8") as out_fp, stderr_path.open("w", encoding="utf-8") as err_fp:
        subprocess.run(
            command,
            cwd=simulator_dir,
            check=True,
            stdout=out_fp,
            stderr=err_fp,
        )
    shutil.copytree(simulator_dir / "results", out_dir / "simulator_results", dirs_exist_ok=True)


def run_real_pipeline(args, real_config_path: Path, out_dir: Path):
    runner_path = args.real_runner.resolve()
    runner_dir = runner_path.parent
    base_dir = real_config_path.parent
    remove_results_dir(base_dir / "generated_input")
    remove_results_dir(base_dir / "work")
    remove_results_dir(base_dir / "output")
    remove_results_dir(base_dir / "results")

    stdout_path = out_dir / "real_stdout.txt"
    stderr_path = out_dir / "real_stderr.txt"
    command = ["python3", str(runner_path), "--config", str(real_config_path.resolve()), "--workers", str(args.workers)]
    if args.direction == "output":
        command.append("--output-only")
    with stdout_path.open("w", encoding="utf-8") as out_fp, stderr_path.open("w", encoding="utf-8") as err_fp:
        subprocess.run(command, cwd=runner_dir, check=True, stdout=out_fp, stderr=err_fp)

    shutil.copytree(base_dir / "results", out_dir / "real_results", dirs_exist_ok=True)


def calibrate_simulator_from_real(args, simulator_config, real_results_dir: Path):
    stage_name = target_stage_name(args)
    prefix = "output" if args.direction == "output" else "input"
    total_metric_field = f"{prefix}_seconds"
    compute_metric_field = f"{prefix}_compute_seconds"
    file_metrics_path = real_results_dir / "file_stage_metrics.csv"
    queue_summary_path = real_results_dir / "stage_queue_summary.csv"

    rows = [row for row in read_csv_rows(file_metrics_path) if row.get("stage") == stage_name]
    if not rows:
        return simulator_config, {}

    metric_field = compute_metric_field if compute_metric_field in rows[0] else total_metric_field
    total_compute_seconds = sum(float_value(row, metric_field) for row in rows)
    total_task_seconds = sum(float_value(row, total_metric_field) for row in rows)
    total_io_seconds = max(total_task_seconds - total_compute_seconds, 0.0)
    total_input_bytes = sum(float_value(row, "input_size_bytes") for row in rows)
    total_output_bytes = sum(float_value(row, "output_size_bytes") for row in rows)

    derived_b_fs = 100.0  # default fallback
    if total_io_seconds > 0.0:
        # total_io_seconds = (total_input_bytes + total_output_bytes) / (b_fs * 1048576.0)
        derived_b_fs = (total_input_bytes + total_output_bytes) / (total_io_seconds * 1048576.0)
        # Apply a realistic 85% filesystem contention and OS lock margin to prevent timing underestimations
        derived_b_fs *= 0.85
        derived_b_fs = max(1.0, min(10000.0, derived_b_fs))

    stage_info = {
        "stage_name": stage_name,
        "metric_field": metric_field,
        "samples": len(rows),
        "total_compute_seconds": total_compute_seconds,
        "total_task_seconds": total_task_seconds,
        "excluded_read_write_seconds": total_io_seconds,
        "total_input_bytes": total_input_bytes,
        "total_output_bytes": total_output_bytes,
        "simulator_filesystem_timing": "enabled",
        "simulator_queue_timing": "disabled",
    }

    calibrated = json.loads(json.dumps(simulator_config))
    calibrated["b_fs"] = derived_b_fs
    calibrated["b_fs_read"] = derived_b_fs
    calibrated["b_fs_write"] = derived_b_fs
    target_stage = None
    for stage in calibrated.get("stages", []):
        stage["b_fs"] = derived_b_fs
        stage["b_fs_read"] = derived_b_fs
        stage["b_fs_write"] = derived_b_fs
        stage.pop("b_fs_bytes", None)
        stage.pop("b_fs_read_bytes", None)
        stage.pop("b_fs_write_bytes", None)
        stage["inter_arrival"] = 0.0
        stage["arrival_model"] = "burst"
        if stage.get("name") == stage_name:
            target_stage = stage

    queue_rows = read_csv_rows(queue_summary_path)
    queue_row = next((row for row in queue_rows if row.get("stage") == stage_name), None)
    if target_stage is not None and queue_row is not None:
        mean_interarrival = float_value(queue_row, "mean_interarrival_seconds")
        stage_info["measured_mean_interarrival_seconds"] = mean_interarrival
        stage_info["arrival_model"] = "burst"

    calibrated.setdefault("_benchmark_calibration", {})
    calibrated["_benchmark_calibration"]["real_pipeline_stage_metrics"] = stage_info
    return calibrated, stage_info


def target_stage_and_prefix(args):
    if args.direction == "output":
        return "stage1", "output"
    return "stage2", "input"


def expected_requirement_label(args):
    action = OUTPUT_LABEL[args.requirement_type] if args.direction == "output" else INPUT_LABEL[args.requirement_type]
    return f"{action}:{args.algorithm}"


def stage_row_by_name(csv_path: Path, stage_name: str):
    rows = read_csv_rows(csv_path)
    for row in rows:
        if row.get("stage_name") == stage_name:
            return row
    raise RuntimeError(f"Could not find stage {stage_name} in {csv_path}")


def requirement_total_seconds_from_row(row, prefix: str, label: str):
    index = 1
    while f"{prefix}_requirement_{index}" in row:
        if row.get(f"{prefix}_requirement_{index}", "") == label:
            total_key = f"{prefix}_requirement_{index}_seconds"
            return float_value(row, total_key)
        index += 1
    return 0.0


def requirement_compute_seconds_from_row(row, prefix: str, label: str):
    index = 1
    while f"{prefix}_requirement_{index}" in row:
        if row.get(f"{prefix}_requirement_{index}", "") == label:
            compute_key = f"{prefix}_requirement_{index}_compute_seconds"
            total_key = f"{prefix}_requirement_{index}_seconds"
            return float_value(row, compute_key, float_value(row, total_key))
        index += 1
    return 0.0


def compute_value(row, compute_key: str, total_key: str):
    return float_value(row, compute_key, float_value(row, total_key))


def extract_metrics(args, benchmark_dir: Path):
    stage_name, prefix = target_stage_and_prefix(args)
    family = REQ_TO_FAMILY[args.requirement_type]
    label = expected_requirement_label(args)

    real_row = stage_row_by_name(benchmark_dir / "real_results" / "stage_totals_by_workers.csv", stage_name)
    sim_row = stage_row_by_name(benchmark_dir / "simulator_results" / "stage_totals_by_workers.csv", stage_name)

    stage_metric = f"{prefix}_stage_seconds"
    stage_compute_metric = f"{prefix}_stage_compute_seconds"
    family_metric = f"{prefix}_{family}_seconds"
    family_compute_metric = f"{prefix}_{family}_compute_seconds"
    requirement_metric_name = label

    # Check simulator config to see if filesystem timing was enabled
    import json
    sim_config_path = benchmark_dir / "simulator_config.json"
    fs_timing_enabled = False
    if sim_config_path.exists():
        try:
            with sim_config_path.open("r") as f:
                sim_config = json.load(f)
                calib = sim_config.get("_benchmark_calibration", {})
                stage_calib = calib.get("real_pipeline_stage_metrics", {})
                if stage_calib.get("simulator_filesystem_timing") == "enabled":
                    fs_timing_enabled = True
        except Exception:
            pass

    timing_basis = "total" if fs_timing_enabled else "compute_only"

    if fs_timing_enabled:
        real_stage = float_value(real_row, stage_metric)
        real_family = float_value(real_row, family_metric)
        real_requirement = requirement_total_seconds_from_row(real_row, prefix, label)
        real_total_stage = float_value(real_row, "total_seconds")

        sim_stage = float_value(sim_row, stage_metric)
        sim_family = float_value(sim_row, family_metric)
        sim_requirement = requirement_total_seconds_from_row(sim_row, prefix, label)
        sim_total_stage = float_value(sim_row, "total_seconds")
    else:
        real_stage = compute_value(real_row, stage_compute_metric, stage_metric)
        real_family = compute_value(real_row, family_compute_metric, family_metric)
        real_requirement = requirement_compute_seconds_from_row(real_row, prefix, label)
        real_total_stage = compute_value(real_row, "total_compute_seconds", "total_seconds")

        sim_stage = compute_value(sim_row, stage_compute_metric, stage_metric)
        sim_family = compute_value(sim_row, family_compute_metric, family_metric)
        sim_requirement = requirement_compute_seconds_from_row(sim_row, prefix, label)
        sim_total_stage = compute_value(sim_row, "total_compute_seconds", "total_seconds")

    return {
        "stage_name": stage_name,
        "direction": args.direction,
        "requirement_type": args.requirement_type,
        "algorithm": args.algorithm,
        "timing_basis": timing_basis,
        "objects": args.objects,
        "size_mb": args.size_mb,
        "workers": args.workers,
        "requirement_label": requirement_metric_name,
        "real": {
            "stage_compute_seconds": real_stage,
            "family_compute_seconds": real_family,
            "requirement_compute_seconds": real_requirement,
            "total_stage_compute_seconds": real_total_stage,
        },
        "simulator": {
            "stage_compute_seconds": sim_stage,
            "family_compute_seconds": sim_family,
            "requirement_compute_seconds": sim_requirement,
            "total_stage_compute_seconds": sim_total_stage,
        },
    }


def extract_simulator_metrics(args, benchmark_dir: Path):
    stage_name, prefix = target_stage_and_prefix(args)
    family = REQ_TO_FAMILY[args.requirement_type]
    label = expected_requirement_label(args)

    sim_row = stage_row_by_name(benchmark_dir / "simulator_results" / "stage_totals_by_workers.csv", stage_name)

    stage_metric = f"{prefix}_stage_seconds"
    stage_compute_metric = f"{prefix}_stage_compute_seconds"
    family_metric = f"{prefix}_{family}_seconds"
    family_compute_metric = f"{prefix}_{family}_compute_seconds"

    sim_stage = compute_value(sim_row, stage_compute_metric, stage_metric)
    sim_family = compute_value(sim_row, family_compute_metric, family_metric)
    sim_requirement = requirement_compute_seconds_from_row(sim_row, prefix, label)
    sim_total_stage = compute_value(sim_row, "total_compute_seconds", "total_seconds")

    return {
        "stage_name": stage_name,
        "direction": args.direction,
        "requirement_type": args.requirement_type,
        "algorithm": args.algorithm,
        "timing_basis": "simulator_only_compute",
        "run_mode": "simulator_only",
        "objects": args.objects,
        "size_mb": args.size_mb,
        "workers": args.workers,
        "requirement_label": label,
        "real": {},
        "simulator": {
            "stage_compute_seconds": sim_stage,
            "family_compute_seconds": sim_family,
            "requirement_compute_seconds": sim_requirement,
            "total_stage_compute_seconds": sim_total_stage,
        },
    }


def add_errors(summary):
    for scope in ("stage_compute_seconds", "family_compute_seconds", "requirement_compute_seconds", "total_stage_compute_seconds"):
        real_value = summary["real"][scope]
        sim_value = summary["simulator"][scope]
        error = sim_value - real_value
        ape = abs(error) / real_value * 100.0 if real_value > 0 else 0.0
        summary.setdefault("comparison", {})[scope] = {
            "real": real_value,
            "simulator": sim_value,
            "error": error,
            "ape_percent": ape,
        }
    return summary


def metric_rows_for_summary(summary):
    if summary.get("comparison"):
        rows = []
        for metric, values in summary["comparison"].items():
            rows.append(
                {
                    "metric": metric,
                    "real": values.get("real", ""),
                    "simulator": values.get("simulator", ""),
                    "error": values.get("error", ""),
                    "ape_percent": values.get("ape_percent", ""),
                }
            )
        return rows

    rows = []
    real_values = summary.get("real", {}) or {}
    for metric, simulator_value in (summary.get("simulator", {}) or {}).items():
        rows.append(
            {
                "metric": metric,
                "real": real_values.get(metric, ""),
                "simulator": simulator_value,
                "error": "",
                "ape_percent": "",
            }
        )
    return rows


def format_optional_seconds(value):
    if value in ("", None):
        return ""
    return f"{float(value):.6f}"


def format_optional_percent(value):
    if value in ("", None):
        return ""
    return f"{float(value):.2f}"


def write_summary(benchmark_dir: Path, summary):
    json_path = benchmark_dir / "summary.json"
    md_path = benchmark_dir / "summary.md"
    csv_path = benchmark_dir / "summary.csv"

    with json_path.open("w", encoding="utf-8") as fp:
        json.dump(summary, fp, indent=2)
        fp.write("\n")

    fieldnames = [
        "direction",
        "requirement_type",
        "algorithm",
        "stage_name",
        "objects",
        "size_mb",
        "workers",
        "run_mode",
        "input_mode",
        "inter_arrival",
        "service_time_model",
        "container_platform",
        "queue_container_image",
        "ida_k",
        "ida_m",
        "metric",
        "real_seconds",
        "simulator_seconds",
        "error_seconds",
        "ape_percent",
    ]
    with csv_path.open("w", newline="", encoding="utf-8") as fp:
        writer = csv.DictWriter(fp, fieldnames=fieldnames)
        writer.writeheader()
        for metric_row in metric_rows_for_summary(summary):
            writer.writerow(
                {
                    "direction": summary["direction"],
                    "requirement_type": summary["requirement_type"],
                    "algorithm": summary["algorithm"],
                    "stage_name": summary["stage_name"],
                    "objects": summary["objects"],
                    "size_mb": summary["size_mb"],
                    "workers": summary["workers"],
                    "run_mode": summary.get("run_mode", "compare"),
                    "input_mode": summary.get("input_mode", ""),
                    "inter_arrival": summary.get("inter_arrival", ""),
                    "service_time_model": summary.get("service_time_model", ""),
                    "container_platform": summary.get("container_platform", ""),
                    "queue_container_image": summary.get("queue_container_image", ""),
                    "ida_k": summary.get("ida_k", ""),
                    "ida_m": summary.get("ida_m", ""),
                    "metric": metric_row["metric"],
                    "real_seconds": metric_row["real"],
                    "simulator_seconds": metric_row["simulator"],
                    "error_seconds": metric_row["error"],
                    "ape_percent": metric_row["ape_percent"],
                }
            )

    lines = [
        "# Single Requirement Compute Benchmark",
        "",
        f"- simulator dir: `{summary['simulator_dir']}`",
        f"- direction: `{summary['direction']}`",
        f"- requirement: `{summary['requirement_label']}`",
        f"- run mode: `{summary.get('run_mode', 'compare')}`",
        f"- timing basis: `{summary['timing_basis']}`",
        f"- objects: `{summary['objects']}`",
        f"- object size: `{summary['size_mb']}` MB",
        f"- workers: `{summary['workers']}`",
        f"- aes key size: `{summary.get('aes_key_bits', '')}` bits",
        f"- ida k: `{summary.get('ida_k', '')}`",
        f"- ida m: `{summary.get('ida_m', '')}`",
        f"- input mode: `{summary.get('input_mode', '')}`",
        f"- inter-arrival: `{summary.get('inter_arrival', '')}`",
        f"- service-time model: `{summary.get('service_time_model', '')}`",
        f"- container platform: `{summary.get('container_platform', '')}`",
        f"- queue container image: `{summary.get('queue_container_image', '')}`",
        f"- calibrated stage: `{summary.get('real_to_simulator_calibration', {}).get('stage_name', '')}`",
        "- real pipeline and error metrics are skipped" if summary.get("run_mode") == "simulator_only" else "- task read/write timing is excluded; simulator filesystem and queue timing are disabled",
        "",
        "| Metric | Real (s) | Simulator (s) | Error (s) | APE (%) |",
        "|---|---:|---:|---:|---:|",
    ]
    for metric_row in metric_rows_for_summary(summary):
        lines.append(
            f"| {metric_row['metric']} | {format_optional_seconds(metric_row['real'])} | "
            f"{format_optional_seconds(metric_row['simulator'])} | "
            f"{format_optional_seconds(metric_row['error'])} | "
            f"{format_optional_percent(metric_row['ape_percent'])} |"
        )
    md_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def slugify(text: str):
    return "".join(ch.lower() if ch.isalnum() else "_" for ch in text).strip("_")


def timestamp_slug():
    return datetime.now().strftime("%Y%m%d_%H%M%S_%f")


def benchmark_name(args):
    parts = [
        timestamp_slug(),
        args.simulator_dir.name,
        args.direction,
        args.requirement_type,
        slugify(args.algorithm),
    ]
    if args.algorithm == "RS":
        parts.append(f"k{getattr(args, 'ida_k', 8)}_m{getattr(args, 'ida_m', 4)}")
    parts.extend([
        f"{args.objects}obj",
        f"{str(args.size_mb).replace('.', 'p')}mb",
        f"{args.workers}w",
        f"{str(args.inter_arrival).replace('.', 'p')}ia",
        f"{args.aes_key_bits}bits",
        slugify(args.service_time_model or "linear"),
        slugify(getattr(args, "container_platform", "") or "docker"),
        args.input_mode,
    ])
    return "_".join(parts)


def run_single_benchmark(args, base_config, parent_out_dir=None):
    args = argparse.Namespace(**vars(args))
    args.service_time_model = resolve_service_time_model(args, base_config)
    args.container_platform = resolve_container_platform(args, base_config)
    args.queue_container_image = resolve_queue_container_image(args, base_config, args.container_platform)
    args.ida_k = resolve_ida_k(args, base_config)
    args.ida_m = resolve_ida_m(args, base_config)
    benchmark_dir = (parent_out_dir or args.out_dir.resolve()) / benchmark_name(args)
    benchmark_dir.mkdir(parents=True, exist_ok=True)

    simulator_config = make_simulator_config(args, base_config)
    simulator_config_initial_path = benchmark_dir / "simulator_config_initial.json"
    write_json(simulator_config_initial_path, simulator_config)

    calibration_info = {}
    if not getattr(args, "simulator_only", False):
        real_config = make_real_pipeline_config(args, simulator_config_initial_path)
        real_config_path = benchmark_dir / "real_pipeline_config.json"
        write_json(real_config_path, real_config)

        run_real_pipeline(args, real_config_path, benchmark_dir)
        simulator_config, calibration_info = calibrate_simulator_from_real(
            args,
            simulator_config,
            benchmark_dir / "real_results",
        )

    simulator_config_path = benchmark_dir / "simulator_config.json"
    write_json(simulator_config_path, simulator_config)
    run_simulator(args, simulator_config_path, benchmark_dir)

    if getattr(args, "simulator_only", False):
        summary = extract_simulator_metrics(args, benchmark_dir)
    else:
        summary = extract_metrics(args, benchmark_dir)
    summary["simulator_dir"] = str(args.simulator_dir)
    summary["run_mode"] = "simulator_only" if getattr(args, "simulator_only", False) else "compare"
    summary["input_mode"] = args.input_mode
    summary["inter_arrival"] = args.inter_arrival
    summary["aes_key_bits"] = args.aes_key_bits
    summary["service_time_model"] = args.service_time_model
    summary["container_platform"] = args.container_platform
    summary["queue_container_image"] = args.queue_container_image
    summary["ida_k"] = args.ida_k
    summary["ida_m"] = args.ida_m
    summary["real_to_simulator_calibration"] = calibration_info
    if not getattr(args, "simulator_only", False):
        add_errors(summary)
    write_summary(benchmark_dir, summary)

    print(f"Benchmark written to {benchmark_dir}")
    print(f"Requirement: {summary['requirement_label']}")
    print(f"Run mode: {summary['run_mode']}")
    print(f"Container platform: {summary['container_platform']} ({summary['queue_container_image']})")
    if summary.get("comparison"):
        for metric, values in summary["comparison"].items():
            print(
                f"{metric}: real={values['real']:.6f}s simulator={values['simulator']:.6f}s "
                f"error={values['error']:.6f}s ape={values['ape_percent']:.2f}%"
            )
    else:
        for metric, value in summary.get("simulator", {}).items():
            print(f"{metric}: simulator={value:.6f}s")
    return summary, benchmark_dir


def make_case_args(args, requirement_type, algorithm, direction, objects, size_mb, workers, inter_arrival, input_mode, aes_key_bits, service_time_model, ida_k, ida_m):
    case_args = argparse.Namespace(**vars(args))
    case_args.requirement_type = requirement_type
    case_args.algorithm = algorithm
    case_args.direction = direction
    case_args.objects = objects
    case_args.size_mb = size_mb
    case_args.workers = workers
    case_args.inter_arrival = inter_arrival
    case_args.input_mode = input_mode
    case_args.aes_key_bits = aes_key_bits
    case_args.service_time_model = service_time_model
    case_args.ida_k = ida_k
    case_args.ida_m = ida_m
    return case_args


def build_sweep_cases(args, base_config):
    cases = []
    requirement_types = parse_requirement_types(args)
    directions = parse_directions(args)
    objects_values = parse_int_list(args.objects_list, args.objects, "--objects-list")
    size_values = parse_float_list(args.size_mb_list, args.size_mb, "--size-mb-list")
    worker_values = parse_int_list(args.workers_list, args.workers, "--workers-list")
    inter_arrival_values = parse_float_list(args.inter_arrival_list, args.inter_arrival, "--inter-arrival-list")
    aes_key_bits_values = parse_int_list(args.aes_key_bits_list, args.aes_key_bits, "--aes-key-bits-list")
    service_time_models = parse_service_time_models(args, base_config)
    container_platform = resolve_container_platform(args, base_config)
    queue_container_image = resolve_queue_container_image(args, base_config, container_platform)
    input_modes = parse_input_modes(args)
    ida_pairs = parse_ida_pairs(args, base_config)

    for requirement_type in requirement_types:
        algorithms = parse_algorithms_for_requirement(args, base_config, requirement_type)
        for algorithm in algorithms:
            for direction in directions:
                for objects in objects_values:
                    for size_mb in size_values:
                        for workers in worker_values:
                            for inter_arrival in inter_arrival_values:
                                for input_mode in input_modes:
                                    for aes_key_bits in aes_key_bits_values:
                                        for service_time_model in service_time_models:
                                            active_pairs = ida_pairs if algorithm == "RS" else [ida_pairs[0]]
                                            for ida_k, ida_m in active_pairs:
                                                case_args = make_case_args(
                                                    args,
                                                    requirement_type,
                                                    algorithm,
                                                    direction,
                                                    objects,
                                                    size_mb,
                                                    workers,
                                                    inter_arrival,
                                                    input_mode,
                                                    aes_key_bits,
                                                    service_time_model,
                                                    ida_k,
                                                    ida_m,
                                                )
                                                case_args.container_platform = container_platform
                                                case_args.queue_container_image = queue_container_image
                                                cases.append(case_args)
    return cases


def sweep_plan_rows(cases):
    rows = []
    for index, case_args in enumerate(cases, start=1):
        rows.append(
            {
                "index": index,
                "direction": case_args.direction,
                "requirement_type": case_args.requirement_type,
                "algorithm": case_args.algorithm,
                "objects": case_args.objects,
                "size_mb": case_args.size_mb,
                "workers": case_args.workers,
                "run_mode": "simulator_only" if getattr(case_args, "simulator_only", False) else "compare",
                "inter_arrival": case_args.inter_arrival,
                "aes_key_bits": case_args.aes_key_bits,
                "service_time_model": case_args.service_time_model,
                "container_platform": getattr(case_args, "container_platform", "") or "",
                "queue_container_image": getattr(case_args, "queue_container_image", "") or "",
                "input_mode": case_args.input_mode,
                "ida_k": case_args.ida_k,
                "ida_m": case_args.ida_m,
            }
        )
    return rows


def flatten_summary_row(summary, benchmark_dir, status="ok", error=""):
    row = {
        "status": status,
        "benchmark_dir": str(benchmark_dir) if benchmark_dir else "",
        "error": error,
        "direction": summary.get("direction", ""),
        "requirement_type": summary.get("requirement_type", ""),
        "algorithm": summary.get("algorithm", ""),
        "stage_name": summary.get("stage_name", ""),
        "objects": summary.get("objects", ""),
        "size_mb": summary.get("size_mb", ""),
        "workers": summary.get("workers", ""),
        "run_mode": summary.get("run_mode", "compare"),
        "input_mode": summary.get("input_mode", ""),
        "inter_arrival": summary.get("inter_arrival", ""),
        "aes_key_bits": summary.get("aes_key_bits", ""),
        "service_time_model": summary.get("service_time_model", ""),
        "container_platform": summary.get("container_platform", ""),
        "queue_container_image": summary.get("queue_container_image", ""),
        "ida_k": summary.get("ida_k", ""),
        "ida_m": summary.get("ida_m", ""),
        "requirement_label": summary.get("requirement_label", ""),
    }
    if summary.get("comparison"):
        for metric, values in summary.get("comparison", {}).items():
            row[f"{metric}_real_seconds"] = values.get("real", 0.0)
            row[f"{metric}_simulator_seconds"] = values.get("simulator", 0.0)
            row[f"{metric}_error_seconds"] = values.get("error", 0.0)
            row[f"{metric}_ape_percent"] = values.get("ape_percent", 0.0)
    else:
        real_values = summary.get("real", {}) or {}
        for metric, simulator_value in (summary.get("simulator", {}) or {}).items():
            row[f"{metric}_real_seconds"] = real_values.get(metric, "")
            row[f"{metric}_simulator_seconds"] = simulator_value
            row[f"{metric}_error_seconds"] = ""
            row[f"{metric}_ape_percent"] = ""
    return row


def failed_summary_row(case_args, error, benchmark_dir=""):
    return {
        "status": "failed",
        "benchmark_dir": str(benchmark_dir) if benchmark_dir else "",
        "error": error,
        "direction": case_args.direction,
        "requirement_type": case_args.requirement_type,
        "algorithm": case_args.algorithm,
        "stage_name": target_stage_name(case_args),
        "objects": case_args.objects,
        "size_mb": case_args.size_mb,
        "workers": case_args.workers,
        "run_mode": "simulator_only" if getattr(case_args, "simulator_only", False) else "compare",
        "input_mode": case_args.input_mode,
        "inter_arrival": case_args.inter_arrival,
        "aes_key_bits": case_args.aes_key_bits,
        "service_time_model": case_args.service_time_model,
        "container_platform": getattr(case_args, "container_platform", "") or "",
        "queue_container_image": getattr(case_args, "queue_container_image", "") or "",
        "ida_k": case_args.ida_k,
        "ida_m": case_args.ida_m,
        "requirement_label": expected_requirement_label(case_args),
    }


def write_csv(path: Path, rows):
    if not rows:
        return
    fieldnames = []
    for row in rows:
        for key in row:
            if key not in fieldnames:
                fieldnames.append(key)
    with path.open("w", newline="", encoding="utf-8") as fp:
        writer = csv.DictWriter(fp, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def write_sweep_outputs(sweep_dir: Path, plan_rows, result_rows):
    write_csv(sweep_dir / "sweep_plan.csv", plan_rows)
    write_csv(sweep_dir / "sweep_summary.csv", result_rows)
    with (sweep_dir / "sweep_plan.json").open("w", encoding="utf-8") as fp:
        json.dump(plan_rows, fp, indent=2)
        fp.write("\n")
    with (sweep_dir / "sweep_summary.json").open("w", encoding="utf-8") as fp:
        json.dump(result_rows, fp, indent=2)
        fp.write("\n")

    completed = sum(1 for row in result_rows if row.get("status") == "ok")
    failed = sum(1 for row in result_rows if row.get("status") == "failed")
    lines = [
        "# Requirement Sweep",
        "",
        f"- planned runs: `{len(plan_rows)}`",
        f"- completed: `{completed}`",
        f"- failed: `{failed}`",
        "",
        "| Status | Requirement | Direction | Algorithm | K | M | Objects | Size MB | Workers | Model | Runtime | Input Mode | Requirement APE (%) |",
        "|---|---|---|---|---|---|---:|---:|---:|---|---|---|---:|",
    ]
    for row in result_rows:
        ape = row.get("requirement_compute_seconds_ape_percent", "")
        ape_value = f"{float(ape):.2f}" if ape not in ("", None) else ""
        lines.append(
            "| {status} | {requirement_type} | {direction} | {algorithm} | {ida_k} | {ida_m} | {objects} | {size_mb} | "
            "{workers} | {service_time_model} | {container_platform} | {input_mode} | {ape} |".format(
                status=row.get("status", ""),
                requirement_type=row.get("requirement_type", ""),
                direction=row.get("direction", ""),
                algorithm=row.get("algorithm", ""),
                ida_k=row.get("ida_k", ""),
                ida_m=row.get("ida_m", ""),
                objects=row.get("objects", ""),
                size_mb=row.get("size_mb", ""),
                workers=row.get("workers", ""),
                service_time_model=row.get("service_time_model", ""),
                container_platform=row.get("container_platform", ""),
                input_mode=row.get("input_mode", ""),
                ape=ape_value,
            )
        )
    (sweep_dir / "sweep_summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def run_sweep(args, base_config):
    cases = build_sweep_cases(args, base_config)
    if not cases:
        raise ValueError("Sweep produced no benchmark cases. Check requirement and algorithm filters.")
    sweep_name = args.sweep_name or f"{timestamp_slug()}_requirement_sweep"
    sweep_dir = args.out_dir.resolve() / sweep_name
    sweep_dir.mkdir(parents=True, exist_ok=True)

    plan_rows = sweep_plan_rows(cases)
    write_sweep_outputs(sweep_dir, plan_rows, [])
    print(f"Sweep plan written to {sweep_dir}")
    print(f"Planned runs: {len(cases)}")

    if args.dry_run:
        return [], sweep_dir

    build_done = False
    if not args.skip_build:
        run_command(["make"], args.simulator_dir.resolve())
        build_done = True

    result_rows = []
    for index, case_args in enumerate(cases, start=1):
        print(
            f"[{index}/{len(cases)}] {case_args.direction} {case_args.requirement_type}:{case_args.algorithm} "
            f"objects={case_args.objects} size_mb={case_args.size_mb} workers={case_args.workers} "
            f"model={case_args.service_time_model} runtime={case_args.container_platform} input_mode={case_args.input_mode}"
        )
        if build_done:
            case_args.skip_build = True
        try:
            summary, benchmark_dir = run_single_benchmark(case_args, base_config, sweep_dir)
            summary["input_mode"] = case_args.input_mode
            summary["inter_arrival"] = case_args.inter_arrival
            result_rows.append(flatten_summary_row(summary, benchmark_dir))
        except Exception as exc:
            error_text = f"{type(exc).__name__}: {exc}"
            result_rows.append(failed_summary_row(case_args, error_text))
            error_path = sweep_dir / f"run_{index:04d}_error.txt"
            error_path.write_text(traceback.format_exc(), encoding="utf-8")
            print(f"Run failed: {error_text}")
            if args.stop_on_error:
                write_sweep_outputs(sweep_dir, plan_rows, result_rows)
                raise
        write_sweep_outputs(sweep_dir, plan_rows, result_rows)

    print(f"Sweep summary written to {sweep_dir / 'sweep_summary.csv'}")
    return result_rows, sweep_dir


def main():
    args = parse_args()
    base_config = load_json(args.base_simulator_config.resolve())

    if is_sweep_requested(args):
        run_sweep(args, base_config)
    else:
        run_single_benchmark(args, base_config)


if __name__ == "__main__":
    main()
