#!/usr/bin/env python3
# plot.py — generates three charts from results/data.csv
# Install deps: pip3 install matplotlib pandas

import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import os

CSV_PATH = os.path.join(os.path.dirname(__file__), "data.csv")
OUT_DIR  = os.path.dirname(__file__)

# Color palette — one color per mechanism
COLORS = {
    "pipe"     : "#4C72B0",
    "fifo"     : "#DD8452",
    "mq"       : "#55A868",
    "shm"      : "#C44E52",
    "rdma_shm" : "#8172B2",
    "unix"     : "#937860",
}

def load():
    df = pd.read_csv(CSV_PATH)
    df.columns = df.columns.str.strip()
    return df

# ── Chart 1: p50 latency at 64B for all mechanisms ────────────────────────────
def chart_latency_bar(df):
    data = df[(df["mode"] == "latency") & (df["msg_size"] == 64)].copy()
    data = data.sort_values("p50_ns")

    fig, ax = plt.subplots(figsize=(9, 5))
    bars = ax.bar(
        data["mechanism"],
        data["p50_ns"],
        color=[COLORS.get(m, "#888") for m in data["mechanism"]],
        edgecolor="white",
        linewidth=0.8,
        width=0.6,
    )

    # Annotate each bar with its value
    for bar, val in zip(bars, data["p50_ns"]):
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            bar.get_height() + 20,
            f"{val:.0f} ns",
            ha="center", va="bottom", fontsize=9, fontweight="bold"
        )

    ax.set_title("p50 One-Way Latency at 64B Message Size", fontsize=13,
                 fontweight="bold", pad=12)
    ax.set_ylabel("Latency (ns)", fontsize=11)
    ax.set_xlabel("IPC Mechanism", fontsize=11)
    ax.yaxis.set_minor_locator(ticker.AutoMinorLocator())
    ax.grid(axis="y", linestyle="--", alpha=0.4)
    ax.set_axisbelow(True)

    # Highlight rdma_shm as the winner
    for bar, mech in zip(bars, data["mechanism"]):
        if mech == "rdma_shm":
            bar.set_edgecolor("#FFD700")
            bar.set_linewidth(2.5)

    plt.tight_layout()
    path = os.path.join(OUT_DIR, "chart1_latency_bar.png")
    plt.savefig(path, dpi=150)
    print(f"Saved {path}")
    plt.close()

# ── Chart 2: throughput (MB/s) vs message size for all mechanisms ─────────────
def chart_throughput_line(df):
    data = df[df["mode"] == "throughput"].copy()
    data["throughput_mbps"] = pd.to_numeric(data["throughput_mbps"],
                                             errors="coerce")
    data["msg_size"] = pd.to_numeric(data["msg_size"])
    data = data.dropna(subset=["throughput_mbps"])

    fig, ax = plt.subplots(figsize=(9, 5))

    for mech, grp in data.groupby("mechanism"):
        grp = grp.sort_values("msg_size")
        ax.plot(
            grp["msg_size"], grp["throughput_mbps"],
            marker="o", linewidth=2, markersize=6,
            label=mech, color=COLORS.get(mech, "#888")
        )
    

    ax.set_xscale("log", base=2)
    ax.set_xticks([64, 1024, 4096])
    ax.set_xticklabels(["64B", "1KB", "4KB"])
    ax.set_title("Throughput vs Message Size", fontsize=13,
                 fontweight="bold", pad=12)
    ax.set_ylabel("Throughput (MB/s)", fontsize=11)
    ax.set_xlabel("Message Size", fontsize=11)
    ax.legend(loc="upper left", fontsize=9)
    ax.grid(linestyle="--", alpha=0.4)
    ax.set_axisbelow(True)

    plt.tight_layout()
    path = os.path.join(OUT_DIR, "chart2_throughput_line.png")
    plt.savefig(path, dpi=150)
    print(f"Saved {path}")
    plt.close()

# ── Chart 3: rdma_shm latency p50/p99/p999 across message sizes ───────────────
def chart_rdma_percentiles(df):
    data = df[(df["mode"] == "latency") &
              (df["mechanism"] == "rdma_shm")].copy()
    data["msg_size"] = pd.to_numeric(data["msg_size"])
    data = data.sort_values("msg_size")

    fig, ax = plt.subplots(figsize=(9, 5))

    sizes  = data["msg_size"].tolist()
    labels = [f"{s}B" if s < 1024 else f"{s//1024}KB" for s in sizes]
    x      = range(len(sizes))

    ax.plot(x, data["p50_ns"],  marker="o", linewidth=2,
            label="p50",  color="#8172B2")
    ax.plot(x, data["p99_ns"],  marker="s", linewidth=2,
            label="p99",  color="#C44E52")
    ax.plot(x, data["p999_ns"], marker="^", linewidth=2,
            label="p999", color="#DD8452")

        
    ax.set_yscale("log")
    ax.set_ylabel("Latency (ns) — log scale", fontsize=11)

    ax.set_xticks(list(x))
    ax.set_xticklabels(labels)
    ax.set_title("RDMA-style SHM: Latency Percentiles vs Message Size",
                 fontsize=13, fontweight="bold", pad=12)
    ax.set_ylabel("Latency (ns)", fontsize=11)
    ax.set_xlabel("Message Size", fontsize=11)
    ax.legend(fontsize=10)
    ax.grid(linestyle="--", alpha=0.4)
    ax.set_axisbelow(True)

    plt.tight_layout()
    path = os.path.join(OUT_DIR, "chart3_rdma_percentiles.png")
    plt.savefig(path, dpi=150)
    print(f"Saved {path}")
    plt.close()

if __name__ == "__main__":
    df = load()
    chart_latency_bar(df)
    chart_throughput_line(df)
    chart_rdma_percentiles(df)
    print("All charts generated.")
