#!/usr/bin/env python3
"""
Run simulated CT scan pipeline experiments matching the real workflow study.
Calibrate using measured timings, execute simulations, and plot results.
"""
import subprocess
import sys
import json
import csv
import re
from pathlib import Path
from datetime import datetime
import matplotlib.pyplot as plt
import numpy as np


WORKERS_LIST = [1, 2, 4, 8, 16, 32]
STUDIES_LIST = [1, 10, 20, 30]
DICOMS_PER_STUDY = 89
BASE_CONFIG_FILE = Path("proxy_dd/ct_scan_pipeline_config_calibrated_dicom_by_dicom.json").resolve()
OUTPUT_BASE = Path("simulated_pipeline_results")
SIMULATOR = Path("proxy_dd/main").resolve()

pt = 1./72.27
jour_sizes = {"PRD": {"onecol": 246.*pt, "twocol": 510.*pt},
              "CQG": {"onecol": 374.*pt}, }
my_width = jour_sizes["PRD"]["twocol"]
golden = (1 + 5 ** 0.5) / 1.1


def run_experiment(workers, studies):
    """Run a single proxy_dd experiment directly and extract timing."""
    output_dir = OUTPUT_BASE / f"workers_{workers}_studies_{studies}"
    output_dir.mkdir(parents=True, exist_ok=True)
    results_dir = output_dir / "results"
    results_dir.mkdir(parents=True, exist_ok=True)

    print(f"[{datetime.now().strftime('%H:%M:%S')}] Running: workers={workers}, studies={studies}")
    
    # Load base config
    try:
        with BASE_CONFIG_FILE.open("r") as f:
            config = json.load(f)
    except Exception as e:
        print(f"  ERROR: Could not load base config {BASE_CONFIG_FILE}: {e}")
        return None
        
    # Override parameters
    config["workers"] = workers
    if "traces" in config and len(config["traces"]) > 0:
        config["traces"][0]["MUESTRAS"] = studies * DICOMS_PER_STUDY
    else:
        print(f"  ERROR: Invalid traces configuration in base config.")
        return None
        
    temp_config_path = output_dir / "temp_config.json"
    with temp_config_path.open("w") as f:
        json.dump(config, f, indent=4)
        
    cmd = [
        str(SIMULATOR),
        str(temp_config_path.resolve())
    ]
    
    try:
        # Run from output directory so results go into output_dir/results
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=300, cwd=str(output_dir.resolve()))
        if result.returncode != 0:
            print(f"  ERROR: {result.stderr[:500]}")
            print(f"  STDOUT: {result.stdout[:500]}")
            return None
        
        # Read the stage totals CSV to get pipeline timing
        stage_totals_file = output_dir / "results" / "stage_totals_by_workers.csv"
        if not stage_totals_file.exists():
            print(f"  ERROR: stage_totals_by_workers.csv not found at {stage_totals_file}")
            print(f"  STDOUT: {result.stdout[:500]}")
            return None
        
        # Read the stage totals and extract the pipeline total (sum of all stages' total_seconds)
        with stage_totals_file.open() as fh:
            reader = csv.DictReader(fh)
            rows = list(reader)
            if not rows:
                print(f"  ERROR: stage_totals_by_workers.csv is empty")
                return None
            # Sum up the total_seconds from all stages to get pipeline completion time
            total_time = sum(float(row.get('total_seconds', 0)) for row in rows)
        
        print(f"  ✓ Complete: {total_time:.2f} seconds")
        return total_time
    
    except subprocess.TimeoutExpired:
        print(f"  ERROR: Timeout after 300 seconds")
        return None
    except Exception as e:
        print(f"  ERROR: {e}")
        return None


def main():
    print(f"Starting calibrated pipeline simulations at {datetime.now()}")
    print(f"Base config: {BASE_CONFIG_FILE}")
    print(f"Simulator: {SIMULATOR}")
    print(f"DICOMs per study: {DICOMS_PER_STUDY}")
    print()
    
    # Verify base config exists
    if not BASE_CONFIG_FILE.exists():
        print(f"ERROR: Base config not found: {BASE_CONFIG_FILE}")
        return 1
    
    # Verify simulator exists
    if not SIMULATOR.exists():
        print(f"ERROR: Simulator not found: {SIMULATOR}")
        return 1
    
    # Run all experiments
    results = {}
    for studies in STUDIES_LIST:
        results[studies] = {}
        for workers in WORKERS_LIST:
            timing = run_experiment(workers, studies)
            results[studies][workers] = timing
    
    print("\n" + "="*60)
    print("SIMULATION RESULTS SUMMARY")
    print("="*60)
    
    # Save results to CSV
    results_csv = OUTPUT_BASE / "simulation_results.csv"
    with results_csv.open("w", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow(["workers"] + [f"studies_{s}" for s in STUDIES_LIST])
        for workers in WORKERS_LIST:
            row = [workers]
            for studies in STUDIES_LIST:
                timing = results[studies].get(workers)
                row.append(f"{timing:.2f}" if timing is not None else "FAIL")
            writer.writerow(row)
    
    print(f"Results saved to: {results_csv}")
    
    # Print simulated table
    print("\nSimulated Execution times (seconds):")
    print(f"{'Workers':>10}", end="")
    for studies in STUDIES_LIST:
        print(f"{studies:>15}", end="")
    print()
    print("-" * (10 + 15 * len(STUDIES_LIST)))
    for workers in WORKERS_LIST:
        print(f"{workers:>10}", end="")
        for studies in STUDIES_LIST:
            timing = results[studies].get(workers)
            if timing is not None:
                print(f"{timing:>15.2f}", end="")
            else:
                print(f"{'FAIL':>15}", end="")
        print()
    
    # Create plots
    plot_simulations(results)
    
    # Load and print real results for comparison
    real_results = load_real_results()
    if real_results:
        print("\nReal Workflow Execution times (seconds):")
        print(f"{'Workers':>10}", end="")
        for studies in STUDIES_LIST:
            print(f"{studies:>15}", end="")
        print()
        print("-" * (10 + 15 * len(STUDIES_LIST)))
        for workers in WORKERS_LIST:
            print(f"{workers:>10}", end="")
            for studies in STUDIES_LIST:
                timing = real_results.get(studies, {}).get(workers)
                if timing is not None:
                    print(f"{timing:>15.2f}", end="")
                else:
                    print(f"{'N/A':>15}", end="")
            print()
        
        # Print error percentages
        print("\nSimulation Error (% difference from real):")
        print(f"{'Workers':>10}", end="")
        for studies in STUDIES_LIST:
            print(f"{studies:>15}", end="")
        print()
        print("-" * (10 + 15 * len(STUDIES_LIST)))
        for workers in WORKERS_LIST:
            print(f"{workers:>10}", end="")
            for studies in STUDIES_LIST:
                sim = results[studies].get(workers)
                real = real_results.get(studies, {}).get(workers)
                if sim is not None and real is not None:
                    error = ((sim - real) / real) * 100
                    print(f"{error:>14.1f}%", end="")
                else:
                    print(f"{'N/A':>15}", end="")
            print()
    
    print("\n" + "="*60)
    
    print("\n" + "="*60)
    print(f"Completed at {datetime.now()}")
    return 0


def load_real_results():
    """Load real execution results from resultsdicombydicom."""
    base_dir = Path("CaseStudyBoneTumore/resultsdicombydicom")
    if not base_dir.exists():
        return {}
    
    real_results = {}
    for studies in STUDIES_LIST:
        std_dir = base_dir / f"{studies}std"
        benchmark_file = std_dir / "final_benchmark.csv"
        
        if not benchmark_file.exists():
            continue
            
        real_results[studies] = {}
        with benchmark_file.open("r") as fh:
            reader = csv.DictReader(fh)
            for row in reader:
                try:
                    w = int(row["Workers"])
                    t = float(row["Total Wall-Clock Time (s)"])
                    real_results[studies][w] = t
                except (ValueError, KeyError):
                    continue
    
    return real_results


def plot_simulations(results):
    """Create comparison plots of simulated results."""
    # Load real results for comparison
    real_results = load_real_results()
    
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    
    # Plot 1: Simulated execution time vs workers for each study count
    ax = axes[0, 0]
    for studies in STUDIES_LIST:
        workers_list = []
        times_list = []
        for workers in WORKERS_LIST:
            timing = results[studies].get(workers)
            if timing is not None:
                workers_list.append(workers)
                times_list.append(timing)
        if workers_list:
            ax.plot(workers_list, times_list, marker='o', label=f'{studies} studies', linewidth=2)
    
    ax.set_xlabel('Workers', fontsize=11)
    ax.set_ylabel('Execution time (s)', fontsize=11)
    ax.set_title('Simulated: Execution Time vs Workers', fontsize=11, fontweight='bold')
    ax.legend()
    ax.grid(True, linestyle='--', alpha=0.5)
    ax.set_xticks(WORKERS_LIST)
    
    # Plot 2: Real execution time vs workers (if available)
    ax = axes[0, 1]
    if real_results:
        for studies in STUDIES_LIST:
            if studies in real_results:
                workers_list = []
                times_list = []
                for workers in WORKERS_LIST:
                    timing = real_results[studies].get(workers)
                    if timing is not None:
                        workers_list.append(workers)
                        times_list.append(timing)
                if workers_list:
                    ax.plot(workers_list, times_list, marker='s', label=f'{studies} studies', linewidth=2)
        ax.set_xlabel('Workers', fontsize=11)
        ax.set_ylabel('Execution time (s)', fontsize=11)
        ax.set_title('Real Workflow: Execution Time vs Workers', fontsize=11, fontweight='bold')
        ax.legend()
        ax.grid(True, linestyle='--', alpha=0.5)
        ax.set_xticks(WORKERS_LIST)
    else:
        ax.text(0.5, 0.5, 'Real results not available', ha='center', va='center', transform=ax.transAxes)
        ax.set_title('Real Workflow: Not Available', fontsize=11, fontweight='bold')
    
    # Plot 3: Speedup - simulated (normalized to 1 worker)
    ax = axes[1, 0]
    for studies in STUDIES_LIST:
        workers_list = []
        speedup_list = []
        baseline = results[studies].get(1)
        if baseline is None:
            continue
        for workers in WORKERS_LIST:
            timing = results[studies].get(workers)
            if timing is not None:
                workers_list.append(workers)
                speedup_list.append(baseline / timing)
        if workers_list:
            ax.plot(workers_list, speedup_list, marker='o', label=f'{studies} studies', linewidth=2)
    
    # Ideal speedup line
    ax.plot(WORKERS_LIST, WORKERS_LIST, 'k--', alpha=0.5, label='Ideal speedup')
    
    ax.set_xlabel('Workers', fontsize=11)
    ax.set_ylabel('Speedup (vs 1 worker)', fontsize=11)
    ax.set_title('Simulated: Speedup vs Workers', fontsize=11, fontweight='bold')
    ax.legend()
    ax.grid(True, linestyle='--', alpha=0.5)
    ax.set_xticks(WORKERS_LIST)
    
    # Plot 4: Speedup - real (if available)
    ax = axes[1, 1]
    if real_results:
        for studies in STUDIES_LIST:
            if studies in real_results:
                workers_list = []
                speedup_list = []
                baseline = real_results[studies].get(1)
                if baseline is None:
                    continue
                for workers in WORKERS_LIST:
                    timing = real_results[studies].get(workers)
                    if timing is not None:
                        workers_list.append(workers)
                        speedup_list.append(baseline / timing)
                if workers_list:
                    ax.plot(workers_list, speedup_list, marker='s', label=f'{studies} studies', linewidth=2)
        
        # Ideal speedup line
        ax.plot(WORKERS_LIST, WORKERS_LIST, 'k--', alpha=0.5, label='Ideal speedup')
        ax.set_xlabel('Workers', fontsize=11)
        ax.set_ylabel('Speedup (vs 1 worker)', fontsize=11)
        ax.set_title('Real Workflow: Speedup vs Workers', fontsize=11, fontweight='bold')
        ax.legend()
        ax.grid(True, linestyle='--', alpha=0.5)
        ax.set_xticks(WORKERS_LIST)
    else:
        ax.text(0.5, 0.5, 'Real results not available', ha='center', va='center', transform=ax.transAxes)
        ax.set_title('Real Workflow: Not Available', fontsize=11, fontweight='bold')
    
    plt.tight_layout()
    plot_file = OUTPUT_BASE / "simulation_results.png"
    fig.savefig(plot_file, dpi=150)
    print(f"\nPlot saved to: {plot_file}")
    plt.close(fig)
    
    # If real results exist, create a separate comparison plot
    if real_results:
        create_comparison_plot(results, real_results)


def create_comparison_plot(sim_results, real_results):
    """Create side-by-side comparison of simulated vs real results."""
    fig, axes = plt.subplots(1, len(STUDIES_LIST), figsize=(my_width, my_width / golden))
    if len(STUDIES_LIST) == 1:
        axes = [axes]
        
    # Plot for each study count
    for idx, studies in enumerate(STUDIES_LIST):
        ax = axes[idx]
        x = np.arange(len(WORKERS_LIST))
        width = 0.35
        
        sim_times = []
        real_times = []
        for workers in WORKERS_LIST:
            st = sim_results[studies].get(workers)
            rt = real_results.get(studies, {}).get(workers)
            sim_times.append(st if st is not None else 0)
            real_times.append(rt if rt is not None else 0)
        
        bars1 = ax.bar(x - width/2, sim_times, width, label='Simulated', alpha=0.8)
        if any(real_times):
            bars2 = ax.bar(x + width/2, real_times, width, label='Real', alpha=0.8)
        
        #ax.set_xlabel('Workers', fontsize=11)
        #ax.set_ylabel('Execution time (s)', fontsize=11)
        ax.set_title(f'{studies} Studies', fontsize=12, fontweight='bold')
        ax.set_xticks(x)
        ax.set_xticklabels(WORKERS_LIST)
        #ax.legend()
        ax.grid(True, axis='y', linestyle='--', alpha=0.5)
    
    # 2. Collect all handles and labels from all axes
    handles, labels = [], []
    for ax in fig.axes:
        h, l = ax.get_legend_handles_labels()
        handles.extend(h)
        labels.extend(l)

    # 3. Remove duplicate labels (keeps the first occurrence)
    # Using a dictionary automatically drops duplicate keys
    by_label = dict(zip(labels, handles))

    # 4. Create the Figure-level legend
    fig.legend(by_label.values(), by_label.keys(), 
            loc='upper center', 
            bbox_to_anchor=(0.5, 1.1), # Positions it above the subplots
            ncol=2)                    # Spreads it horizontally

    #plt.suptitle('Simulated vs Real Workflow Execution Times', fontsize=13, fontweight='bold', y=1.02)
    fig.supylabel('Response time (s)', x=0.02)
    fig.supxlabel('Workers', y=0.08)
    plt.tight_layout()
    comp_file = OUTPUT_BASE / "simulation_vs_real_comparison.pdf"
    fig.savefig(comp_file, dpi=150, bbox_inches='tight')
    print(f"Comparison plot saved to: {comp_file}")
    plt.close(fig)


if __name__ == "__main__":
    sys.exit(main())
