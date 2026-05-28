#!/bin/bash

# Default Configuration
STUDIES=${1:-10}
WORKERS=${2:-4}
OUTPUT_DIR=${3:-"."}
PARTITION="large"

# Clean up local logs if not sandboxed
if [ "$OUTPUT_DIR" = "." ]; then
    rm -rf tmp_edge tmp_fog tmp_cloud workflow_timing.log slurm-*.out
fi

# Ensure output directory exists
mkdir -p "$OUTPUT_DIR"

STUDIES_DIR="/lustre/uc3m_a0/dynamic/dantedomizzi/dicoms/"

# 1. Deploy EDGE Node
EDGE_JOB=$(sbatch --parsable --partition=$PARTITION --nodes=1 -c $WORKERS --wrap="python3 workflow_multiprocessing.py --stage edge --studies $STUDIES --workers $WORKERS --dataset $STUDIES_DIR --output_dir $OUTPUT_DIR")

# 2. Deploy FOG Node
FOG_JOB=$(sbatch --parsable --partition=$PARTITION --nodes=1 -c $WORKERS --dependency=afterok:$EDGE_JOB --wrap="python3 workflow_multiprocessing.py --stage fog --studies $STUDIES --workers $WORKERS --dataset $STUDIES_DIR --output_dir $OUTPUT_DIR")

# 3. Deploy CLOUD Node
CLOUD_JOB=$(sbatch --parsable --partition=$PARTITION --nodes=1 -c $WORKERS --dependency=afterok:$FOG_JOB --wrap="python3 workflow_multiprocessing.py --stage cloud --studies $STUDIES --workers $WORKERS --dataset $STUDIES_DIR --output_dir $OUTPUT_DIR")

# OUTPUT ONLY THE CLOUD JOB ID FOR AUTOMATION (Do not add extra echos to stdout)
echo $CLOUD_JOB
