# /// script
# requires-python = ">=3.12"
# dependencies = ["sounddevice"]
# ///
"""Fase 1: inventarisatie van audio-devices via PortAudio (sounddevice).

Toont per host-API (MME, DirectSound, WASAPI, WDM-KS) alle devices,
en test voor de interessante devices (Rocksmith input, Realtek Digital
Output) welke sample rates PortAudio accepteert.
"""

import sounddevice as sd

INTERESTING = ("rocksmith", "realtek digital", "spdif", "s/pdif")
TEST_RATES = [22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000]


def main() -> None:
    print("=== Host APIs ===")
    for i, api in enumerate(sd.query_hostapis()):
        print(f"[{i}] {api['name']}  (devices: {len(api['devices'])}, "
              f"default in: {api['default_input_device']}, "
              f"default out: {api['default_output_device']})")

    print("\n=== Devices ===")
    devices = sd.query_devices()
    apis = sd.query_hostapis()
    for idx, dev in enumerate(devices):
        api_name = apis[dev["hostapi"]]["name"]
        print(f"[{idx:2d}] {api_name:20s} in={dev['max_input_channels']} "
              f"out={dev['max_output_channels']} "
              f"default_sr={dev['default_samplerate']:.0f} "
              f"lat_in={dev['default_low_input_latency']*1000:.1f}ms "
              f"lat_out={dev['default_low_output_latency']*1000:.1f}ms  "
              f"{dev['name']}")

    print("\n=== Sample-rate support (interessante devices) ===")
    for idx, dev in enumerate(devices):
        if not any(k in dev["name"].lower() for k in INTERESTING):
            continue
        api_name = apis[dev["hostapi"]]["name"]
        kind = "input" if dev["max_input_channels"] > 0 else "output"
        ok_rates = []
        for rate in TEST_RATES:
            try:
                if kind == "input":
                    sd.check_input_settings(device=idx, samplerate=rate)
                else:
                    sd.check_output_settings(device=idx, samplerate=rate)
                ok_rates.append(rate)
            except Exception:
                pass
        print(f"[{idx:2d}] {api_name:20s} {kind:6s} {dev['name']}")
        print(f"     rates OK: {ok_rates or 'GEEN'}")

    print("\n=== Defaults ===")
    print(f"default device (in, out): {sd.default.device}")


if __name__ == "__main__":
    main()
