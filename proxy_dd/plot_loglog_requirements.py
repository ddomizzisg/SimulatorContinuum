import pandas as pd
import matplotlib.pyplot as plt
import os

reqs = [
    {
        'title': 'Cost-Efficiency (Compression)',
        'file': 'cost-efficiency.csv',
        'algo_col': 'algoritmo',
        'time_col': 'avg_comp_s'
    },
    {
        'title': 'Integrity (Hashing)',
        'file': 'integrity.csv',
        'algo_col': 'algoritmo',
        'time_col': 'avg_time_seconds'
    },
    {
        'title': 'Confidentiality (Encryption)',
        'file': 'confidentiality.csv',
        'algo_col': 'algorithm',
        'time_col': 'avg_encryption_s'
    },
    {
        'title': 'Reliability (Erasure Coding)',
        'file': 'reliability.csv',
        'algo_col': 'algoritmo',
        'time_col': 'avg_encoding_s'
    }
]

pt = 1./72.27
jour_sizes = {"PRD": {"onecol": 246.*pt, "twocol": 510.*pt},
              "CQG": {"onecol": 374.*pt}, }
my_width = jour_sizes["PRD"]["twocol"]
golden = (1 + 5 ** 0.5) / 2.2
# golden = (1 + 5 ** 0.5) / 2.4 # you can modify here for a higher height if needed
plt.rcParams.update({
    'axes.labelsize': 14,       # Axis label font size
    'legend.fontsize': 10,      # Legend font size
    'xtick.labelsize': 12,      # X-axis tick label font size
})


# Adjust this path based on where the script is run
base_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "results_different_machines/organized/c3/real_values")

fig, axes = plt.subplots(2, 2, figsize=(my_width, my_width / golden), sharey=False)
axes = axes.flatten()

for i, req in enumerate(reqs):
    file_path = os.path.join(base_dir, req['file'])
    df = pd.read_csv(file_path)
    
    algo_col = req['algo_col']
    time_col = req['time_col']
    title = req['title']
    ax = axes[i]
    
    # We group by algorithm and size_mb
    df_plot = df.groupby([algo_col, 'size_mb']).mean(numeric_only=True).reset_index()
    
    for algo in df_plot[algo_col].unique():
        subset = df_plot[df_plot[algo_col] == algo].sort_values('size_mb')
        x = subset['size_mb']
        y = subset[time_col]
        
        # Log-Log plot
        ax.plot(x, y, marker='o', label=algo)

    # Configure log-log plot
    #ax.set_title(title)
    #ax.set_xlabel("Size (MB)")
    #ax.set_ylabel("Response Time (s)")
    ax.set_xscale('log')
    ax.set_yscale('log')
    ax.legend()
    ax.grid(True, which="both", ls="-", alpha=0.3)

fig.supylabel("Response Time (seconds)", fontsize=14)
fig.supxlabel("Workload Size (MB)", fontsize=14)
plt.tight_layout()
out_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), "all_requirements_loglog.pdf")
plt.savefig(out_file, dpi=300)
print(f"Plot saved to {out_file}")
