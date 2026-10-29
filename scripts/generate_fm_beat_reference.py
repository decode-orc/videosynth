#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Simon Inns
"""Writes the FM-beat golden vectors used by tests/unit/test_fm_beat_model.cpp.

Runs the reference beat model, `_beat_basis` in domesday-aiv-restore
newdecode/deherring.py, on one synthetic PAL signal and writes the real part
of its basis as a C++ header. The unit test rebuilds the same signal from the
same formulas, so it reads no files at runtime.

The reference is not vendored. Without --reference the script uses
transcribed_beat_basis() below, a line-for-line transcription of the parts of
_beat_basis that form the basis (model-2026-10-27b). To check the
transcription against the original, download it and pass its path; both must
write the same header:

    curl -LO https://raw.githubusercontent.com/dp111/domesday-aiv-restore/main/newdecode/deherring.py
    python3 scripts/generate_fm_beat_reference.py --reference deherring.py \\
        --output tests/unit/fm_beat_reference_vectors.h

Needs numpy and scipy (the reference's own dependencies).
"""

import argparse
import importlib.util
import math
import sys

import numpy as np
import scipy.fft as sfft
import scipy.signal as sps
from scipy.ndimage import uniform_filter1d

# The synthetic signal. Keep in step with MakeReferenceSignal() in
# tests/unit/test_fm_beat_model.cpp.
COUNT = 1400
WINDOW_START = 150
WINDOW_END = 1250
RAMP = 40
LUMA_RAMP = 100
LUMA_MV = 350.0
LUMA_SWING_MV = 150.0
CHROMA_MV = 200.0
CHROMA_PHASE_RAD = 0.7
BLACK_MV = 0.0
WHITE_MV = 700.0
# Compared range: clear of the chroma ramps and the band-pass tails.
COMPARE_START = 300
COMPARE_END = 1100


REFERENCE_STAMP = "model:model-2026-10-27b"


def transcribed_beat_basis(x, black, white):
    """deherring.py _beat_basis (model-2026-10-27b), basis output only."""
    fs = 17.734475e6
    f0_hz = 7.088e6
    hz_ire = 8000.0
    gamma = 0.4
    alpha = -1.0
    band = (2.2e6, 3.9e6)
    fsc = 4.43361875e6
    size = 368640

    def emph(t1, t2):
        w1 = 2 * fs * np.tan((1 / t1) / (2 * fs))
        w2 = 2 * fs * np.tan((1 / t2) / (2 * fs))
        b, a = sps.zpk2tf([-w1], [-w2], w2 / w1)
        return sps.bilinear(b, a, fs)

    def box(v, n):
        return uniform_filter1d(v, n, mode="nearest")

    deemp = emph(100e-9, 400e-9)
    preemp = emph(400e-9, 100e-9)
    fr = sfft.fftfreq(size, 1 / fs)
    rf = sfft.rfftfreq(size, 1 / fs)
    cb = (2 * np.clip((rf - 3.3e6) / 0.4e6, 0, 1) * np.clip((5.6e6 - rf) / 0.4e6, 0, 1)).astype(np.float32)
    bp = (np.clip((np.abs(fr) - band[0]) / 0.3e6 + 0.5, 0, 1)
          * np.clip((band[1] - np.abs(fr)) / 0.3e6 + 0.5, 0, 1)).astype(np.float32)
    hp = sps.freqz(*preemp, worN=[fsc], fs=fs)[1][0]

    ire = (x - black) * (100.0 / (white - black))
    ip = sps.lfilter(*preemp, ire - ire[0]) + ire[0]
    spectrum = sfft.rfft(x.astype(np.float32), size)
    sp = np.zeros(size, np.complex64)
    sp[:spectrum.size] = spectrum * cb
    ca = sfft.ifft(sp)[:x.size]
    cp = (ca * hp).real * (100.0 / (white - black))
    fc = f0_hz + hz_ire * (ip - (1 - gamma) * cp)
    ph = np.cumsum(fc * (2 * np.pi / fs)) % (2 * np.pi)
    lf = box(box(fc, 4), 17)
    ph = ph.astype(np.float32)
    am = ((lf - fsc) / 3e9 * np.exp(alpha * (lf - 7.5e6) / 1e6)).astype(np.float32)
    r = (np.cos(ph) * am + 1j * (np.sin(ph) * am)) * np.conj(ca)
    r = (sps.lfilter(*deemp, r.real) + 1j * sps.lfilter(*deemp, r.imag)).astype(np.complex64)
    return sfft.ifft(sfft.fft(r, size) * bp)[:x.size]


def envelope(start, end, ramp):
    """Raised-cosine envelope: zero outside [start, end), ramp samples long."""
    values = np.zeros(COUNT)
    for i in range(start, end):
        rise = min(i - start, end - 1 - i)
        values[i] = 1.0 if rise >= ramp else 0.5 - 0.5 * math.cos(math.pi * rise / ramp)
    return values


def make_signal():
    # Luma rises from black at both ends: the reference zero-pads its FFTs,
    # and a level held to the ends would step there and ring into its chroma
    # band.
    n = np.arange(COUNT, dtype=np.float64)
    luma = envelope(0, COUNT, LUMA_RAMP) * (
        LUMA_MV + LUMA_SWING_MV * np.sin(2.0 * math.pi * n / COUNT))
    chroma = (CHROMA_MV * envelope(WINDOW_START, WINDOW_END, RAMP)
              * np.cos(0.5 * math.pi * n + CHROMA_PHASE_RAD))
    return luma, chroma


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--reference", help="path to deherring.py (default: the transcription)")
    parser.add_argument("--output", required=True, help="header to write")
    args = parser.parse_args()

    luma, chroma = make_signal()
    if args.reference:
        spec = importlib.util.spec_from_file_location("deherring", args.reference)
        reference = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(reference)
        stamp = reference.STAMP
        basis = reference._beat_basis(luma + chroma, BLACK_MV, WHITE_MV)[0].real
    else:
        stamp = REFERENCE_STAMP
        basis = transcribed_beat_basis(luma + chroma, BLACK_MV, WHITE_MV).real
    values = basis[COMPARE_START:COMPARE_END]

    lines = [
        "/*",
        " * File:        fm_beat_reference_vectors.h",
        " * Module:      channel_tests",
        " * Purpose:     Real part of the reference FM-beat basis (deherring.py",
        " *              _beat_basis, " + stamp + ") for the",
        " *              synthetic signal of test_fm_beat_model.cpp. Generated by",
        " *              scripts/generate_fm_beat_reference.py; do not edit.",
        " *",
        " * SPDX-License-Identifier: GPL-3.0-or-later",
        " * SPDX-FileCopyrightText: 2026 Simon Inns",
        " */",
        "",
        "#pragma once",
        "",
        "#include <cstddef>",
        "",
        "namespace videosynth::fm_beat_reference {",
        "",
        f"constexpr std::size_t kCompareStart = {COMPARE_START};",
        f"constexpr std::size_t kCompareEnd = {COMPARE_END};",
        "",
        "// Basis in mV for samples [kCompareStart, kCompareEnd).",
        "// clang-format off",
        "constexpr double kBasisMv[] = {",
    ]
    for i in range(0, len(values), 4):
        lines.append("    " + " ".join(f"{v:.9e}," for v in values[i:i + 4]))
    lines += ["};", "// clang-format on", "", "}  // namespace videosynth::fm_beat_reference", ""]
    with open(args.output, "w", encoding="utf-8") as stream:
        stream.write("\n".join(lines))
    print(f"wrote {len(values)} values, peak {np.max(np.abs(values)):.6g} mV", file=sys.stderr)


if __name__ == "__main__":
    main()
