# /// script
# requires-python = ">=3.12"
# dependencies = ["sounddevice", "numpy"]
# ///
"""Check: kunnen de WDM-KS pins (Rocksmith in, SPDIF uit) apart geopend worden?"""

import time

import numpy as np
import sounddevice as sd


def try_open(kind: str, idx: int, blocksize: int = 128) -> None:
    name = sd.query_devices(idx)["name"]
    print(f"\n{kind} [{idx}] {name!r} bs={blocksize}")
    try:
        if kind == "input":
            stream = sd.InputStream(device=idx, samplerate=48000, channels=1,
                                    blocksize=blocksize, dtype="float32",
                                    latency="low",
                                    callback=lambda *a: None)
        else:
            def cb(outdata, frames, t, status):
                outdata.fill(0)
            stream = sd.OutputStream(device=idx, samplerate=48000, channels=2,
                                     blocksize=blocksize, dtype="float32",
                                     latency="low", callback=cb)
        with stream:
            time.sleep(1.0)
            lat = stream.latency
            cpu = stream.cpu_load
        lat_ms = lat * 1000 if isinstance(lat, float) else tuple(x * 1000 for x in lat)
        print(f"  OK  latency={lat_ms} ms  cpu={cpu*100:.1f}%")
    except Exception as e:
        print(f"  FOUT: {e}")


def main() -> None:
    devs = sd.query_devices()
    apis = sd.query_hostapis()
    in_idx = out_idx = None
    for idx, dev in enumerate(devs):
        if apis[dev["hostapi"]]["name"] != "Windows WDM-KS":
            continue
        low = dev["name"].lower()
        if "rocksmith" in low and dev["max_input_channels"] > 0:
            in_idx = idx
            try_open("input", idx)
        if "spdif" in low and dev["max_output_channels"] > 0:
            out_idx = idx
            try_open("output", idx)

    print("\n=== gelijktijdig: eerst output openen, dan input ===")
    def out_cb(outdata, frames, t, status):
        outdata.fill(0)
    try:
        out_s = sd.OutputStream(device=out_idx, samplerate=48000, channels=2,
                                blocksize=128, dtype="float32", latency="low",
                                callback=out_cb)
        print("  output open: OK")
        try:
            in_s = sd.InputStream(device=in_idx, samplerate=48000, channels=1,
                                  blocksize=128, dtype="float32", latency="low",
                                  callback=lambda *a: None)
            print("  input open (naast output): OK")
            with out_s, in_s:
                time.sleep(2.0)
            print("  beide gedraaid: OK")
        except Exception as e:
            print(f"  input open FOUT: {e}")
            out_s.close()
    except Exception as e:
        print(f"  output open FOUT: {e}")


if __name__ == "__main__":
    main()
