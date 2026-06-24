#!/usr/bin/env python3
"""Plot Reed-Solomon chunk sweep results from benchmark_single_requirement."""

from __future__ import annotations

import argparse
import os
import sys
import tempfile
from pathlib import Path

import pandas as pd

try:
    os.environ.setdefault("MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / "matplotlib-cache"))
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import seaborn as sns
except ImportError:
    print("Error: matplotlib and seaborn are required. Install them with: pip install matplotlib seaborn")
    sys.exit(1)


pt = 1./72.27
jour_sizes = {"PRD": {"onecol": 246.*pt, "twocol": 510.*pt},
              "CQG": {"onecol": 374.*pt}, }
my_width = jour_sizes["PRD"]["twocol"]
golden = (1 + 5 ** 0.5) / 1.1
# golden = (1 + 5 ** 0.5) / 2.4 # you can modify here for a higher height if needed
plt.rcParams.update({
    'axes.labelsize': 14,       # Axis label font size
    'legend.fontsize': 14,      # Legend font size
    'xtick.labelsize': 12,      # X-axis tick label font size
})



DEFAULT_SUMMARY = Path("single_requirement_benchmarks/rs_100mb_chunk_sweep/sweep_summary.csv")

METRIC_COLUMNS = {
    "requirement": "requirement_compute_seconds",
    "stage": "stage_compute_seconds",
    "family": "family_compute_seconds",
    "total-stage": "total_stage_compute_seconds",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Create plots for an RS data/parity chunk sweep."
    )
    parser.add_argument(
        "summary_csv",
        nargs="?",
        type=Path,
        default=DEFAULT_SUMMARY,
        help=f"Sweep summary CSV. Defaults to {DEFAULT_SUMMARY}.",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        help="Output directory for figures. Defaults to <summary_csv parent>/rs_plots.",
    )
    parser.add_argument(
        "--metric",
        choices=sorted(METRIC_COLUMNS),
        default="requirement",
        help="Metric to plot. Defaults to requirement.",
    )
    parser.add_argument(
        "--formats",
        default="png",
        help="Comma-separated output formats, for example png,pdf. Defaults to png.",
    )
    parser.add_argument(
        "--dpi",
        type=int,
        default=180,
        help="Image DPI for raster outputs. Defaults to 180.",
    )
    return parser.parse_args()


def as_numeric(df: pd.DataFrame, columns: list[str]) -> pd.DataFrame:
    for column in columns:
        if column in df.columns:
            df[column] = pd.to_numeric(df[column], errors="coerce")
    return df


def load_results(path: Path, metric_prefix: str) -> pd.DataFrame:
    if not path.exists():
        raise FileNotFoundError(path)

    df = pd.read_csv(path)
    required = [
        "status",
        "direction",
        "ida_k",
        "ida_m",
        "size_mb",
        "workers",
        "service_time_model",
        f"{metric_prefix}_simulator_seconds",
    ]
    missing = [column for column in required if column not in df.columns]
    if missing:
        raise ValueError(f"Missing required column(s): {', '.join(missing)}")

    df = df[df["status"] == "ok"].copy()
    if df.empty:
        raise ValueError("No successful rows found in the sweep summary.")

    numeric_columns = [
        "ida_k",
        "ida_m",
        "size_mb",
        "workers",
        f"{metric_prefix}_simulator_seconds",
    ]
    optional_numeric_columns = [
        f"{metric_prefix}_real_seconds",
        f"{metric_prefix}_ape_percent",
    ]
    df = as_numeric(df, numeric_columns).dropna(subset=numeric_columns)
    df = as_numeric(df, optional_numeric_columns)
    if df.empty:
        raise ValueError("No rows with complete numeric metrics found.")

    df["total_chunks"] = df["ida_k"] + df["ida_m"]
    df["total_encoded_size_mb"] = df["size_mb"] * df["total_chunks"] / df["ida_k"]
    df["parity_size_mb"] = df["total_encoded_size_mb"] - df["size_mb"]
    df["storage_overhead_percent"] = df["ida_m"] / df["ida_k"] * 100.0
    df["data_efficiency_percent"] = df["ida_k"] / df["total_chunks"] * 100.0
    df["config"] = "k=" + df["ida_k"].astype(int).astype(str) + ", m=" + df["ida_m"].astype(int).astype(str)
    df["series"] = (
        df["direction"].astype(str)
        + " | w="
        + df["workers"].astype(int).astype(str)
        + " | "
        + df["service_time_model"].astype(str)
    )
    if f"{metric_prefix}_real_seconds" in df.columns:
        df["real_throughput_mb_s"] = df["size_mb"] / df[f"{metric_prefix}_real_seconds"]
    else:
        df["real_throughput_mb_s"] = pd.NA
    df["simulator_throughput_mb_s"] = df["size_mb"] / df[f"{metric_prefix}_simulator_seconds"]
    return df.sort_values(["direction", "workers", "service_time_model", "ida_m", "ida_k"])


def save_figure(fig: plt.Figure, out_dir: Path, stem: str, formats: list[str], dpi: int) -> list[Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    paths = []
    for fmt in formats:
        path = out_dir / f"{stem}.{fmt}"
        fig.savefig(path, dpi=dpi, bbox_inches="tight")
        paths.append(path)
    plt.close(fig)
    return paths


def plot_seconds(df: pd.DataFrame, metric_prefix: str, title_suffix: str) -> plt.Figure:
    value_vars = [
        column
        for column in [f"{metric_prefix}_real_seconds", f"{metric_prefix}_simulator_seconds"]
        if column in df.columns and df[column].notna().any()
    ]
    long_df = df.melt(
        id_vars=["ida_k", "ida_m", "config", "series"],
        value_vars=value_vars,
        var_name="source",
        value_name="seconds",
    )
    long_df["source"] = long_df["source"].map(
        {
            f"{metric_prefix}_real_seconds": "Real",
            f"{metric_prefix}_simulator_seconds": "Simulator",
        }
    )

    fig, ax = plt.subplots(figsize=(10, 5.8))
    sns.lineplot(
        data=long_df,
        x="ida_k",
        y="seconds",
        hue="series",
        style="source",
        markers=True,
        dashes={"Real": "", "Simulator": (4, 2)},
        ax=ax,
    )
    ax.set_title(f"RS Compute Time by Data Chunks{title_suffix}")
    ax.set_xlabel("Data chunks (k)")
    ax.set_ylabel("Seconds")
    ax.grid(True, axis="y", alpha=0.3)
    ax.legend(title="Run / source", bbox_to_anchor=(1.02, 1), loc="upper left")
    fig.tight_layout()
    return fig


import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns

def plot_time_by_parity_blocks(df: pd.DataFrame, metric_prefix: str, title_suffix: str) -> plt.Figure:
    value_vars = [
        column
        for column in [f"{metric_prefix}_real_seconds", f"{metric_prefix}_simulator_seconds"]
        if column in df.columns and df[column].notna().any()
    ]
    long_df = df.melt(
        id_vars=["ida_k", "ida_m", "series"],
        value_vars=value_vars,
        var_name="source",
        value_name="seconds",
    ).dropna(subset=["seconds"])
    
    long_df["source"] = long_df["source"].map(
        {
            f"{metric_prefix}_real_seconds": "Real",
        }
    )
    long_df["parity_label"] = "m=" + long_df["ida_m"].astype(int).astype(str)

    storage_df = (
        df[["ida_k", "ida_m", "size_mb", "total_encoded_size_mb"]]
        .drop_duplicates()
        .sort_values(["ida_m", "size_mb", "ida_k"])
    )
    storage_df["parity_label"] = "m=" + storage_df["ida_m"].astype(int).astype(str)

    fig, ax_time = plt.subplots(figsize=(my_width, my_width / golden))
    ax_storage = ax_time.twinx()
    
    # Ensure lines sit ON TOP of the twinx bars
    ax_time.set_zorder(ax_storage.get_zorder() + 1)
    ax_time.patch.set_visible(False)

    parity_values = sorted(long_df["ida_m"].dropna().unique())
    palette = sns.color_palette("tab10", n_colors=max(len(parity_values), 1))
    color_by_m = {parity_m: palette[index] for index, parity_m in enumerate(parity_values)}
    line_styles = {"Real": "-", "Simulator": "--"}
    markers = {"Real": "o", "Simulator": "x"}
    include_series = long_df["series"].nunique() > 1

    # --- 1. Plot Time (Lines) ---
    group_keys = ["ida_m", "parity_label", "series", "source"]
    for (parity_m, parity_label, series, source), group in long_df.groupby(group_keys):
        group = group.sort_values("ida_k")
        label_parts = [parity_label]
        if include_series:
            label_parts.append(str(series))
        
        ax_time.plot(
            group["ida_k"],
            group["seconds"] * 1000,  # Convert to milliseconds for better readability
            color=color_by_m[parity_m],
            linestyle=line_styles.get(source, "-"),
            marker=markers.get(source, "o"),
            linewidth=2.1,
            markersize=6,
            label="  ".join(label_parts),
        )

    # --- 2. Plot Storage (Bars) ---
    storage_group_keys = ["ida_m", "parity_label"]
    if storage_df["size_mb"].nunique() > 1:
        storage_group_keys.append("size_mb")
        
    storage_groups = list(storage_df.groupby(storage_group_keys))
    num_groups = len(storage_groups)
    
    # Calculate widths so bars can sit side-by-side if there are multiple groups
    total_bar_width = 0.6
    bar_width = total_bar_width / num_groups if num_groups > 0 else total_bar_width

    for idx, (group_key, group) in enumerate(storage_groups):
        if len(storage_group_keys) == 2:
            parity_m, parity_label = group_key
            storage_label = f"{parity_label} (Resulting storage)"
        else:
            parity_m, parity_label, size_mb = group_key
            storage_label = f"{parity_label} storage ({size_mb:g} MB input)"
            
        group = group.sort_values("ida_k")
        
        # Calculate offset to prevent bars from overlapping
        offset = (idx - num_groups / 2.0 + 0.5) * bar_width
        
        ax_storage.bar(
            group["ida_k"] + offset,
            group["total_encoded_size_mb"],
            width=bar_width,
            color=color_by_m.get(parity_m, "0.3"),
            alpha=0.3, # Adds transparency so lines aren't obscured
            label=storage_label,
        )

    data_block_counts = sorted(long_df["ida_k"].dropna().unique())
    ax_time.set_xticks(data_block_counts)
    ax_time.set_xticklabels([str(int(value)) for value in data_block_counts])
    
    ax_time.set_xlabel("Data blocks (k)")
    ax_time.set_ylabel("Response time (ms)")
    ax_storage.set_ylabel("Resulting storage (MB)")
    ax_time.grid(True, axis="y", alpha=0.3)

    time_handles, time_labels = ax_time.get_legend_handles_labels()
    storage_handles, storage_labels = ax_storage.get_legend_handles_labels()
    
    ax_time.legend(
        time_handles + storage_handles,
        time_labels + storage_labels,
        frameon=True,
        loc="upper right" # Forced to upper left to usually stay out of the way of the bars
    )
    
    fig.tight_layout()
    return fig


def plot_throughput(df: pd.DataFrame, title_suffix: str) -> plt.Figure:
    value_vars = [
        column
        for column in ["real_throughput_mb_s", "simulator_throughput_mb_s"]
        if column in df.columns and df[column].notna().any()
    ]
    long_df = df.melt(
        id_vars=["ida_k", "ida_m", "config", "series"],
        value_vars=value_vars,
        var_name="source",
        value_name="throughput_mb_s",
    )
    long_df["source"] = long_df["source"].map(
        {
            "real_throughput_mb_s": "Real",
            "simulator_throughput_mb_s": "Simulator",
        }
    )

    fig, ax = plt.subplots(figsize=(10, 5.8))
    sns.lineplot(
        data=long_df,
        x="ida_k",
        y="throughput_mb_s",
        hue="series",
        style="source",
        markers=True,
        dashes={"Real": "", "Simulator": (4, 2)},
        ax=ax,
    )
    ax.set_title(f"RS Throughput by Data Chunks{title_suffix}")
    ax.set_xlabel("Data chunks (k)")
    ax.set_ylabel("MB/s")
    ax.grid(True, axis="y", alpha=0.3)
    ax.legend(title="Run / source", bbox_to_anchor=(1.02, 1), loc="upper left")
    fig.tight_layout()
    return fig


def plot_error(df: pd.DataFrame, metric_prefix: str, title_suffix: str) -> plt.Figure:
    fig, ax = plt.subplots(figsize=(9, 5.2))
    error_column = f"{metric_prefix}_ape_percent"
    if error_column in df.columns and df[error_column].notna().any():
        sns.lineplot(
            data=df,
            x="ida_k",
            y=error_column,
            hue="series",
            style="ida_m",
            markers=True,
            dashes=False,
            ax=ax,
        )
    else:
        ax.text(
            0.5,
            0.5,
            "APE is unavailable for simulator-only runs",
            ha="center",
            va="center",
            transform=ax.transAxes,
        )
    ax.set_title(f"Simulator Absolute Percentage Error{title_suffix}")
    ax.set_xlabel("Data chunks (k)")
    ax.set_ylabel("APE (%)")
    ax.grid(True, axis="y", alpha=0.3)
    handles, labels = ax.get_legend_handles_labels()
    if handles:
        ax.legend(handles, labels, title="Run / parity m", bbox_to_anchor=(1.02, 1), loc="upper left")
    fig.tight_layout()
    return fig


def plot_result_size_and_overhead(df: pd.DataFrame, title_suffix: str) -> plt.Figure:
    layout_df = (
        df[["ida_k", "ida_m", "total_chunks", "size_mb", "total_encoded_size_mb", "parity_size_mb"]]
        .drop_duplicates()
        .sort_values(["ida_m", "ida_k"])
    )

    fig, ax = plt.subplots(figsize=(my_width, my_width / golden))
    palette = sns.color_palette("tab10", n_colors=layout_df["ida_m"].nunique())
    for color, (parity_chunks, group) in zip(palette, layout_df.groupby("ida_m")):
        ax.plot(
            group["ida_k"],
            group["total_encoded_size_mb"],
            color=color,
            marker="o",
            linewidth=2.2,
            label=f"Resulting size (m={int(parity_chunks)})",
        )
        ax.plot(
            group["ida_k"],
            group["parity_size_mb"],
            color=color,
            marker="s",
            linestyle="--",
            linewidth=2.0,
            label=f"Overhead (m={int(parity_chunks)})",
        )

    input_sizes = sorted(layout_df["size_mb"].dropna().unique())
    if len(input_sizes) == 1:
        ax.axhline(
            input_sizes[0],
            color="0.35",
            linestyle=":",
            linewidth=1.6,
            label=f"Input size ({input_sizes[0]:g} MB)",
        )

    ax.set_title(f"RS Resulting Size and Storage Overhead by Chunk Layout{title_suffix}")
    ax.set_xlabel("Data chunks (k)")
    ax.set_ylabel("Size (MB)")
    ax.grid(True, axis="y", alpha=0.3)
    ax.legend(title="Layout result", bbox_to_anchor=(1.02, 1), loc="upper left")

    fig.tight_layout()
    return fig


def write_plot_table(df: pd.DataFrame, metric_prefix: str, out_dir: Path) -> Path:
    columns = [
        "direction",
        "workers",
        "service_time_model",
        "ida_k",
        "ida_m",
        "total_chunks",
        "size_mb",
        "total_encoded_size_mb",
        "parity_size_mb",
        f"{metric_prefix}_real_seconds",
        f"{metric_prefix}_simulator_seconds",
        f"{metric_prefix}_ape_percent",
        "real_throughput_mb_s",
        "simulator_throughput_mb_s",
        "storage_overhead_percent",
        "data_efficiency_percent",
    ]
    out_dir.mkdir(parents=True, exist_ok=True)
    table_path = out_dir / "rs_chunk_plot_data.csv"
    df[columns].to_csv(table_path, index=False)
    return table_path


def main() -> int:
    args = parse_args()
    metric_prefix = METRIC_COLUMNS[args.metric]
    formats = [item.strip().lstrip(".") for item in args.formats.split(",") if item.strip()]
    if not formats:
        print("Error: at least one output format is required.", file=sys.stderr)
        return 2

    summary_csv = args.summary_csv.resolve()
    out_dir = (args.out_dir or summary_csv.parent / "rs_plots").resolve()

    sns.set_theme(style="whitegrid", context="paper")
    plt.rcParams.update(
        {
            "font.size": 10,
            "axes.labelsize": 11,
            "axes.titlesize": 13,
            "legend.fontsize": 9,
            "legend.title_fontsize": 9,
        }
    )

    try:
        df = load_results(summary_csv, metric_prefix)
    except Exception as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1

    size_values = ", ".join(f"{value:g} MB" for value in sorted(df["size_mb"].unique()))
    title_suffix = f" ({size_values})" if size_values else ""

    written = []
    written.extend(save_figure(plot_seconds(df, metric_prefix, title_suffix), out_dir, "rs_compute_seconds_by_k", formats, args.dpi))
    written.extend(save_figure(plot_time_by_parity_blocks(df, metric_prefix, title_suffix), out_dir, "rs_time_by_failures", formats, args.dpi))
    written.extend(save_figure(plot_throughput(df, title_suffix), out_dir, "rs_throughput_by_k", formats, args.dpi))
    written.extend(save_figure(plot_error(df, metric_prefix, title_suffix), out_dir, "rs_simulator_error_by_k", formats, args.dpi))
    written.extend(save_figure(plot_result_size_and_overhead(df, title_suffix), out_dir, "rs_storage_overhead_by_k", formats, args.dpi))
    table_path = write_plot_table(df, metric_prefix, out_dir)

    print(f"Read {len(df)} successful RS sweep rows from {summary_csv}")
    print(f"Wrote plot data to {table_path}")
    for path in written:
        print(f"Wrote {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
