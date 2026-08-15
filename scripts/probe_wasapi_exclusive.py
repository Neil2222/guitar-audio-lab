# /// script
# requires-python = ">=3.12"
# dependencies = ["sounddevice"]
# ///
"""Fase 1: test welke formaten WASAPI Exclusive accepteert voor
de Rocksmith-input en de Realtek Digital Output (S/PDIF)."""

import sounddevice as sd

TEST_RATES = [44100, 48000, 96000]
TEST_DTYPES = ["int16", "int24", "int32", "float32"]


def find(name_part: str, kind: str) -> int | None:
    for idx, dev in enumerate(sd.query_devices()):
        if sd.query_hostapis(dev["hostapi"])["name"] != "Windows WASAPI":
            continue
        if name_part.lower() not in dev["name"].lower():
            continue
        if kind == "input" and dev["max_input_channels"] > 0:
            return idx
        if kind == "output" and dev["max_output_channels"] > 0:
            return idx
    return None


def probe(idx: int, kind: str, channels_list: list[int]) -> None:
    dev = sd.query_devices(idx)
    print(f"\n[{idx}] {dev['name']} ({kind})")
    excl = sd.WasapiSettings(exclusive=True)
    for ch in channels_list:
        for rate in TEST_RATES:
            for dtype in TEST_DTYPES:
                try:
                    if kind == "input":
                        sd.check_input_settings(device=idx, samplerate=rate,
                                                channels=ch, dtype=dtype,
                                                extra_settings=excl)
                    else:
                        sd.check_output_settings(device=idx, samplerate=rate,
                                                 channels=ch, dtype=dtype,
                                                 extra_settings=excl)
                    print(f"  OK : {ch}ch {rate}Hz {dtype}")
                except Exception as e:
                    print(f"  -- : {ch}ch {rate}Hz {dtype}  ({type(e).__name__})")


def main() -> None:
    rock = find("rocksmith", "input")
    spdif = find("realtek digital", "output")
    print(f"Rocksmith WASAPI input index: {rock}")
    print(f"Realtek Digital Output WASAPI index: {spdif}")
    if rock is not None:
        probe(rock, "input", [1, 2])
    if spdif is not None:
        probe(spdif, "output", [2])


if __name__ == "__main__":
    main()
