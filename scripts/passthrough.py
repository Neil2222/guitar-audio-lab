# /// script
# requires-python = ">=3.12"
# dependencies = ["sounddevice", "numpy"]
# ///
"""Fase 2: minimale realtime passthrough Rocksmith -> S/PDIF.

Eén full-duplex PortAudio-stream (input- en outputdevice mogen verschillen
binnen dezelfde host-API). De callback kopieert alleen mono -> stereo.

Gebruik:
  uv run scripts/passthrough.py                          # shared, blocksize 256, 10 s
  uv run scripts/passthrough.py --mode exclusive --blocksize 128 --seconds 15
  uv run scripts/passthrough.py --bench                  # matrix van configuraties
"""

import argparse
import time

import numpy as np
import sounddevice as sd

SAMPLERATE = 48000
IN_NAME = "rocksmith"
OUT_NAME = "realtek digital"

HOST_APIS = {"wasapi": "Windows WASAPI", "wdmks": "Windows WDM-KS"}
HOST = "wasapi"  # gezet via --host
OUT_NAME_PER_HOST = {"wasapi": "realtek digital", "wdmks": "spdif"}


def find_wasapi_device(name_part: str, kind: str) -> int:
    for idx, dev in enumerate(sd.query_devices()):
        if sd.query_hostapis(dev["hostapi"])["name"] != HOST_APIS[HOST]:
            continue
        if name_part not in dev["name"].lower():
            continue
        if kind == "input" and dev["max_input_channels"] > 0:
            return idx
        if kind == "output" and dev["max_output_channels"] > 0:
            return idx
    raise SystemExit(f"WASAPI-device niet gevonden: {name_part!r} ({kind})")


class Stats:
    def __init__(self) -> None:
        self.callbacks = 0
        self.input_overflows = 0
        self.output_underflows = 0
        self.other_flags = 0
        self.peak = 0.0
        self.rms_acc = 0.0
        self.last_t = None
        self.intervals: list[float] = []

    def note(self, status: sd.CallbackFlags, block: np.ndarray) -> None:
        self.callbacks += 1
        if status.input_overflow:
            self.input_overflows += 1
        if status.output_underflow:
            self.output_underflows += 1
        if status.input_underflow or status.output_overflow or status.priming_output:
            self.other_flags += 1
        peak = float(np.max(np.abs(block)))
        if peak > self.peak:
            self.peak = peak
        self.rms_acc += float(np.mean(block * block))
        now = time.perf_counter()
        if self.last_t is not None:
            self.intervals.append(now - self.last_t)
        self.last_t = now


def run_once(mode: str, blocksize: int, seconds: float, gain: float,
             quiet: bool = False) -> dict | None:
    in_dev = find_wasapi_device(IN_NAME, "input")
    out_dev = find_wasapi_device(OUT_NAME_PER_HOST[HOST], "output")

    if mode == "exclusive" and HOST == "wasapi":
        extra = (sd.WasapiSettings(exclusive=True), sd.WasapiSettings(exclusive=True))
    else:
        extra = None

    stats = Stats()

    def callback(indata, outdata, frames, time_info, status):
        outdata[:] = indata * gain  # mono (n,1) broadcast naar stereo (n,2)
        stats.note(status, indata)

    try:
        stream = sd.Stream(
            device=(in_dev, out_dev),
            samplerate=SAMPLERATE,
            blocksize=blocksize,
            dtype="float32",
            channels=(1, 2),
            latency="low",
            callback=callback,
            extra_settings=extra,
        )
    except Exception as e:
        if not quiet:
            print(f"  Stream openen mislukt ({mode}, bs={blocksize}): {e}")
        return None

    with stream:
        lat_in, lat_out = stream.latency
        deadline = time.time() + seconds
        while time.time() < deadline:
            time.sleep(0.25)
        cpu = stream.cpu_load

    expected = blocksize / SAMPLERATE
    iv = np.array(stats.intervals) if stats.intervals else np.array([expected])
    late = int(np.sum(iv > 1.5 * expected))
    rms = (stats.rms_acc / max(stats.callbacks, 1)) ** 0.5
    result = {
        "mode": mode,
        "blocksize": blocksize,
        "buffer_ms": expected * 1000,
        "lat_in_ms": lat_in * 1000,
        "lat_out_ms": lat_out * 1000,
        "total_reported_ms": (lat_in + lat_out) * 1000,
        "callbacks": stats.callbacks,
        "in_overflow": stats.input_overflows,
        "out_underflow": stats.output_underflows,
        "other_flags": stats.other_flags,
        "late_callbacks": late,
        "cb_interval_p99_ms": float(np.percentile(iv, 99)) * 1000,
        "cb_interval_max_ms": float(np.max(iv)) * 1000,
        "cpu_load_pct": cpu * 100,
        "peak_dbfs": 20 * np.log10(stats.peak) if stats.peak > 0 else float("-inf"),
        "rms_dbfs": 20 * np.log10(rms) if rms > 0 else float("-inf"),
    }
    if not quiet:
        print_result(result)
    return result


def print_result(r: dict) -> None:
    print(f"\n--- {r['mode']} | blocksize {r['blocksize']} "
          f"({r['buffer_ms']:.2f} ms/blok) ---")
    print(f"  PortAudio-latency  in={r['lat_in_ms']:.1f} ms  "
          f"out={r['lat_out_ms']:.1f} ms  totaal={r['total_reported_ms']:.1f} ms")
    print(f"  callbacks={r['callbacks']}  overflow={r['in_overflow']}  "
          f"underflow={r['out_underflow']}  overig={r['other_flags']}  "
          f"laat(>1.5x)={r['late_callbacks']}")
    print(f"  callback-interval p99={r['cb_interval_p99_ms']:.2f} ms  "
          f"max={r['cb_interval_max_ms']:.2f} ms  cpu={r['cpu_load_pct']:.1f}%")
    print(f"  signaal peak={r['peak_dbfs']:.1f} dBFS  rms={r['rms_dbfs']:.1f} dBFS")


def bench(seconds: float, gain: float) -> None:
    configs = [("shared", 512), ("shared", 256), ("shared", 128),
               ("exclusive", 256), ("exclusive", 128), ("exclusive", 96),
               ("exclusive", 64), ("exclusive", 48), ("exclusive", 32)]
    results = []
    for mode, bs in configs:
        print(f"test: {mode} blocksize {bs} ...")
        r = run_once(mode, bs, seconds, gain, quiet=True)
        if r:
            results.append(r)
        time.sleep(0.5)

    print("\n=== RESULTATEN ===")
    hdr = (f"{'mode':10s} {'bs':>4s} {'blok ms':>8s} {'lat tot ms':>10s} "
           f"{'oflow':>5s} {'uflow':>5s} {'laat':>5s} {'p99 ms':>7s} "
           f"{'max ms':>7s} {'cpu%':>5s} {'peak dB':>8s}")
    print(hdr)
    for r in results:
        print(f"{r['mode']:10s} {r['blocksize']:>4d} {r['buffer_ms']:>8.2f} "
              f"{r['total_reported_ms']:>10.1f} {r['in_overflow']:>5d} "
              f"{r['out_underflow']:>5d} {r['late_callbacks']:>5d} "
              f"{r['cb_interval_p99_ms']:>7.2f} {r['cb_interval_max_ms']:>7.2f} "
              f"{r['cpu_load_pct']:>5.1f} {r['peak_dbfs']:>8.1f}")


def main() -> None:
    global HOST
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=["shared", "exclusive"], default="shared")
    ap.add_argument("--host", choices=["wasapi", "wdmks"], default="wasapi")
    ap.add_argument("--blocksize", type=int, default=256)
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--gain", type=float, default=1.0)
    ap.add_argument("--bench", action="store_true")
    args = ap.parse_args()
    HOST = args.host

    print(f"input : [{find_wasapi_device(IN_NAME, 'input')}] Rocksmith ({HOST})")
    print(f"output: [{find_wasapi_device(OUT_NAME_PER_HOST[HOST], 'output')}] "
          f"S/PDIF ({HOST})")

    if args.bench:
        bench(args.seconds, args.gain)
    else:
        run_once(args.mode, args.blocksize, args.seconds, args.gain)


if __name__ == "__main__":
    main()
