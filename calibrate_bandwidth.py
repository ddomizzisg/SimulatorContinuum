#!/usr/bin/env python3
import json
from pathlib import Path

CONFIG_PATH = Path("proxy_dd/ct_scan_pipeline_config_calibrated_dicom_by_dicom.json")

# From manual calculations based on single-worker real IO times vs simulated IO times:
# Edge real IO / obj = 0.05043s | Sim IO / obj = 0.00429s -> Scale = 0.085
# Fog real IO / obj = 0.02403s | Sim IO / obj = 0.00567s -> Scale = 0.235
# Cloud real IO / obj = 0.00843s | Sim IO / obj = 0.00179s -> Scale = 0.212

scales = {
    "edge_acquisition": 0.085,
    "fog_preprocessing": 0.235,
    "cloud_inference": 0.212
}

def main():
    with CONFIG_PATH.open("r") as f:
        config = json.load(f)
        
    for stage in config.get("stages", []):
        name = stage.get("name")
        if name in scales:
            scale = scales[name]
            old_bfs = stage.get("b_fs", 800.0)
            new_bfs = old_bfs * scale
            stage["b_fs"] = round(new_bfs, 2)
            stage["b_fs_read"] = round(new_bfs, 2)
            stage["b_fs_write"] = round(new_bfs, 2)
            print(f"Calibrated {name} b_fs: {old_bfs} -> {round(new_bfs, 2)}")
            
    # Save it
    with CONFIG_PATH.open("w") as f:
        json.dump(config, f, indent=4)
        
    print(f"Successfully updated {CONFIG_PATH}")

if __name__ == "__main__":
    main()
