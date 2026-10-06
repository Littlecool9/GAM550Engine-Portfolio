"""Render the experiment CSV using matplotlib (kept out of the engine build)."""
import argparse
import csv
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("evidence", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

with (args.evidence / "curves.csv").open(newline="") as stream:
    rows = list(csv.DictReader(stream))

def curve(case, kind, weight="smoothstep", width="2"):
    selected = [r for r in rows if r["case"] == case and r["curve"] == kind and
                r["weight"] == weight and r["bandMultiplier"] == width]
    return [float(r["z" if kind == "east-border" else "x"]) for r in selected], \
           [float(r["raw"]) for r in selected], [float(r["final"]) for r in selected]

fig, axes = plt.subplots(2, 2, figsize=(12, 8), constrained_layout=True)
for ax, case, kind, title in [
    (axes[0, 0], "mixed-four", "east-border", "Mixed 1/2/4/8 m: 1 m east boundary"),
    (axes[0, 1], "mixed-four", "cross-section", "Mixed four: cross-section through fine-tile maximum"),
    (axes[1, 0], "same-1-corner-8", "east-border", "Two 1 m neighbours, incident 8 m corner")]:
    position, raw, final = curve(case, kind)
    ax.plot(position, raw, label="raw source", linewidth=1.3)
    ax.plot(position, final, label="final float32", linewidth=1.2, linestyle="--")
    ax.set_title(title, fontsize=11)
    ax.set_xlabel("z (m)" if kind == "east-border" else "x (m)")
    ax.set_ylabel("Height (m)")
    ax.grid(alpha=.25)
    ax.legend(fontsize=9)
for axis, case, kind, left in [(axes[0, 1], "mixed-four", "cross-section", -40),
                                (axes[1, 0], "same-1-corner-8", "east-border", -20)]:
    position, raw, final = curve(case, kind)
    visible = [v for x, a, b in zip(position, raw, final) if left <= x <= 0 for v in (a, b)]
    padding = max((max(visible) - min(visible)) * .08, .005)
    axis.set_xlim(left, 0)
    axis.set_ylim(min(visible) - padding, max(visible) + padding)
ax = axes[1, 1]
for weight, style in [("linear", "--"), ("smoothstep", "-")]:
    for width in ("1", "2", "4"):
        position, raw, final = curve("direct-8", "cross-section", weight, width)
        ax.plot(position, [b - a for a, b in zip(raw, final)], linestyle=style,
                label=f"{weight}, {width} x spacing", linewidth=1.2)
ax.set_xlim(-40, 0)
ax.set_title("Direct 1 to 8 m: width / weight comparison", fontsize=11)
ax.set_xlabel("x (m)")
ax.set_ylabel("Final minus raw (m)")
ax.grid(alpha=.25)
ax.legend(fontsize=8, ncols=2)
fig.suptitle("Terrain P2 transition measurements | 256 m tiles, fixed 1 m reference", fontsize=13)
args.output.parent.mkdir(parents=True, exist_ok=True)
fig.savefig(args.output, dpi=160)
print(args.output)
