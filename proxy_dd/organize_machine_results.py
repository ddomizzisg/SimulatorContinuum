#!/usr/bin/env python3
import argparse
import csv
import json
import shutil
from pathlib import Path


MACHINE_SPECS = {
    "c3": {
        "machine_name": "C3",
        "hardware_profile": "hpc",
        "description": "HPC machine",
        "datasets": {
            "cost-efficiency.csv": [
                "c3/cost-efficiency/test1_1to1000/comp_stats.csv",
                "kbresults/c3/cost-efficiency/comp_stats.csv",
                "kbresults/c3/cost-efficiency/test1/comp_stats.csv",
                "kbresults/c3/cost-efficiency/test2/comp_stats.csv",
            ],
            "integrity.csv": [
                "c3/security/test_1to100/integ_stats.csv",
                "c3/security/test_1000/integ_stats.csv",
                "kbresults/c3/security/integ_stats.csv",
                "kbresults/c3/security/test1/integ_stats.csv",
                "kbresults/c3/security/test2/integ_stats.csv",
            ],
            "confidentiality.csv": [
                "c3/security/test_1to100/conf_stats.csv",
                "c3/security/test_1000/conf_stats.csv",
                "kbresults/c3/security/conf_stats.csv",
            ],
            "reliability.csv": [
                "c3/reliability/test_1to100/erasure_stats.csv",
                "c3/reliability/test_1000/erasure_stats.csv",
                "kbresults/c3/reliability/erasure_stats.csv",
            ],
        },
    },
    "toge": {
        "machine_name": "Toge",
        "hardware_profile": "raspberry_pi_5",
        "description": "Raspberry Pi 5",
        "datasets": {
            "cost-efficiency.csv": [
                "toge/cost-efficiency/comp_stats.csv",
                "kbresults/toge/cost-efficiency/comp_stats.csv",
            ],
            "integrity.csv": [
                "toge/security/integ_stats.csv",
                "kbresults/toge/security/integ_stats.csv",
            ],
            "confidentiality.csv": [
                "toge/security/conf_stats.csv",
                "kbresults/toge/security/conf_stats.csv",
            ],
            "reliability.csv": [
                "toge/reliability/erasure_stats.csv",
                "kbresults/toge/reliability/erasure_stats.csv",
            ],
        },
    },
    "dianalap": {
        "machine_name": "DianaLap",
        "hardware_profile": "personal_laptop",
        "description": "Personal laptop",
        "datasets": {
            "cost-efficiency.csv": [
                "DianaLap/cost-efficiency/comp_stats.csv",
                "kbresults/Dianalap/cost-efficiency/comp_stats.csv",
            ],
            "integrity.csv": [
                "DianaLap/security/integ_stats.csv",
                "kbresults/Dianalap/security/integ_stats.csv",
            ],
            "confidentiality.csv": [
                "DianaLap/security/conf_stats.csv",
                "kbresults/Dianalap/security/conf_stats.csv",
            ],
            "reliability.csv": [
                "DianaLap/reliability/erasure_stats.csv",
                "kbresults/Dianalap/reliability/erasure_stats.csv",
            ],
        },
    },
    "dantelap": {
        "machine_name": "DanteLap",
        "hardware_profile": "personal_laptop_with_gpu",
        "description": "Personal laptop with GPU",
        "datasets": {
            "cost-efficiency.csv": [
                "DanteLap/cost-efficiency/comp_stats.csv",
            ],
            "integrity.csv": [
                "DanteLap/security/integ_stats.csv",
            ],
            "confidentiality.csv": [
                "DanteLap/security/conf_stats.csv",
            ],
            "reliability.csv": [
                "DanteLap/reliability/erasure_stats.csv",
            ],
        },
    },
    "panda": {
        "machine_name": "Panda",
        "hardware_profile": "unknown",
        "description": "Panda machine",
        "datasets": {
            "cost-efficiency.csv": [
                "kbresults/panda/cost-efficiency/comp_stats.csv",
            ],
            "integrity.csv": [
                "kbresults/panda/security/integ_stats.csv",
            ],
            "confidentiality.csv": [
                "kbresults/panda/security/conf_stats.csv",
            ],
            "reliability.csv": [
                "kbresults/panda/reliability/erasure_stats.csv",
            ],
        },
    },
}


SORT_COLUMNS = {
    "cost-efficiency.csv": ["algoritmo", "size_mb"],
    "integrity.csv": ["algoritmo", "size_mb"],
    "confidentiality.csv": ["algorithm", "key_bits", "size_mb"],
    "reliability.csv": ["algoritmo", "k_datos", "m_paridad", "size_mb"],
}


def parse_args():
    parser = argparse.ArgumentParser(
        description="Normalize results_different_machines into machine-specific real_values-style datasets."
    )
    parser.add_argument(
        "--source",
        type=Path,
        default=Path("results_different_machines"),
        help="Source directory with raw benchmark results.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("results_different_machines/organized"),
        help="Output directory for normalized machine datasets.",
    )
    return parser.parse_args()


def numeric_or_text(value):
    try:
        return (0, float(value))
    except (TypeError, ValueError):
        return (1, str(value))


def sort_rows(rows, dataset_name):
    sort_columns = SORT_COLUMNS.get(dataset_name, [])

    def sort_key(row):
        return tuple(numeric_or_text(row.get(column, "")) for column in sort_columns)

    return sorted(rows, key=sort_key)


def read_csv_rows(path):
    with path.open("r", encoding="utf-8", newline="") as fp:
        reader = csv.DictReader(fp)
        rows = list(reader)
        return reader.fieldnames or [], rows


def merge_dataset_rows(source_root, relative_paths, dataset_name):
    fieldnames = None
    merged = []
    seen = set()

    for relative_path in relative_paths:
        path = source_root / relative_path
        if not path.exists():
            raise FileNotFoundError(f"Missing source CSV: {path}")

        current_fields, rows = read_csv_rows(path)
        
        # Normalize KB columns to MB columns
        normalized_fields = []
        kb_columns = []
        for f in current_fields:
            if '_kb' in f:
                new_f = f.replace('_kb', '_mb')
                normalized_fields.append(new_f)
                kb_columns.append((f, new_f))
            else:
                normalized_fields.append(f)
                
        if kb_columns:
            current_fields = normalized_fields
            for row in rows:
                for old_f, new_f in kb_columns:
                    val = row.pop(old_f, None)
                    if val is not None:
                        try:
                            row[new_f] = f"{float(val) / 1024.0:g}"
                        except ValueError:
                            row[new_f] = val

        if fieldnames is None:
            fieldnames = current_fields
        elif current_fields != fieldnames:
            raise ValueError(
                f"Schema mismatch while merging {dataset_name}: {path} has fields {current_fields}, expected {fieldnames}"
            )

        for row in rows:
            dedupe_key = tuple((name, row.get(name, "")) for name in fieldnames)
            if dedupe_key in seen:
                continue
            seen.add(dedupe_key)
            merged.append(row)

    return fieldnames or [], sort_rows(merged, dataset_name)


def write_csv(path, fieldnames, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as fp:
        writer = csv.DictWriter(fp, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def list_sizes(rows):
    size_values = sorted({row.get("size_mb", "") for row in rows}, key=numeric_or_text)
    return [value for value in size_values if value != ""]


def summarize_dataset(machine_id, spec, dataset_name, rows):
    algorithm_column = "algoritmo" if rows and "algoritmo" in rows[0] else "algorithm"
    algorithms = sorted({row.get(algorithm_column, "") for row in rows if row.get(algorithm_column, "")})
    return {
        "machine_id": machine_id,
        "machine_name": spec["machine_name"],
        "hardware_profile": spec["hardware_profile"],
        "dataset": dataset_name,
        "rows": len(rows),
        "sizes_mb": "|".join(list_sizes(rows)),
        "algorithms": "|".join(algorithms),
    }


def write_readme(output_root, summaries):
    lines = [
        "# Organized Machine Results",
        "",
        "This directory reorganizes `results_different_machines` into machine-specific datasets that mirror the structure of `real_values`.",
        "",
        "Machine mapping:",
        "",
        "- `c3`: HPC machine",
        "- `toge`: Raspberry Pi 5",
        "- `dianalap`: Personal laptop",
        "- `dantelap`: Personal laptop with GPU",
        "- `panda`: Panda machine",
        "",
        "Each machine folder contains a `real_values/` directory with:",
        "",
        "- `cost-efficiency.csv`",
        "- `integrity.csv`",
        "- `confidentiality.csv`",
        "- `reliability.csv`",
        "",
        "Notes:",
        "",
        "- `confidentiality.csv` is additional to the original `real_values` layout because the raw machine benchmarks include encryption timings separately.",
        "- `c3` security and reliability results were merged from split benchmark runs covering `1..100 MB` and `1000 MB`.",
        "",
        "Coverage summary:",
        "",
        "| Machine | Profile | Dataset | Rows | Sizes (MB) | Algorithms |",
        "|---|---|---:|---:|---|---|",
    ]

    for summary in summaries:
        lines.append(
            f"| {summary['machine_name']} | {summary['hardware_profile']} | {summary['dataset']} | "
            f"{summary['rows']} | {summary['sizes_mb']} | {summary['algorithms']} |"
        )

    (output_root / "README.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    args = parse_args()
    repo_dir = Path(__file__).resolve().parent
    source_root = (repo_dir / args.source).resolve() if not args.source.is_absolute() else args.source.resolve()
    output_root = (repo_dir / args.output).resolve() if not args.output.is_absolute() else args.output.resolve()

    if output_root.exists():
        shutil.rmtree(output_root)
    output_root.mkdir(parents=True, exist_ok=True)

    summaries = []

    for machine_id, spec in MACHINE_SPECS.items():
        machine_root = output_root / machine_id
        real_values_root = machine_root / "real_values"
        real_values_root.mkdir(parents=True, exist_ok=True)

        manifest = {
            "machine_id": machine_id,
            "machine_name": spec["machine_name"],
            "hardware_profile": spec["hardware_profile"],
            "description": spec["description"],
            "source_root": str(source_root),
            "datasets": {},
        }

        for dataset_name, relative_paths in spec["datasets"].items():
            fieldnames, rows = merge_dataset_rows(source_root, relative_paths, dataset_name)
            write_csv(real_values_root / dataset_name, fieldnames, rows)
            manifest["datasets"][dataset_name] = {
                "source_files": relative_paths,
                "rows": len(rows),
                "sizes_mb": list_sizes(rows),
            }
            summaries.append(summarize_dataset(machine_id, spec, dataset_name, rows))

        with (machine_root / "machine_info.json").open("w", encoding="utf-8") as fp:
            json.dump(manifest, fp, indent=2)
            fp.write("\n")

    summary_path = output_root / "machine_dataset_summary.csv"
    write_csv(
        summary_path,
        ["machine_id", "machine_name", "hardware_profile", "dataset", "rows", "sizes_mb", "algorithms"],
        summaries,
    )
    write_readme(output_root, summaries)

    print(f"Organized machine datasets written to {output_root}")
    print(f"Summary written to {summary_path}")


if __name__ == "__main__":
    main()
