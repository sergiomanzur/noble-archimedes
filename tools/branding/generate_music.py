"""Compose and synthesize Noble Archimedes' original launcher music.

Run from any directory with Python 3.10+: python tools/branding/generate_music.py
Only the Python standard library is used. No recordings, soundfonts, game
assets, pretrained music models, or external services are used.
"""
from __future__ import annotations

import argparse
from array import array
import json
import math
from pathlib import Path
import sys
import wave

ROOT = Path(__file__).resolve().parents[2]
TAU = 2 * math.pi


def build_score() -> dict:
    """An explicitly authored eight-bar score; all times are seconds."""
    chords = [
        [38, 53, 57, 64], [34, 53, 57, 62],
        [43, 53, 58, 65], [45, 52, 59, 64],
        [38, 53, 57, 62], [41, 52, 57, 64],
        [40, 55, 58, 62], [45, 52, 58, 64],
    ]
    melody = [
        (1.0, 74), (3.0, 69), (5.5, 77), (7.0, 76),
        (9.0, 74), (11.5, 70), (13.0, 71), (15.0, 76),
        (17.0, 77), (19.5, 81), (21.0, 76), (23.0, 72),
        (25.5, 74), (27.0, 70), (29.0, 76), (31.0, 73),
    ]
    events = []
    for bar, chord in enumerate(chords):
        for voice, note in enumerate(chord):
            events.append(dict(start=bar * 4.0, duration=6.2, midi=note,
                               instrument="organ", gain=0.40 if voice == 0 else 0.28,
                               pan=[-0.25, -0.65, 0.60, 0.15][voice]))
    for index, (start, note) in enumerate(melody):
        events.append(dict(start=start, duration=3.6, midi=note,
                           instrument="celesta", gain=0.25,
                           pan=-0.35 if index % 2 == 0 else 0.40))
    return {
        "title": "The Quiet Orrery",
        "version": 1,
        "duration_seconds": 32,
        "sample_rate": 44100,
        "tempo_bpm": 60,
        "meter": "4/4",
        "tonal_center": "D minor; suspended and extended harmonies",
        "peak_amplitude": 0.20,
        "provenance": {
            "origin": "Original composition and mathematical synthesis created for Noble Archimedes.",
            "sources": "No external audio, melodies, samples, soundfonts, or game assets.",
            "reproduction": "python tools/branding/generate_music.py",
            "license": "See project LICENSE for original project materials.",
        },
        "synthesis": {
            "organ": "Sine partials 1, 2, 3; slow sine-squared attack and release.",
            "celesta": "Exponentially decaying sine partials 1, 2, 4; soft attack and release.",
            "space": "Three circular stereo delay taps; tails wrap into the next repetition.",
            "loop": "Each voice envelope reaches zero at both ends; note tails and echoes wrap modulo 32 seconds.",
            "normalization": "Fixed whole-loop peak at 20% full scale, approximately -14 dBFS peak.",
        },
        "events": events,
    }


def render(score: dict, sample_rate: int | None = None) -> array:
    """Return interleaved little/native-endian signed 16-bit stereo samples.

    Notes are synthesized with local phase and wrapped into a circular buffer.
    Consequently the beginning includes the preceding repetition's natural
    releases, avoiding a silence gap or an artificial whole-track fade.
    """
    rate = sample_rate or score["sample_rate"]
    if rate < 1000:
        raise ValueError("sample rate must be at least 1000 Hz")
    count = round(score["duration_seconds"] * rate)
    left = array("d", [0.0]) * count
    right = array("d", [0.0]) * count
    for event in score["events"]:
        start = round(event["start"] * rate)
        length = round(event["duration"] * rate)
        freq = 440.0 * 2 ** ((event["midi"] - 69) / 12)
        pan_angle = (event["pan"] + 1) * math.pi / 4
        lg = math.cos(pan_angle) * event["gain"]
        rg = math.sin(pan_angle) * event["gain"]
        organ = event["instrument"] == "organ"
        # Omit partials above Nyquist, also supporting inexpensive test renders.
        harmonics = [(1, 1.0), (2, 0.18), (3, 0.045)] if organ else [(1, 1.0), (2, 0.30), (4, 0.09)]
        harmonics = [(h, gain) for h, gain in harmonics if h * freq < rate * 0.48]
        for i in range(length):
            t = i / rate
            remaining = (length - 1 - i) / rate
            if organ:
                envelope = math.sin(min(t / 1.4, 1) * math.pi / 2) ** 2
                envelope *= math.sin(min(remaining / 2.1, 1) * math.pi / 2) ** 2
            else:
                envelope = (1 - math.exp(-t * 65)) * math.exp(-t * 1.45)
                envelope *= math.sin(min(remaining / 0.4, 1) * math.pi / 2) ** 2
            phase = TAU * freq * t
            tone = sum(gain * math.sin(phase * h) for h, gain in harmonics) * envelope
            at = (start + i) % count
            left[at] += tone * lg
            right[at] += tone * rg
    taps = [(round(rate * 0.31), 0.16), (round(rate * 0.73), 0.10), (round(rate * 1.37), 0.06)]
    stereo = array("d")
    for i in range(count):
        l, r = left[i], right[i]
        for delay, gain in taps:
            l += right[(i - delay) % count] * gain
            r += left[(i - delay) % count] * gain
        stereo.extend((l, r))
    scale = score["peak_amplitude"] * 32767 / max(abs(x) for x in stereo)
    return array("h", (round(x * scale) for x in stereo))


def write_music(output: Path, sample_rate: int = 44100) -> None:
    score = build_score()
    score["sample_rate"] = sample_rate
    pcm = render(score)
    if sys.byteorder != "little":
        pcm.byteswap()
    output.mkdir(parents=True, exist_ok=True)
    with wave.open(str(output / "launcher-theme.wav"), "wb") as wav:
        wav.setparams((2, 2, sample_rate, 0, "NONE", "not compressed"))
        wav.writeframes(pcm.tobytes())
    (output / "music-score.json").write_text(
        json.dumps(score, indent=2) + "\n", encoding="utf-8"
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "assets/noble")
    parser.add_argument("--sample-rate", type=int, default=44100)
    args = parser.parse_args()
    write_music(args.output, args.sample_rate)
    print(f"Wrote original 32-second stereo theme to {args.output}")
