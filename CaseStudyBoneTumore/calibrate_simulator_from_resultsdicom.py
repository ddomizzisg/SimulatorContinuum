#!/usr/bin/env python3
"""Infer per-DICOM stage times from resultsdicombydicom and emit a simulator config."""
import argparse
import csv
import json
import re
import statistics
from collections import defaultdict
from pathlib import Path


STAGE_ORDER = ("edge", "fog", "cloud")
STAGE_MARKERS = {
    "stage_wall_clock_edge": "edge",
    "stage_wall_clock_fog": "fog",
    "stage_wall_clock_cloud": "cloud",
}
NEXT_STAGE = {
    "edge": "fog",
    "fog": "cloud",
    "cloud": "cloud",
}
APPLICATION_TASK_BY_STAGE = {
    "edge": "edge_acquisition",
    "fog": "fog_preprocessing",
    "cloud": "cloud_inference",
}
SIMULATOR_STAGE_NAME = {
    "edge": "edge_acquisition",
    "fog": "fog_preprocessing",
    "cloud": "cloud_inference",
}


def parse_args():
    parser = argparse.ArgumentParser(
        description="Calibrate proxy_dd CT simulator timings from CaseStudyBoneTumore/resultsdicombydicom."
    )
    parser.add_argument(
        "--results-root",
        type=Path,
        default=Path("CaseStudyBoneTumore/resultsdicombydicom"),
        help="Root containing <N>std/exp_w*/workflow_timing.log results.",
    )
    parser.add_argument(
        "--dataset",
        type=Path,
        default=Path.home() / "Downloads" / "medicalimages" / "dicoms",
        help="Dataset root used to infer DICOM files per study when --dicoms-per-study is not set.",
    )
    parser.add_argument(
        "--dicoms-per-study",
        type=int,
        default=None,
        help="Override DICOM files per study. Use this when the benchmark dataset is unavailable locally.",
    )
    parser.add_argument(
        "--base-config",
        type=Path,
        default=Path("proxy_dd/ct_scan_pipeline_results/ct_scan_pipeline_config.json"),
        help="Simulator config to copy and calibrate.",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=Path("CaseStudyBoneTumore/resultsdicombydicom/calibration"),
        help="Directory for calibration summaries.",
    )
    parser.add_argument(
        "--out-config",
        type=Path,
        default=Path("proxy_dd/ct_scan_pipeline_results/ct_scan_pipeline_config_calibrated_dicom_by_dicom.json"),
        help="Calibrated simulator config path.",
    )
    parser.add_argument(
        "--calibration-workers",
        type=int,
        default=1,
        help="Worker count used to infer single-worker per-DICOM service times.",
    )
    parser.add_argument(
        "--min-studies",
        type=int,
        default=10,
        help="Ignore smaller workloads when inferring calibration values.",
    )
    parser.add_argument(
        "--stat",
        choices=("median", "mean"),
        default="median",
        help="Statistic used across selected experiments.",
    )
    return parser.parse_args()


def count_dataset_files(dataset):
    if not dataset.exists():
        return None
    dcm_files = list(dataset.glob("**/*.dcm"))
    files = dcm_files if dcm_files else [path for path in dataset.rglob("*") if path.is_file()]
    return len(files)


def parse_experiment_path(path):
    study_match = re.fullmatch(r"(\d+)std", path.parents[1].name)
    worker_match = re.fullmatch(r"exp_w(\d+)", path.parent.name)
    if not study_match or not worker_match:
        return None
    return int(study_match.group(1)), int(worker_match.group(1))


def parse_timing_log(path):
    current_stage = "edge"
    stage_task_sums = defaultdict(float)
    stage_task_counts = defaultdict(int)
    stage_wall = {}

    with path.open(newline="", encoding="utf-8") as fp:
        reader = csv.DictReader(fp)
        for row in reader:
            task = row.get("task", "")
            try:
                duration = float(row.get("duration_seconds", 0.0))
            except (TypeError, ValueError):
                continue

            marker_stage = STAGE_MARKERS.get(task)
            if marker_stage is not None:
                stage_wall[marker_stage] = duration
                current_stage = NEXT_STAGE[marker_stage]
                continue

            key = (current_stage, task)
            stage_task_sums[key] += duration
            stage_task_counts[key] += 1

    return stage_task_sums, stage_task_counts, stage_wall


def statistic(values, stat_name):
    if not values:
        return 0.0
    if stat_name == "mean":
        return statistics.mean(values)
    return statistics.median(values)


def load_experiments(results_root, dicoms_per_study):
    experiments = []
    for timing_log in sorted(results_root.glob("*std/exp_w*/workflow_timing.log")):
        parsed = parse_experiment_path(timing_log)
        if parsed is None:
            continue
        studies, workers = parsed
        total_dicoms = studies * dicoms_per_study
        stage_task_sums, stage_task_counts, stage_wall = parse_timing_log(timing_log)

        experiment = {
            "studies": studies,
            "workers": workers,
            "total_dicoms": total_dicoms,
            "path": str(timing_log),
            "stage_task_sums": stage_task_sums,
            "stage_task_counts": stage_task_counts,
            "stage_wall": stage_wall,
        }
        experiments.append(experiment)
    return experiments


def build_stage_rows(experiments):
    rows = []
    for exp in experiments:
        for stage in STAGE_ORDER:
            app_task = APPLICATION_TASK_BY_STAGE[stage]
            app_seconds = exp["stage_task_sums"].get((stage, app_task), 0.0)
            work_seconds = sum(
                seconds for (task_stage, _), seconds in exp["stage_task_sums"].items()
                if task_stage == stage
            )
            wall_seconds = exp["stage_wall"].get(stage, 0.0)
            rows.append({
                "studies": exp["studies"],
                "workers": exp["workers"],
                "total_dicoms": exp["total_dicoms"],
                "stage": stage,
                "application_task": app_task,
                "application_work_seconds": app_seconds,
                "application_seconds_per_dicom": app_seconds / exp["total_dicoms"],
                "stage_work_seconds": work_seconds,
                "stage_work_seconds_per_dicom": work_seconds / exp["total_dicoms"],
                "stage_wall_seconds": wall_seconds,
                "stage_wall_seconds_per_dicom": wall_seconds / exp["total_dicoms"],
                "timing_log": exp["path"],
            })
    return rows


def build_task_rows(experiments):
    rows = []
    for exp in experiments:
        for stage in STAGE_ORDER:
            for (task_stage, task), seconds in sorted(exp["stage_task_sums"].items()):
                if task_stage != stage:
                    continue
                count = exp["stage_task_counts"].get((stage, task), 0)
                rows.append({
                    "studies": exp["studies"],
                    "workers": exp["workers"],
                    "total_dicoms": exp["total_dicoms"],
                    "stage": stage,
                    "task": task,
                    "task_count": count,
                    "task_work_seconds": seconds,
                    "task_work_seconds_per_dicom": seconds / exp["total_dicoms"],
                    "mean_chunk_seconds": seconds / count if count else 0.0,
                    "timing_log": exp["path"],
                })
    return rows


def write_csv(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    with path.open("w", newline="", encoding="utf-8") as fp:
        writer = csv.DictWriter(fp, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def select_stage_rows(stage_rows, workers, min_studies):
    selected = [
        row for row in stage_rows
        if int(row["workers"]) == workers and int(row["studies"]) >= min_studies
    ]
    return selected or stage_rows


def build_calibration(stage_rows, selected_rows, stat_name):
    calibration = {}
    wall_targets = {}
    work_totals = {}
    selected_by_stage = defaultdict(list)
    for row in selected_rows:
        selected_by_stage[row["stage"]].append(row)

    for stage in STAGE_ORDER:
        rows = selected_by_stage.get(stage, [])
        calibration[stage] = statistic(
            [float(row["application_seconds_per_dicom"]) for row in rows],
            stat_name,
        )
        wall_targets[stage] = statistic(
            [float(row["stage_wall_seconds_per_dicom"]) for row in rows],
            stat_name,
        )
        work_totals[stage] = statistic(
            [float(row["stage_work_seconds_per_dicom"]) for row in rows],
            stat_name,
        )

    return calibration, wall_targets, work_totals


def update_config(base_config_path, out_config_path, calibration, wall_targets, work_totals, metadata):
    with base_config_path.open(encoding="utf-8") as fp:
        config = json.load(fp)

    app_by_sim_stage = {
        SIMULATOR_STAGE_NAME[stage]: seconds
        for stage, seconds in calibration.items()
    }

    for stage in config.get("stages", []):
        name = stage.get("name")
        if name in app_by_sim_stage:
            stage["application_mean_service_time"] = app_by_sim_stage[name]

    config_metadata = config.setdefault("metadata", {})
    stage_seconds_per_object = config_metadata.setdefault("stage_seconds_per_object", {})
    stage_seconds_per_study = config_metadata.setdefault("stage_seconds_per_study", {})
    object_count = int(config.get("traces", [{}])[0].get("MUESTRAS", metadata["dicoms_per_study"]))
    for stage, seconds in calibration.items():
        sim_name = SIMULATOR_STAGE_NAME[stage]
        stage_seconds_per_object[sim_name] = seconds
        stage_seconds_per_study[sim_name] = seconds * object_count

    config_metadata["dicom_by_dicom_calibration"] = {
        **metadata,
        "application_seconds_per_dicom": {
            SIMULATOR_STAGE_NAME[stage]: seconds for stage, seconds in calibration.items()
        },
        "stage_wall_seconds_per_dicom_target": {
            SIMULATOR_STAGE_NAME[stage]: seconds for stage, seconds in wall_targets.items()
        },
        "stage_work_seconds_per_dicom_observed": {
            SIMULATOR_STAGE_NAME[stage]: seconds for stage, seconds in work_totals.items()
        },
    }

    out_config_path.parent.mkdir(parents=True, exist_ok=True)
    with out_config_path.open("w", encoding="utf-8") as fp:
        json.dump(config, fp, indent=2)
        fp.write("\n")


def main():
    args = parse_args()
    dicoms_per_study = args.dicoms_per_study or count_dataset_files(args.dataset)
    if not dicoms_per_study:
        raise SystemExit("Could not infer DICOM count. Pass --dicoms-per-study.")

    experiments = load_experiments(args.results_root, dicoms_per_study)
    if not experiments:
        raise SystemExit(f"No timing logs found under {args.results_root}")

    stage_rows = build_stage_rows(experiments)
    task_rows = build_task_rows(experiments)
    selected_rows = select_stage_rows(stage_rows, args.calibration_workers, args.min_studies)
    calibration, wall_targets, work_totals = build_calibration(stage_rows, selected_rows, args.stat)

    args.out_dir.mkdir(parents=True, exist_ok=True)
    stage_summary_path = args.out_dir / "stage_summary.csv"
    task_summary_path = args.out_dir / "task_summary.csv"
    application_calibration_path = args.out_dir / "application_calibration.csv"
    calibration_summary_path = args.out_dir / "calibration_summary.json"

    write_csv(stage_summary_path, stage_rows)
    write_csv(task_summary_path, task_rows)
    write_csv(
        application_calibration_path,
        [
            {
                "task": SIMULATOR_STAGE_NAME[stage],
                "duration_seconds_per_dicom": seconds,
                "duration_seconds": seconds * dicoms_per_study,
            }
            for stage, seconds in calibration.items()
        ],
    )

    metadata = {
        "results_root": str(args.results_root),
        "dicoms_per_study": dicoms_per_study,
        "calibration_workers": args.calibration_workers,
        "min_studies": args.min_studies,
        "stat": args.stat,
        "selected_experiments": sorted({
            f"{int(row['studies'])}std/exp_w{int(row['workers'])}" for row in selected_rows
        }),
        "stage_summary_csv": str(stage_summary_path),
        "task_summary_csv": str(task_summary_path),
        "application_calibration_csv": str(application_calibration_path),
    }

    summary = {
        **metadata,
        "application_seconds_per_dicom": {
            SIMULATOR_STAGE_NAME[stage]: calibration[stage] for stage in STAGE_ORDER
        },
        "stage_wall_seconds_per_dicom_target": {
            SIMULATOR_STAGE_NAME[stage]: wall_targets[stage] for stage in STAGE_ORDER
        },
        "stage_work_seconds_per_dicom_observed": {
            SIMULATOR_STAGE_NAME[stage]: work_totals[stage] for stage in STAGE_ORDER
        },
        "calibrated_config": str(args.out_config),
    }
    with calibration_summary_path.open("w", encoding="utf-8") as fp:
        json.dump(summary, fp, indent=2)
        fp.write("\n")

    update_config(args.base_config, args.out_config, calibration, wall_targets, work_totals, metadata)

    print(f"DICOMs per study: {dicoms_per_study}")
    print(f"Selected experiments: {', '.join(summary['selected_experiments'])}")
    print("Application service time per DICOM:")
    for stage in STAGE_ORDER:
        sim_name = SIMULATOR_STAGE_NAME[stage]
        print(f"  {sim_name}: {calibration[stage]:.6f} s")
    print("Stage wall-clock target per DICOM:")
    for stage in STAGE_ORDER:
        sim_name = SIMULATOR_STAGE_NAME[stage]
        print(f"  {sim_name}: {wall_targets[stage]:.6f} s")
    print(f"Wrote calibrated config: {args.out_config}")
    print(f"Wrote calibration summary: {calibration_summary_path}")


if __name__ == "__main__":
    main()
