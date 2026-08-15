# /// script
# requires-python = ">=3.12"
# dependencies = ["sounddevice", "numpy"]
# ///
"""Fase 2: passthrough via twee losse streams + ringbuffer.

Nodig omdat (a) PortAudio WDM-KS geen duplex over twee devices kan en
(b) input (Rocksmith) en output (Realtek USB) op verschillende USB-klokken
lopen: de ringbuffer vangt clock drift op.

Gebruik:
  uv run scripts/split_passthrough.py --host wdmks --blocksize 128 --seconds 10
  uv run scripts/split_passthrough.py --host wasapi --mode exclusive --blocksize 128
"""

import argparse
import time
from collections import deque

import numpy as np
import sounddevice as sd

SAMPLERATE = 48000
HOST_APIS = {"wasapi": "Windows WASAPI", "wdmks": "Windows WDM-KS"}
IN_NAME = "rocksmith"
OUT_NAME = {"wasapi": "realtek digital", "wdmks": "spdif"}
PREFILL = 2    # blokken in de ring voordat de output begint te spelen
MAX_QUEUE = 4  # blokken; daarboven droppen we (drift-bescherming); zie --maxq


def find_device(host: str, name_part: str, kind: str) -> int:
    for idx, dev in enumerate(sd.query_devices()):
        if sd.query_hostapis(dev["hostapi"])["name"] != HOST_APIS[host]:
            continue
        if name_part not in dev["name"].lower():
            continue
        if kind == "input" and dev["max_input_channels"] > 0:
            return idx
        if kind == "output" and dev["max_output_channels"] > 0:
            return idx
    raise SystemExit(f"device niet gevonden: {host}/{name_part}/{kind}")


def run(host: str, mode: str, blocksize: int, seconds: float, gain: float,
        latency) -> None:
    in_dev = find_device(host, IN_NAME, "input")
    out_dev = find_device(host, OUT_NAME[host], "output")
    print(f"input : [{in_dev}] {sd.query_devices(in_dev)['name']}")
    print(f"output: [{out_dev}] {sd.query_devices(out_dev)['name']}")

    excl = sd.WasapiSettings(exclusive=True) if (host == "wasapi" and
                                                 mode == "exclusive") else None

    q: deque[np.ndarray] = deque()
    warmup_cbs = int(1.0 * SAMPLERATE / blocksize)  # eerste seconde niet meetellen
    st = {"in_cb": 0, "out_cb": 0, "underrun": 0, "drop": 0, "peak": 0.0,
          "fills": [], "in_flags": 0, "out_flags": 0, "started": False}

    def in_cb(indata, frames, t, status):
        if status:
            st["in_flags"] += 1
        block = indata * gain
        p = float(np.max(np.abs(block)))
        if p > st["peak"]:
            st["peak"] = p
        q.append(block.copy())
        if len(q) > MAX_QUEUE:
            q.popleft()
            if st["out_cb"] > warmup_cbs:
                st["drop"] += 1
        st["in_cb"] += 1

    def out_cb(outdata, frames, t, status):
        if status:
            st["out_flags"] += 1
        if not st["started"]:
            if len(q) < PREFILL:
                outdata.fill(0)
                st["out_cb"] += 1
                return
            st["started"] = True
        if st["out_cb"] > warmup_cbs:
            st["fills"].append(len(q))
        if q:
            outdata[:] = q.popleft()  # mono (n,1) -> stereo (n,2) broadcast
        else:
            outdata.fill(0)
            st["started"] = False  # opnieuw prefillen na leegloop
            if st["out_cb"] > warmup_cbs:
                st["underrun"] += 1
        st["out_cb"] += 1

    out_stream = sd.OutputStream(device=out_dev, samplerate=SAMPLERATE, channels=2,
                                 blocksize=blocksize, dtype="float32",
                                 latency=latency, callback=out_cb,
                                 extra_settings=excl)
    in_stream = sd.InputStream(device=in_dev, samplerate=SAMPLERATE, channels=1,
                               blocksize=blocksize, dtype="float32",
                               latency=latency, callback=in_cb,
                               extra_settings=excl)
    with in_stream, out_stream:
        time.sleep(seconds)
        lat_in = in_stream.latency * 1000
        lat_out = out_stream.latency * 1000
        cpu = (in_stream.cpu_load + out_stream.cpu_load) * 100

    fills = np.array(st["fills"]) if st["fills"] else np.array([0])
    block_ms = blocksize / SAMPLERATE * 1000
    ring_ms = float(np.mean(fills)) * block_ms
    print(f"\n--- {host} {mode} | blocksize {blocksize} ({block_ms:.2f} ms) | "
          f"latency-hint {latency} ---")
    print(f"  stream-latency  in={lat_in:.1f} ms  out={lat_out:.1f} ms  "
          f"ring~{ring_ms:.1f} ms (fill avg={np.mean(fills):.1f} "
          f"min={fills.min()} max={fills.max()} blokken)")
    print(f"  geschat totaal: {lat_in + lat_out + ring_ms:.1f} ms "
          f"(in + ring + out)")
    print(f"  callbacks in/out={st['in_cb']}/{st['out_cb']}  "
          f"underruns={st['underrun']}  drops={st['drop']}  "
          f"flags in/out={st['in_flags']}/{st['out_flags']}  cpu={cpu:.1f}%")
    peak_db = 20 * np.log10(st["peak"]) if st["peak"] > 0 else float("-inf")
    print(f"  signaal peak={peak_db:.1f} dBFS")


def main() -> None:
    global MAX_QUEUE
    ap = argparse.ArgumentParser()
    # defaults = beste stabiele configuratie gevonden op 2026-08-15:
    # wasapi exclusive, bs=128, hint 3 ms -> ~17.5 ms totaal, 0 underruns
    ap.add_argument("--host", choices=["wasapi", "wdmks"], default="wasapi")
    ap.add_argument("--mode", choices=["shared", "exclusive"], default="exclusive")
    ap.add_argument("--blocksize", type=int, default=128)
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--gain", type=float, default=1.0)
    ap.add_argument("--latency", default="0.003",
                    help="'low', 'high' of een getal in seconden (bv. 0.003)")
    ap.add_argument("--maxq", type=int, default=6,
                    help="ringbuffer-cap in blokken")
    args = ap.parse_args()
    MAX_QUEUE = args.maxq
    try:
        latency = float(args.latency)
    except ValueError:
        latency = args.latency
    run(args.host, args.mode, args.blocksize, args.seconds, args.gain, latency)


if __name__ == "__main__":
    main()
