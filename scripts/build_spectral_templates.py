"""Generate compact spectral templates for converter-free AMB82 matching."""

from __future__ import annotations

import json
import wave
from pathlib import Path

import numpy as np


ROOT = Path(__file__).resolve().parents[1]
FRAMES = 32
FREQUENCIES = np.array([250, 400, 550, 700, 900, 1100, 1350, 1650, 2000, 2400, 2900, 3500])
SAMPLE_RATE = 16000


def read(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as wav:
        return np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2").astype(np.float64)


def features(audio: np.ndarray) -> np.ndarray:
    output = []
    for frame in range(FRAMES):
        start = frame * len(audio) // FRAMES
        end = (frame + 1) * len(audio) // FRAMES
        samples = audio[start:end]
        samples = samples - samples.mean()
        window = np.hanning(len(samples))
        spectrum = np.fft.rfft(samples * window)
        bins = np.rint(FREQUENCIES * len(samples) / SAMPLE_RATE).astype(int)
        values = np.log1p(np.abs(spectrum[bins]) ** 2)
        values -= values.mean()
        norm = np.sqrt(np.sum(values * values)) + 1e-9
        output.extend(values / norm)
    return np.asarray(output, dtype=np.float32)


def cpp_array(name: str, values: np.ndarray) -> str:
    lines = []
    for start in range(0, len(values), 8):
        literals = []
        for value in values[start:start + 8]:
            number = f"{value:.8g}"
            if "." not in number and "e" not in number:
                number += ".0"
            literals.append(number + "f")
        lines.append("    " + ", ".join(literals))
    return f"static const float {name}[SPECTRAL_FEATURES] = {{\n" + ",\n".join(lines) + "\n};\n"


def main() -> None:
    groups = {}
    for label in ("left", "right"):
        groups[label] = np.stack([features(read(path)) for path in sorted((ROOT / "dataset" / "samples" / label).glob("*.wav"))])
    templates = {label: values.mean(axis=0) for label, values in groups.items()}

    own, other, margins = [], [], []
    confusion = [[0, 0], [0, 0]]
    for actual, label in enumerate(("left", "right")):
        for vector in groups[label]:
            distances = [float(np.mean((vector - templates[name]) ** 2)) for name in ("left", "right")]
            predicted = int(np.argmin(distances))
            confusion[actual][predicted] += 1
            own.append(distances[actual])
            other.append(distances[1 - actual])
            margins.append(distances[1 - actual] - distances[actual])

    # Conservative gates; unknown speech must be close to a template and clearly closer than the other command.
    max_distance = float(np.quantile(own, 0.98) * 1.35)
    min_margin = float(max(0.0005, np.quantile(margins, 0.05) * 0.45))
    header = f"""#pragma once
// Generated spectral command templates; no custom neural-network conversion required.
constexpr int SPECTRAL_FRAMES = {FRAMES};
constexpr int SPECTRAL_BANDS = {len(FREQUENCIES)};
constexpr int SPECTRAL_FEATURES = SPECTRAL_FRAMES * SPECTRAL_BANDS;
constexpr float SPECTRAL_MAX_DISTANCE = {max_distance:.8g}f;
constexpr float SPECTRAL_MIN_MARGIN = {min_margin:.8g}f;
static const int SPECTRAL_FREQUENCIES[SPECTRAL_BANDS] = {{{', '.join(map(str, FREQUENCIES))}}};
"""
    header += cpp_array("LEFT_TEMPLATE", templates["left"])
    header += cpp_array("RIGHT_TEMPLATE", templates["right"])
    (ROOT / "spectral_templates.h").write_text(header, encoding="ascii")
    report = {
        "feature_shape": [FRAMES, len(FREQUENCIES)],
        "training_confusion": confusion,
        "own_distance_range": [float(min(own)), float(max(own))],
        "other_distance_range": [float(min(other)), float(max(other))],
        "margin_range": [float(min(margins)), float(max(margins))],
        "max_distance": max_distance,
        "min_margin": min_margin,
        "limit": "Thresholds are based on supplied recordings; live board microphone testing is required.",
    }
    (ROOT / "model" / "artifacts" / "spectral_template_report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
