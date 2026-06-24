# Calibrated Simulation Results Summary

## Project Overview

This project calibrates the CT scan pipeline simulator using measured workflow timings and compares simulated performance against real Slurm-based workflow executions.

## What Was Done

1. **Calibration**: Modified `ct_scan_pipeline.py` to accept a `--calibration-file` parameter that loads measured stage timings from a CSV file and applies them to override default reference timings.

2. **Script Creation**: Created `run_calibrated_simulations.py` that:
   - Runs 15 simulation experiments (5 worker configs × 3 study counts)
   - Extracts timing data from simulator outputs
   - Compares against real workflow results from `CaseStudyBoneTumore/resultadostiempos/output.txt`
   - Generates comparative plots and error analysis

3. **Visualization**: Generated four comprehensive plots:
   - Simulated execution time vs workers
   - Real workflow execution time vs workers
   - Simulated speedup curves
   - Real workflow speedup curves
   - Side-by-side bar chart comparison

## Key Findings

### Execution Time Comparison

| Workers | 1 Study | 10 Studies | 100 Studies |
|---------|---------|-----------|------------|
| **Simulated** (1 worker) | 127.8s | 1,715.5s | 17,779.9s |
| **Real** (1 worker) | 28.3s | 125.1s | 1,251.3s |
| **Error** | +352% | +1,271% | +1,321% |

### Key Observations

1. **Significant Over-Estimation**: The simulator consistently overestimates execution times, with errors ranging from -54% (16 workers, 1 study) to +1,321% (1 worker, 100 studies).

2. **Parallelism Behavior**:
   - **Simulated**: Approaches near-linear speedup with additional workers
   - **Real**: Shows sublinear speedup, indicating bottlenecks in the real workflow
   - The simulator appears more optimistic about parallelism benefits

3. **Scaling Patterns**:
   - **Single Worker**: Simulator 4.5× slower than reality
   - **Multiple Workers**: Gaps narrow significantly (simulator becomes faster at 16 workers for single study)
   - **More Studies**: The relative error increases, suggesting different scaling characteristics

### Calibration Issues

The calibration was derived from a single-worker, single-study scenario:
- **Source**: `workers_1_studies_1_workflow_timing.log`
- **Timings Used**:
  - edge_acquisition: 5.26 seconds
  - fog_preprocessing: 6.45 seconds
  - cloud_inference: 6.39 seconds

These values capture only the application stage times, not queueing, contention, or scheduling effects that may become more prominent with multiple workers and studies.

## Files Generated

### Output Directory: `simulated_pipeline_results/`

- **simulation_results.csv**: Raw timing data
- **simulation_results.png**: 4-panel comparative analysis plot
- **simulation_vs_real_comparison.png**: Side-by-side bar chart comparison
- **workers_X_studies_Y/**: Individual experiment directories with:
  - Configuration used
  - Per-stage timing breakdown
  - Network link metrics
  - System metrics
  - Simulator logs

## Usage

Run the simulation script:

```bash
python3 run_calibrated_simulations.py
```

To modify parameters (worker counts, studies, calibration file), edit the configuration variables at the top of `run_calibrated_simulations.py`.

## Recommendations

1. **Model Validation**: Investigate why the simulator overestimates. Potential causes:
   - Excessive queueing delays in simulator
   - Different task scheduling assumptions
   - Mismodeled network or storage contention
   - Different parallelism efficiency modeling

2. **Improved Calibration**: Consider calibrating from multi-worker scenarios to capture scalability behavior better.

3. **Parameter Tuning**: Adjust simulator parameters (buffer sizes, scheduling policies, network model) to better match observed real-world behavior.

4. **Sensitivity Analysis**: Run the sensitivity analysis script to understand which simulator parameters have the most impact.

## Related Files

- **Script**: `run_calibrated_simulations.py` - Main execution script
- **Documentation**: `RUN_CALIBRATED_SIMULATIONS_README.md` - Detailed usage guide
- **Pipeline**: `proxy_dd/ct_scan_pipeline.py` - Calibration-enabled simulator config generator
- **Real Results**: `CaseStudyBoneTumore/resultadostiempos/output.txt` - Source of real workflow timings
- **Calibration Data**: `CaseStudyBoneTumore/resultadostiempos/workers_1_studies_1_workflow_timing.log` - Per-task measurements

## Next Steps

1. Analyze the specific bottlenecks causing over-estimation
2. Run sensitivity analysis on key simulator parameters
3. Consider multi-worker calibration scenarios
4. Validate simulator model against different hardware profiles
5. Document any systematic biases in simulator predictions
