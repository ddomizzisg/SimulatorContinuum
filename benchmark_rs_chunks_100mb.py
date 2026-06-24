#!/usr/bin/env python3
"""Run a Reed-Solomon chunk-layout sweep for a 100 MB file."""

from __future__ import annotations

import argparse
import shlex
import subprocess
import sys
from pathlib import Path


DEFAULT_DATA_CHUNKS = "4,6,8,10,12,16,32"
DEFAULT_FAULT_TOLERANCE = 3
DEFAULT_DIRECTIONS = "output,input"


def positive_int(value: str) -> int:
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("value must be > 0")
    return number


def positive_float(value: str) -> float:
    number = float(value)
    if number <= 0.0:
        raise argparse.ArgumentTypeError("value must be > 0")
    return number


def nonnegative_float(value: str) -> float:
    number = float(value)
    if number < 0.0:
        raise argparse.ArgumentTypeError("value must be >= 0")
    return number


def parse_pairs(raw_pairs: str) -> list[tuple[int, int]]:
    pairs: list[tuple[int, int]] = []
    for token in raw_pairs.split(","):
        token = token.strip()
        if not token:
            continue
        if ":" not in token:
            raise argparse.ArgumentTypeError(
                f"invalid pair `{token}`; expected data:parity, for example 8:4"
            )
        raw_k, raw_m = token.split(":", 1)
        try:
            k = int(raw_k)
            m = int(raw_m)
        except ValueError as exc:
            raise argparse.ArgumentTypeError(
                f"invalid pair `{token}`; data and parity chunk counts must be integers"
            ) from exc
        if k <= 0 or m <= 0:
            raise argparse.ArgumentTypeError(
                f"invalid pair `{token}`; data and parity chunk counts must be > 0"
            )
        if k + m > 256:
            raise argparse.ArgumentTypeError(
                f"invalid pair `{token}`; zfec requires data + parity chunks <= 256"
            )
        pairs.append((k, m))

    if not pairs:
        raise argparse.ArgumentTypeError("at least one data:parity pair is required")
    return pairs


def parse_data_chunks(raw_values: str) -> list[int]:
    values: list[int] = []
    for token in raw_values.split(","):
        token = token.strip()
        if not token:
            continue
        try:
            k = int(token)
        except ValueError as exc:
            raise argparse.ArgumentTypeError(
                f"invalid data chunk count `{token}`; values must be integers"
            ) from exc
        if k <= 0:
            raise argparse.ArgumentTypeError(
                f"invalid data chunk count `{token}`; values must be > 0"
            )
        values.append(k)

    if not values:
        raise argparse.ArgumentTypeError("at least one data chunk count is required")
    return values


def pairs_for_fault_tolerance(data_chunks: list[int], fault_tolerance: int) -> list[tuple[int, int]]:
    pairs = []
    for k in data_chunks:
        if k + fault_tolerance > 256:
            raise argparse.ArgumentTypeError(
                f"invalid layout k={k}, m={fault_tolerance}; zfec requires data + parity chunks <= 256"
            )
        pairs.append((k, fault_tolerance))
    return pairs


def normalize_directions(raw_directions: str) -> str:
    value = raw_directions.strip().lower()
    if value in ("all", "both"):
        return DEFAULT_DIRECTIONS

    directions: list[str] = []
    for item in value.split(","):
        direction = item.strip()
        if not direction:
            continue
        if direction not in ("output", "input"):
            raise argparse.ArgumentTypeError(
                f"invalid direction `{direction}`; expected output, input, or both"
            )
        if direction not in directions:
            directions.append(direction)

    if not directions:
        raise argparse.ArgumentTypeError("at least one direction is required")
    return ",".join(directions)


def csv(values) -> str:
    return ",".join(str(value) for value in values)


def add_if_present(command: list[str], flag: str, value) -> None:
    if value not in (None, ""):
        command.extend([flag, str(value)])


def parse_args() -> tuple[argparse.Namespace, list[str]]:
    repo_root = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(
        description=(
            "Benchmark Reed-Solomon performance for paired data/parity chunk "
            "layouts on a 100 MB file by delegating to benchmark_single_requirement.py."
        )
    )
    parser.add_argument(
        "--pairs",
        default=None,
        help=(
            "Comma-separated data:parity chunk pairs. Overrides --fault-tolerance "
            "and --data-chunks-list, for example 4:3,8:3,16:3."
        ),
    )
    parser.add_argument(
        "--fault-tolerance",
        type=positive_int,
        default=DEFAULT_FAULT_TOLERANCE,
        help=(
            "Number of chunk failures to tolerate. This becomes parity chunk count m "
            f"when --pairs is not provided. Defaults to {DEFAULT_FAULT_TOLERANCE}."
        ),
    )
    parser.add_argument(
        "--data-chunks-list",
        default=DEFAULT_DATA_CHUNKS,
        help=(
            "Comma-separated data chunk counts k used with --fault-tolerance. "
            f"Defaults to {DEFAULT_DATA_CHUNKS}."
        ),
    )
    parser.add_argument(
        "--directions",
        default=DEFAULT_DIRECTIONS,
        help=(
            "RS operation directions to benchmark: output, input, output,input, "
            "or both. Defaults to output,input."
        ),
    )
    parser.add_argument(
        "--objects",
        type=positive_int,
        default=1,
        help="Number of files to generate. Defaults to 1.",
    )
    parser.add_argument(
        "--size-mb",
        type=positive_float,
        default=100.0,
        help="Size of each generated file in MB. Defaults to 100.0.",
    )
    parser.add_argument(
        "--workers",
        type=positive_int,
        default=8,
        help="Worker count when --workers-list is not provided. Defaults to 8.",
    )
    parser.add_argument(
        "--workers-list",
        help="Optional comma-separated worker counts passed to benchmark_single_requirement.py.",
    )
    parser.add_argument(
        "--hardware-profile",
        help="Hardware profile for the generated simulator config, for example dantelap.",
    )
    parser.add_argument(
        "--service-time-model",
        choices=("linear", "log-log"),
        help="Single simulator service-time model to use.",
    )
    parser.add_argument(
        "--service-time-models",
        help="Comma-separated service-time models, or all, passed to the benchmark sweep.",
    )
    parser.add_argument(
        "--input-mode",
        choices=("random", "compressible"),
        default="random",
        help="Generated input payload mode for the real pipeline. Defaults to random.",
    )
    parser.add_argument(
        "--inter-arrival",
        type=nonnegative_float,
        default=0.18,
        help="Simulator source interarrival used for output-direction traces.",
    )
    parser.add_argument(
        "--aes-key-bits",
        type=positive_int,
        default=256,
        help="Forwarded benchmark parameter kept for compatibility. Defaults to 256.",
    )
    parser.add_argument(
        "--simulator-dir",
        type=Path,
        default=Path("proxy_dd"),
        help="Simulator directory. Defaults to proxy_dd.",
    )
    parser.add_argument(
        "--base-simulator-config",
        type=Path,
        default=Path("proxy_dd/config_distributed_example.json"),
        help="Base simulator config template.",
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
        help="Directory where benchmark results are written.",
    )
    parser.add_argument(
        "--sweep-name",
        default="rs_100mb_ft3_chunk_sweep",
        help="Sweep directory name under --out-dir.",
    )
    parser.add_argument(
        "--container-platform",
        choices=("docker", "apptainer", "singularity"),
        help="Container runtime for the simulator queue estimator.",
    )
    parser.add_argument(
        "--queue-container-image",
        help="Queue estimator Docker image or Apptainer/Singularity SIF.",
    )
    parser.add_argument(
        "--benchmark-script",
        type=Path,
        default=repo_root / "benchmark_single_requirement.py",
        help="Path to benchmark_single_requirement.py.",
    )
    parser.add_argument(
        "--python",
        default=sys.executable,
        help="Python executable used to run benchmark_single_requirement.py.",
    )
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="Forward --skip-build to benchmark_single_requirement.py.",
    )
    parser.add_argument(
        "--simulator-only",
        action="store_true",
        help="Forward --simulator-only to benchmark_single_requirement.py.",
    )
    parser.add_argument(
        "--stop-on-error",
        action="store_true",
        help="Forward --stop-on-error to benchmark_single_requirement.py.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Write the sweep plan without running benchmarks.",
    )
    parser.add_argument(
        "--print-command-only",
        action="store_true",
        help="Print the delegated benchmark command and exit.",
    )
    args, passthrough_args = parser.parse_known_args()
    try:
        if args.pairs:
            args.pairs = parse_pairs(args.pairs)
        else:
            data_chunks = parse_data_chunks(args.data_chunks_list)
            args.pairs = pairs_for_fault_tolerance(data_chunks, args.fault_tolerance)
        args.directions = normalize_directions(args.directions)
    except argparse.ArgumentTypeError as exc:
        parser.error(str(exc))
    return args, passthrough_args


def build_command(args: argparse.Namespace, passthrough_args: list[str]) -> list[str]:
    pairs = args.pairs
    directions = args.directions

    command = [
        args.python,
        str(args.benchmark_script),
        "--sweep",
        "--requirement-type",
        "cipher",
        "--algorithm",
        "RS",
        "--directions",
        directions,
        "--objects-list",
        str(args.objects),
        "--size-mb-list",
        f"{args.size_mb:g}",
        "--ida-k-list",
        csv(k for k, _ in pairs),
        "--ida-m-list",
        csv(m for _, m in pairs),
        "--inter-arrival",
        f"{args.inter_arrival:g}",
        "--aes-key-bits",
        str(args.aes_key_bits),
        "--input-mode",
        args.input_mode,
        "--simulator-dir",
        str(args.simulator_dir),
        "--base-simulator-config",
        str(args.base_simulator_config),
        "--real-runner",
        str(args.real_runner),
        "--out-dir",
        str(args.out_dir),
        "--sweep-name",
        args.sweep_name,
    ]

    if args.workers_list:
        command.extend(["--workers-list", args.workers_list])
    else:
        command.extend(["--workers", str(args.workers)])

    if args.service_time_models:
        command.extend(["--service-time-models", args.service_time_models])
    else:
        add_if_present(command, "--service-time-model", args.service_time_model)

    add_if_present(command, "--hardware-profile", args.hardware_profile)
    add_if_present(command, "--container-platform", args.container_platform)
    add_if_present(command, "--queue-container-image", args.queue_container_image)

    if args.skip_build:
        command.append("--skip-build")
    if args.simulator_only:
        command.append("--simulator-only")
    if args.stop_on_error:
        command.append("--stop-on-error")
    if args.dry_run:
        command.append("--dry-run")

    command.extend(passthrough_args)
    return command


def main() -> int:
    args, passthrough_args = parse_args()
    repo_root = Path(__file__).resolve().parent
    benchmark_script = args.benchmark_script
    if not benchmark_script.is_absolute():
        benchmark_script = repo_root / benchmark_script
    if not benchmark_script.exists():
        print(f"benchmark script not found: {benchmark_script}", file=sys.stderr)
        return 2
    args.benchmark_script = benchmark_script

    command = build_command(args, passthrough_args)
    printable = " ".join(shlex.quote(part) for part in command)
    print(f"Running RS chunk sweep via benchmark_single_requirement.py:\n{printable}", flush=True)
    if args.print_command_only:
        return 0

    completed = subprocess.run(command, cwd=repo_root, check=False)
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
