# Calibrated Pipeline Simulation Script

This script executes CT scan pipeline simulations that match the real workflow experiments, using measured timing data for calibration. It compares simulated results against real workflow execution times.

## Overview

- **Purpose**: Run simulated CT pipeline experiments using calibrated stage timings, then generate comparative plots and analysis
- **Configuration**: Workers: [1, 2, 4, 8, 16] | Studies: [1, 10, 100]
- **Output**: Execution times, error analysis, and visualization plots

## Running the Script

```bash
python3 run_calibrated_simulations.py
```

The script will:
1. Verify required files (calibration CSV, dataset, simulator executable)
2. Run all 15 experiment combinations (5 worker configs × 3 study counts)
3. Extract timing results from simulator outputs
4. Compare against real workflow results
5. Generate summary tables and plots
6. Calculate error percentages

## Output Files

Generated in `simulated_pipeline_results/`:

- **simulation_results.csv** - Raw timing data (seconds)
- **simulation_results.png** - 4-panel plot showing:
  - Simulated execution time vs workers
  - Real workflow execution time vs workers
  - Simulated speedup curves
  - Real workflow speedup curves
- **simulation_vs_real_comparison.png** - Side-by-side bar charts comparing simulated vs real times for each study count
- **workers_X_studies_Y/**: Individual experiment directories containing:
  - `ct_scan_pipeline_config.json` - Simulation configuration
  - `results/stage_totals_by_workers.csv` - Per-stage timing breakdown
  - `results/link_metrics.csv` - Network link statistics
  - `results/manager_metrics.csv` - System metrics
  - `simulator_stdout.txt` - Simulator console output

## Configuration Parameters

Edit these variables in `run_calibrated_simulations.py`:

```python
WORKERS_LIST = [1, 2, 4, 8, 16]          # Number of workers per stage
STUDIES_LIST = [1, 10, 100]              # Number of studies to simulate
CALIBRATION_FILE = Path(...)             # CSV with measured stage timings
DATASET = Path.home() / "Downloads" / ... # Reference dataset
OUTPUT_BASE = Path("...")                 # Output directory
SIMULATOR = Path("...")                   # Path to simulator executable
HARDWARE_PROFILE = "c3"                   # Hardware profile
```

## Calibration File Format

The calibration file must be a CSV with columns `task,duration_seconds`:

```csv
task,duration_seconds
edge_acquisition,5.2649
fog_preprocessing,6.4465
cloud_inference,6.3912
```

Measured times are used as per-study durations and automatically divided by object count for per-object service times.

## Results Interpretation

The output console shows three tables:

1. **Simulated Execution times** - Results from the proxy_dd simulator
2. **Real Workflow Execution times** - From CaseStudyBoneTumore experiments
3. **Simulation Error (% difference)** - Percentage error between simulated and real

Positive error values indicate the simulator overestimated execution time; negative values indicate underestimation.

## Example Output

```
Simulated Execution times (seconds):
   Workers              1             10            100
-------------------------------------------------------
         1         127.81        1715.52       17779.95
         2          71.29         811.86        8932.18
         4          36.37         360.87        4553.72
         8          16.10         169.68        2162.15
        16          11.08          87.40        1022.75

Real Workflow Execution times (seconds):
   Workers              1             10            100
-------------------------------------------------------
         1          28.26         125.11        1251.25
         2          23.12          86.61         693.71
         4          24.26          56.83         438.84
         8          23.40          46.02         344.86
        16          23.97          43.96         274.94

Simulation Error (% difference from real):
   Workers              1             10            100
-------------------------------------------------------
         1         352.3%        1271.2%        1321.0%
         2         208.4%         837.4%        1187.6%
         4          49.9%         535.0%         937.7%
         8         -31.2%         268.7%         527.0%
        16         -53.8%          98.8%         272.0%
```

## Notes

- Each simulation takes ~2 seconds; total runtime ~40-60 seconds depending on system load
- The simulator models queueing effects, network transfers, and security requirements
- Real results are loaded from `CaseStudyBoneTumore/resultadostiempos/output.txt`
- All intermediate results (stage timings, logs) are preserved for further analysis
