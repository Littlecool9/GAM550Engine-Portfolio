"""Plot a recorded GPU float image; no terrain is reconstructed here."""
import argparse
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path, help="captured PNG with adjacent .depth.f32/.depth.json")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    meta = json.loads(Path(str(args.capture) + ".depth.json").read_text(encoding="utf-8"))
    data = np.fromfile(str(args.capture) + ".depth.f32", dtype="<f4")
    data = data.reshape(meta["height"], meta["width"], meta["components"])
    values = data[:, :, 0]
    visible = np.ma.masked_where(values == 0, values)
    fig, axis = plt.subplots(figsize=(9, 5.5), layout="constrained")
    image = axis.imshow(visible, cmap="viridis")
    label = "D32F reverse-Z depth" if meta["components"] == 1 else "GPU ray hit distance (m)"
    fig.colorbar(image, ax=axis, label=label)
    axis.set_title(args.capture.stem + "\n" + meta["origin"])
    axis.set_xlabel("pixel x")
    axis.set_ylabel("pixel y")
    fig.savefig(args.out, dpi=140)
    plt.close(fig)


if __name__ == "__main__":
    main()
