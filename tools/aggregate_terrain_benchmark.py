"""Aggregate engine terrain traces with nearest-rank percentiles and explicit censored requests."""

import argparse
import json
import math
from collections import defaultdict
from pathlib import Path


def distribution(values):
    values = sorted(float(v) for v in values if math.isfinite(v))
    if not values:
        return {"count": 0, "p50": None, "p95": None, "p99": None, "max": None}
    return {"count": len(values), **{
        f"p{p}": values[max(0, math.ceil(len(values) * p / 100) - 1)] for p in (50, 95, 99)
    }, "max": values[-1]}


def aggregate(trace, spike_ms):
    if trace.get("schemaVersion") != 1:
        raise ValueError("unsupported trace schema")
    gpu = {r["serial"]: r for r in trace["gpuFrames"]}
    groups = defaultdict(list)
    previous_waits = trace.get("initialWaitCounts", {"device": 0, "queue": 0})
    for row in trace["frames"]:
        row["idleWaitDelta"] = {"device": row["upload"]["deviceWaits"] - previous_waits["device"],
                               "queue": row["upload"]["queueWaits"] - previous_waits["queue"]}
        previous_waits = {"device": row["upload"]["deviceWaits"], "queue": row["upload"]["queueWaits"]}
        groups[row["route"]].append(row)
    routes = {}
    for name, rows in groups.items():
        available = [gpu[r["gpuSerial"]] for r in rows if r["gpuSerial"] in gpu]
        samples = defaultdict(list)
        canceled = defaultdict(int)
        for sample in trace["samples"]:
            if sample["route"] != name:
                continue
            if sample["success"]:
                samples[sample["metric"]].append(sample["ms"])
            else:
                canceled[sample["metric"]] += 1
        missing = [sum(v["missing"] for v in r["views"]) for r in rows]
        first_covered = next((i for i, r in enumerate(rows) if r["views"] and missing[i] == 0), None)
        peaks = {
            "cpuPayloadBytes": max(r.get("cpu", {}).get("accountedBytes", 0) for r in rows),
            "gpuMeshesAndRetirementBytes": max(r.get("meshes", {}).get("accountedBytes", 0) for r in rows),
            "pendingUploadBytes": max(r["upload"]["pendingBytes"] for r in rows),
            "uploadingBytes": max(r["upload"]["uploadingBytes"] for r in rows),
            "retiringBytes": max(r["upload"]["retiringBytes"] for r in rows),
            "stagingUsedBytes": max(r["upload"]["stagingUsedBytes"] for r in rows),
            "stagingCapacityBytes": max(r["upload"]["stagingCapacityBytes"] for r in rows),
            "gpuMeshesRetirementAndStagingCapacityBytes": max(
                r.get("meshes", {}).get("accountedBytes", 0) + r["upload"]["stagingCapacityBytes"] for r in rows),
            "collisionBytes": max(r.get("collision", {}).get("heldBytes", 0) for r in rows),
            "processBytes": max(r["processBytes"] for r in rows),
            "deviceAllocatedBytes": max(r["deviceAllocatedBytes"] for r in rows),
        }
        overlap = [r for r in rows if any(a["staged"] for a in r["releases"])]
        peaks["releaseOverlapCpuBytes"] = max((sum(a["cpuBytes"] for a in r["releases"]) for r in overlap), default=0)
        peaks["releaseOverlapGpuBytes"] = max((sum(a["gpuBytes"] for a in r["releases"]) for r in overlap), default=0)
        peaks["releaseOverlapCollisionBytes"] = max((sum(a["collisionBytes"] for a in r["releases"]) for r in overlap), default=0)
        routes[name] = {
            "frames": len(rows),
            "cpuFrameMs": distribution([r["cpuFrameMs"] for r in rows]),
            "intervalMs": distribution([r["intervalMs"] for r in rows]),
            "gpuFrameMs": distribution([r["gpuMs"] for r in available]),
            "gpuUploadMs": distribution([r["uploadMs"] for r in available]),
            "primaryDrawCalls": distribution([r["primaryDrawCalls"] for r in available]),
            "terrainDrawUnits": distribution([sum(v["drawUnits"] for v in r["views"]) for r in rows]),
            "telemetryMs": distribution([r["telemetryMs"] for r in rows]),
            "streamerUpdateMs": distribution([r.get("cpu", {}).get("updateMs", 0) for r in rows]),
            "meshRequestMs": distribution([r.get("meshes", {}).get("requestMs", 0) for r in rows]),
            "timingsMs": {metric: distribution(values) for metric, values in sorted(samples.items())},
            "failedOrCanceledSamples": dict(canceled),
            "missingFrames": sum(v > 0 for v in missing),
            "firstFullyCoveredFrame": first_covered,
            "missingFramesAfterFirstCover": sum(v > 0 for v in missing[first_covered + 1:]) if first_covered is not None else None,
            "maxMissingAreas": max(missing),
            "duplicatedTiles": sum(v["duplicatedTiles"] for r in rows for v in r["views"]),
            "mainThreadRangeReads": max(r["mainThreadRangeReads"] for r in rows),
            "idleWaitCalls": {kind: sum(r["idleWaitDelta"][kind] for r in rows) for kind in ("device", "queue")},
            "movementIdleWaitCalls": {kind: sum(r["idleWaitDelta"][kind] for r in rows if not r["boundary"]) for kind in ("device", "queue")},
            "heldBodyFrames": sum(r.get("collision", {}).get("heldBodies", 0) > 0 for r in rows),
            "collisionPendingSourceFrames": sum(any(v["pending"] > 0 for v in r["capabilityProbes"]) for r in rows),
            "maxContactError": max((v["contactError"] for r in rows for v in r["capabilityProbes"]), default=0),
            "peaks": peaks,
            "spikes": [{"frame": r["frame"], "cpuMs": r["cpuFrameMs"], "boundary": r["boundary"],
                        "gpuMs": gpu.get(r["gpuSerial"], {}).get("gpuMs")} for r in rows
                       if r["cpuFrameMs"] > spike_ms or gpu.get(r["gpuSerial"], {}).get("gpuMs", 0) > spike_ms],
        }
    timings = defaultdict(list)
    failures = defaultdict(int)
    for s in trace["samples"]:
        if s["success"]:
            timings[s["metric"]].append(s["ms"])
        else:
            failures[s["metric"]] += 1
    return {
        "completed": trace["completed"], "buildConfiguration": trace["buildConfiguration"],
        "plan": trace["plan"], "gpu": trace["gpu"], "maps": trace["maps"], "settings": trace["settings"],
        "percentileMethod": "nearest-rank", "spikeThresholdMs": spike_ms,
        "pendingRequests": trace["pendingRequests"], "failedOrCanceledSamples": dict(failures),
        "timingsMs": {name: distribution(v) for name, v in sorted(timings.items())},
        "routes": routes,
    }


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("traces", nargs="+", type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--spike-ms", required=True, type=float)
    args = parser.parse_args()
    if not math.isfinite(args.spike_ms) or args.spike_ms <= 0:
        parser.error("--spike-ms must be a positive reporting threshold")
    summary = {str(path): aggregate(json.loads(path.read_text(encoding="utf-8-sig")), args.spike_ms) for path in args.traces}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(summary, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")
    for path, report in summary.items():
        print(path)
        for route, row in report["routes"].items():
            print(f"  {route}: CPU P95/P99/max={row['cpuFrameMs']['p95']:.3f}/{row['cpuFrameMs']['p99']:.3f}/{row['cpuFrameMs']['max']:.3f} ms, "
                  f"missing={row['missingFrames']}, duplicates={row['duplicatedTiles']}, main IO={row['mainThreadRangeReads']}")


if __name__ == "__main__":
    main()
