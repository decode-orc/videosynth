#!/usr/bin/env python3
"""
Validate MKV files in assets/mkv against docs/mkv-bt601-compliance-requirements.md.

Sampling policy:
- If video has at least 3 frames: validate beginning, middle, and end frames.
- If video has fewer than 3 frames: validate all available frames.
"""

from __future__ import annotations

import argparse
import glob
import json
import subprocess
import sys
from array import array
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple


def run_cmd(args: Sequence[str]) -> Tuple[int, bytes, bytes]:
    try:
        p = subprocess.run(list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    except FileNotFoundError:
        return 127, b"", f"command not found: {args[0]}".encode("utf-8", errors="replace")
    return p.returncode, p.stdout, p.stderr


def require_tools(tools: Sequence[str]) -> List[str]:
    missing: List[str] = []
    for t in tools:
        rc, _, _ = run_cmd(["sh", "-lc", f"command -v {t}"])
        if rc != 0:
            missing.append(t)
    return missing


def parse_rational(text: str) -> Optional[Tuple[int, int]]:
    sep = "/" if "/" in text else (":" if ":" in text else None)
    if sep is None:
        return None
    a, b = text.split(sep, 1)
    try:
        n = int(a.strip())
        d = int(b.strip())
    except ValueError:
        return None
    if d == 0:
        return None
    return n, d


def ratio_to_float(r: Tuple[int, int]) -> float:
    return r[0] / r[1]


def nominal_tc_fps(expected_fps: Optional[Tuple[int, int]]) -> Optional[int]:
    if expected_fps is None:
        return None
    n, d = expected_fps
    if (n, d) == (30000, 1001):
        return 30
    if (n, d) == (24000, 1001):
        return 24
    if (n, d) == (25, 1):
        return 25
    val = int(round(n / d))
    return val if val > 0 else None


def parse_timecode_ff(tc: str) -> Optional[int]:
    # Accept HH:MM:SS:FF and HH:MM:SS;FF forms.
    if not tc:
        return None
    if ";" in tc:
        parts = tc.split(";")
        if len(parts) != 2:
            return None
        hhmmss, ff = parts
        base = hhmmss.split(":")
        if len(base) != 3:
            return None
        try:
            int(base[0])
            int(base[1])
            int(base[2])
            return int(ff)
        except ValueError:
            return None

    parts = tc.split(":")
    if len(parts) != 4:
        return None
    try:
        int(parts[0])
        int(parts[1])
        int(parts[2])
        return int(parts[3])
    except ValueError:
        return None


def find_timecode_streams(streams: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    out: List[Dict[str, Any]] = []
    for s in streams:
        tags = s.get("tags") if isinstance(s.get("tags"), dict) else {}
        codec_type = str(s.get("codec_type", ""))
        codec_name = str(s.get("codec_name", "")).lower()
        codec_tag = str(s.get("codec_tag_string", "")).lower()
        handler = str(tags.get("handler_name", "")).lower()
        has_timecode_tag = "timecode" in tags
        looks_timecode = (
            has_timecode_tag
            or "timecode" in handler
            or codec_tag == "tmcd"
            or (codec_type == "data" and "timecode" in codec_name)
        )
        if looks_timecode:
            out.append(s)
    return out


def ffprobe_json(path: Path) -> Dict[str, Any]:
    rc, out, err = run_cmd([
        "ffprobe",
        "-v",
        "error",
        "-show_streams",
        "-show_format",
        "-print_format",
        "json",
        str(path),
    ])
    if rc != 0:
        msg = (err or out).decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"ffprobe failed: {msg}")
    try:
        return json.loads(out.decode("utf-8", errors="replace"))
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"failed to parse ffprobe json: {exc}") from exc


def ffprobe_side_data(path: Path) -> Dict[str, int]:
    rc, out, err = run_cmd([
        "ffprobe",
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-show_entries",
        "stream_side_data=side_data_type,crop_left,crop_right,crop_top,crop_bottom",
        "-of",
        "default=nw=1:nk=0",
        str(path),
    ])
    if rc != 0:
        return {}
    text = out.decode("utf-8", errors="replace")
    data: Dict[str, int] = {}
    for line in text.splitlines():
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        k = k.strip()
        v = v.strip()
        if k in ("crop_left", "crop_right", "crop_top", "crop_bottom"):
            try:
                data[k] = int(v)
            except ValueError:
                pass
    return data


def decode_frame_yuv422p10(path: Path, frame_index: int, width: int, height: int) -> array:
    vf = f"select=eq(n\\,{frame_index})"
    rc, out, err = run_cmd([
        "ffmpeg",
        "-v",
        "error",
        "-apply_cropping",
        "none",
        "-i",
        str(path),
        "-vf",
        vf,
        "-vsync",
        "0",
        "-frames:v",
        "1",
        "-f",
        "rawvideo",
        "-pix_fmt",
        "yuv422p10le",
        "-",
    ])
    if rc != 0:
        msg = (err or out).decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"ffmpeg decode failed for frame {frame_index}: {msg}")

    raw = out
    expected_bytes = width * height * 4
    if len(raw) != expected_bytes:
        raise RuntimeError(
            f"decoded frame {frame_index} bytes mismatch: got {len(raw)}, expected {expected_bytes}"
        )

    vals = array("H")
    vals.frombytes(raw)
    if sys.byteorder != "little":
        vals.byteswap()
    return vals


def discover_mkvs(repo_root: Path, patterns: Sequence[str]) -> List[Path]:
    found: List[Path] = []
    seen = set()
    for pat in patterns:
        full = str(repo_root / pat)
        for p in glob.glob(full, recursive=True):
            pp = Path(p)
            if not pp.is_file() or pp.suffix.lower() != ".mkv":
                continue
            rp = pp.resolve()
            if rp in seen:
                continue
            seen.add(rp)
            found.append(pp)
    found.sort()
    return found


def choose_sample_indices(frame_count: int) -> List[int]:
    if frame_count <= 0:
        return []
    if frame_count >= 3:
        idx = [0, frame_count // 2, frame_count - 1]
        uniq: List[int] = []
        for v in idx:
            if v not in uniq:
                uniq.append(v)
        return uniq
    return list(range(frame_count))


def get_video_frame_count(path: Path, video_stream: Dict[str, Any]) -> int:
    nb_frames = video_stream.get("nb_frames")
    if isinstance(nb_frames, str) and nb_frames.isdigit():
        return int(nb_frames)

    # Fallback to counted frames.
    rc, out, err = run_cmd([
        "ffprobe",
        "-v",
        "error",
        "-count_frames",
        "-select_streams",
        "v:0",
        "-show_entries",
        "stream=nb_read_frames",
        "-of",
        "default=nw=1:nk=1",
        str(path),
    ])
    if rc == 0:
        txt = out.decode("utf-8", errors="replace").strip()
        if txt.isdigit():
            return int(txt)

    # Last fallback: duration * avg_frame_rate.
    afr = parse_rational(str(video_stream.get("avg_frame_rate", "")))
    dur = None
    try:
        dur = float(video_stream.get("duration"))
    except (TypeError, ValueError):
        dur = None
    if dur is not None and afr is not None:
        n, d = afr
        est = int(round(dur * (n / d)))
        if est > 0:
            return est

    msg = (err or out).decode("utf-8", errors="replace").strip()
    raise RuntimeError(f"unable to determine frame count: {msg}")


@dataclass
class CheckResult:
    name: str
    ok: bool
    message: str
    severity: str = "FAIL"


@dataclass
class FileReport:
    path: Path
    checks: List[CheckResult]

    def failed(self, strict_warnings: bool) -> bool:
        for c in self.checks:
            if c.ok:
                continue
            if c.severity == "FAIL":
                return True
            if strict_warnings and c.severity == "WARN":
                return True
        return False


def add(checks: List[CheckResult], name: str, ok: bool, msg: str, severity: str = "FAIL") -> None:
    checks.append(CheckResult(name=name, ok=ok, message=msg, severity=severity))


def validate_padding_and_codes(
    yuv: array,
    width: int,
    height: int,
    left_pad: int,
    right_pad: int,
    pad_y_max: int,
    pad_c_delta: int,
    legal_tol: int,
    studio_min: int,
    studio_max: int,
) -> Tuple[bool, str, bool, bool, str]:
    y_plane = width * height
    c_plane = (width // 2) * height
    u_off = y_plane
    v_off = y_plane + c_plane

    # Legal range checks on entire sampled frame.
    y_min = 10_000
    y_max = -1
    u_min = 10_000
    u_max = -1
    v_min = 10_000
    v_max = -1

    # Padding-black checks only at side borders.
    border_pixels = 0
    bad_border = 0
    examples: List[str] = []

    for yy in range(height):
        y_row = yy * width
        c_row = yy * (width // 2)

        for xx in range(width):
            yv = int(yuv[y_row + xx])
            uv = int(yuv[u_off + c_row + (xx // 2)])
            vv = int(yuv[v_off + c_row + (xx // 2)])

            if yv < y_min:
                y_min = yv
            if yv > y_max:
                y_max = yv
            if uv < u_min:
                u_min = uv
            if uv > u_max:
                u_max = uv
            if vv < v_min:
                v_min = vv
            if vv > v_max:
                v_max = vv

            if xx < left_pad or xx >= (width - right_pad):
                border_pixels += 1
                ok_pad = (yv <= pad_y_max) and (abs(uv - 512) <= pad_c_delta) and (abs(vv - 512) <= pad_c_delta)
                if not ok_pad:
                    bad_border += 1
                    if len(examples) < 5:
                        examples.append(f"(x={xx},y={yy},YUV10=({yv},{uv},{vv}))")

    nominal_y_ok = y_min >= (64 - legal_tol) and y_max <= (940 + legal_tol)
    nominal_c_ok = (
        u_min >= (64 - legal_tol)
        and u_max <= (960 + legal_tol)
        and v_min >= (64 - legal_tol)
        and v_max <= (960 + legal_tol)
    )
    nominal_ok = nominal_y_ok and nominal_c_ok

    studio_ok = (
        y_min >= studio_min
        and y_max <= studio_max
        and u_min >= studio_min
        and u_max <= studio_max
        and v_min >= studio_min
        and v_max <= studio_max
    )

    nominal_state = "inside nominal" if nominal_ok else "outside nominal"
    studio_state = "inside studio excursion" if studio_ok else "outside studio excursion"
    codes_msg = (
        f"Y[{y_min},{y_max}] Cb[{u_min},{u_max}] Cr[{v_min},{v_max}] "
        f"(nominal tolerance +- {legal_tol}: {nominal_state}; "
        f"studio range [{studio_min},{studio_max}]: {studio_state})"
    )

    pad_ok = bad_border == 0
    if pad_ok:
        pad_msg = (
            f"all {border_pixels} border pixels match black-padding expectation "
            f"(left={left_pad}, right={right_pad}, Y<={pad_y_max}, C delta<={pad_c_delta})"
        )
    else:
        pad_msg = (
            f"{bad_border}/{border_pixels} border pixels violate black-padding expectation "
            f"(left={left_pad}, right={right_pad}); examples: {'; '.join(examples)}"
        )

    return pad_ok, pad_msg, studio_ok, nominal_ok, codes_msg


def validate_one(
    media_path: Path,
    left_pad: int,
    right_pad: int,
    pad_y_max: int,
    pad_c_delta: int,
    strict_crop: bool,
    legal_tol: int,
    studio_min: int,
    studio_max: int,
) -> FileReport:
    checks: List[CheckResult] = []

    try:
        meta = ffprobe_json(media_path)
    except Exception as exc:
        add(checks, "metadata", False, f"failed to read metadata: {exc}")
        return FileReport(path=media_path, checks=checks)

    streams = meta.get("streams", []) if isinstance(meta.get("streams"), list) else []
    fmt = meta.get("format", {}) if isinstance(meta.get("format"), dict) else {}

    video_streams = [s for s in streams if s.get("codec_type") == "video"]
    audio_streams = [s for s in streams if s.get("codec_type") == "audio"]

    fmt_name = str(fmt.get("format_name", ""))
    add(checks, "container_format", "matroska" in fmt_name, f"format_name={fmt_name}")
    add(checks, "video_stream_count", len(video_streams) == 1, f"video_stream_count={len(video_streams)}")

    if len(video_streams) != 1:
        return FileReport(path=media_path, checks=checks)

    v = video_streams[0]
    width = int(v.get("width", -1))
    height = int(v.get("height", -1))

    if height == 486:
        expected_sar = (108, 119)
        expected_fps = (30000, 1001)
        expected_field_order = "bt"
        # A 24000/1001 film clip for 2:3 pulldown is a progressive 525-line
        # source: the film rate and a progressive field order replace the
        # interlaced video ones.
        if parse_rational(str(v.get("r_frame_rate", ""))) == (24000, 1001):
            expected_fps = (24000, 1001)
            expected_field_order = "progressive"
        expected_primaries = {"smpte170m"}
        expected_transfer = {"bt709", "smpte170m"}
    elif height == 576:
        expected_sar = (128, 117)
        expected_fps = (25, 1)
        expected_field_order = "tb"
        expected_primaries = {"bt470bg"}
        expected_transfer = {"bt709", "bt470bg"}
    else:
        expected_sar = None
        expected_fps = None
        expected_field_order = ""
        expected_primaries = set()
        expected_transfer = set()

    add(checks, "video_codec", v.get("codec_name") == "ffv1", f"codec_name={v.get('codec_name')}")
    add(checks, "pixel_format", v.get("pix_fmt") == "yuv422p10le", f"pix_fmt={v.get('pix_fmt')}")
    bprs = str(v.get("bits_per_raw_sample", ""))
    bprs_ok = bprs in ("10", "", "N/A", "unknown")
    add(
        checks,
        "bit_depth",
        bprs_ok,
        f"bits_per_raw_sample={v.get('bits_per_raw_sample')} (pix_fmt must remain yuv422p10le)",
        severity="WARN" if not bprs_ok else "FAIL",
    )
    add(checks, "dimensions", width == 720 and height in (486, 576), f"size={width}x{height}")
    sar = parse_rational(str(v.get("sample_aspect_ratio", "")))
    sar_ok = (
        expected_sar is not None
        and sar is not None
        and abs(ratio_to_float(sar) - ratio_to_float(expected_sar)) <= 2e-4
    )
    expected_sar_text = f"{expected_sar[0]}:{expected_sar[1]}" if expected_sar is not None else "n/a"
    add(checks, "sample_aspect_ratio", sar_ok, f"sar={v.get('sample_aspect_ratio')} parsed={sar} expected={expected_sar_text}")

    avg_fps = parse_rational(str(v.get("avg_frame_rate", "")))
    r_fps = parse_rational(str(v.get("r_frame_rate", "")))
    fps_ok = expected_fps is not None and avg_fps == expected_fps and r_fps == expected_fps
    add(
        checks,
        "frame_rate",
        fps_ok,
        f"avg_frame_rate={v.get('avg_frame_rate')} r_frame_rate={v.get('r_frame_rate')} expected={expected_fps}",
    )

    field_order = str(v.get("field_order", ""))
    if field_order == expected_field_order:
        add(checks, "field_order", True, f"field_order={field_order} expected={expected_field_order}")
    elif field_order in ("", "unknown", "progressive"):
        add(
            checks,
            "field_order",
            False,
            f"field_order={field_order or 'missing'} expected={expected_field_order} (metadata missing/ambiguous)",
            severity="WARN",
        )
    else:
        add(checks, "field_order", False, f"field_order={field_order} expected={expected_field_order}")

    add(checks, "color_space", v.get("color_space") == "smpte170m", f"color_space={v.get('color_space')}")
    add(checks, "color_primaries", v.get("color_primaries") in expected_primaries, f"color_primaries={v.get('color_primaries')} expected_one_of={sorted(expected_primaries)}")

    transfer = str(v.get("color_transfer", ""))
    transfer_ok = transfer in expected_transfer
    add(checks, "color_transfer", transfer_ok, f"color_transfer={transfer}", severity="WARN" if not transfer_ok else "FAIL")

    color_range = str(v.get("color_range", "unknown"))
    if color_range in ("tv", "mpeg"):
        add(checks, "color_range", True, f"color_range={color_range}")
    else:
        add(checks, "color_range", False, f"color_range={color_range} (limited range preferred)", severity="WARN")

    # Audio policy: optional, but if present require 48k PCM.
    if not audio_streams:
        add(checks, "audio_stream", True, "no audio stream present")
    else:
        a0 = audio_streams[0]
        a_ok = a0.get("codec_name") == "pcm_s24le" and str(a0.get("sample_rate")) == "48000"
        add(checks, "audio_stream", a_ok, f"codec={a0.get('codec_name')} sample_rate={a0.get('sample_rate')}")

    # Timecode policy: optional; if present, frame number field should be consistent
    # with nominal system frame rate.
    tc_streams = find_timecode_streams(streams)
    if not tc_streams:
        add(checks, "timecode_stream", True, "no timecode stream detected")
    else:
        tc_limit = nominal_tc_fps(expected_fps)
        tc_ok = True
        tc_notes: List[str] = []
        for s in tc_streams:
            tags = s.get("tags") if isinstance(s.get("tags"), dict) else {}
            tc = str(tags.get("timecode", ""))
            ff = parse_timecode_ff(tc)
            idx = s.get("index", "?")
            if ff is None:
                tc_ok = False
                tc_notes.append(f"stream {idx}: invalid/missing timecode '{tc}'")
                continue
            if tc_limit is None or ff < 0 or ff >= tc_limit:
                tc_ok = False
                tc_notes.append(f"stream {idx}: timecode '{tc}' not consistent with nominal fps {tc_limit}")
                continue
            tc_notes.append(f"stream {idx}: timecode '{tc}' consistent with nominal fps {tc_limit}")

        add(checks, "timecode_stream", tc_ok, " | ".join(tc_notes))

    # Optional crop side data check.
    sd = ffprobe_side_data(media_path)
    if sd:
        crop_l = sd.get("crop_left", 0)
        crop_r = sd.get("crop_right", 0)
        crop_ok = crop_l == left_pad and crop_r == right_pad
        sev = "FAIL" if strict_crop else "WARN"
        add(
            checks,
            "crop_side_data",
            crop_ok,
            f"crop_left={crop_l} crop_right={crop_r} expected_left={left_pad} expected_right={right_pad}",
            severity=sev,
        )
    else:
        add(checks, "crop_side_data", False, "no crop side data present", severity="WARN")

    # Frame sampling per user requirement.
    try:
        frame_count = get_video_frame_count(media_path, v)
        add(checks, "frame_count", frame_count > 0, f"frame_count={frame_count}")
    except Exception as exc:
        add(checks, "frame_count", False, f"failed to determine frame count: {exc}")
        return FileReport(path=media_path, checks=checks)

    indices = choose_sample_indices(frame_count)
    add(checks, "sampling_policy", True, f"sampled_frame_indices={indices}")

    all_pad_ok = True
    all_code_ok = True
    all_nominal_ok = True
    per_frame_notes: List[str] = []
    per_frame_nominal_notes: List[str] = []

    for idx in indices:
        try:
            yuv = decode_frame_yuv422p10(media_path, idx, width, height)
            pad_ok, pad_msg, code_ok, nominal_ok, code_msg = validate_padding_and_codes(
                yuv=yuv,
                width=width,
                height=height,
                left_pad=left_pad,
                right_pad=right_pad,
                pad_y_max=pad_y_max,
                pad_c_delta=pad_c_delta,
                legal_tol=legal_tol,
                studio_min=studio_min,
                studio_max=studio_max,
            )
        except Exception as exc:
            all_pad_ok = False
            all_code_ok = False
            all_nominal_ok = False
            per_frame_notes.append(f"frame {idx}: decode/check failed: {exc}")
            per_frame_nominal_notes.append(f"frame {idx}: decode/check failed: {exc}")
            continue

        if not pad_ok:
            all_pad_ok = False
        if not code_ok:
            all_code_ok = False
        if not nominal_ok:
            all_nominal_ok = False

        per_frame_notes.append(f"frame {idx}: padding=({'PASS' if pad_ok else 'FAIL'}), codes=({'PASS' if code_ok else 'FAIL'})")
        per_frame_nominal_notes.append(
            f"frame {idx}: nominal_codes=({'PASS' if nominal_ok else 'WARN'}) {code_msg}"
        )

    add(checks, "active_window_and_padding", all_pad_ok, " | ".join(per_frame_notes))
    add(checks, "bt601_code_legality", all_code_ok, " | ".join(per_frame_notes))
    add(
        checks,
        "bt601_nominal_range",
        all_nominal_ok,
        " | ".join(per_frame_nominal_notes),
        severity="WARN",
    )

    return FileReport(path=media_path, checks=checks)


def print_report(reports: List[FileReport], strict_warnings: bool) -> int:
    pass_count = 0
    fail_count = 0

    for rep in reports:
        failed = rep.failed(strict_warnings)
        if failed:
            fail_count += 1
        else:
            pass_count += 1

        print(("FAIL" if failed else "PASS") + f": {rep.path}")
        for c in rep.checks:
            if c.ok:
                print(f"  PASS {c.name}: {c.message}")
            else:
                tag = "WARN" if c.severity == "WARN" else "FAIL"
                print(f"  {tag} {c.name}: {c.message}")

    print()
    print("Summary")
    print(f"  total files: {len(reports)}")
    print(f"  pass: {pass_count}")
    print(f"  fail: {fail_count}")
    if strict_warnings:
        print("  mode: strict warnings (WARN counted as FAIL)")

    return 1 if fail_count else 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Validate BT.601-related MKV requirements across a file set")
    p.add_argument(
        "--root",
        default=str(Path(__file__).resolve().parents[1]),
        help="repository root (default: parent of scripts/)",
    )
    p.add_argument(
        "--glob",
        action="append",
        default=["assets/mkv/**/*.mkv"],
        help="glob pattern(s) for MKV discovery; can be repeated",
    )
    p.add_argument("--left-pad", type=int, default=8, help="expected left padding width in pixels")
    p.add_argument("--right-pad", type=int, default=8, help="expected right padding width in pixels")
    p.add_argument("--pad-y-max", type=int, default=68, help="max Y code for black padding (10-bit)")
    p.add_argument("--pad-c-delta", type=int, default=8, help="max chroma delta from 512 in padding (10-bit)")
    p.add_argument(
        "--legal-tolerance-codes",
        type=int,
        default=0,
        help="extra legal-range tolerance in 10-bit code values",
    )
    p.add_argument(
        "--studio-min-code",
        type=int,
        default=4,
        help="minimum allowed 10-bit code for studio footroom/headroom checks",
    )
    p.add_argument(
        "--studio-max-code",
        type=int,
        default=1019,
        help="maximum allowed 10-bit code for studio footroom/headroom checks",
    )
    p.add_argument(
        "--strict-crop",
        action="store_true",
        help="treat crop side-data mismatch as FAIL (default is WARN)",
    )
    p.add_argument(
        "--strict-warnings",
        action="store_true",
        help="treat WARN checks as failures for overall per-file status",
    )
    return p


def main() -> int:
    args = build_parser().parse_args()
    root = Path(args.root).resolve()

    missing = require_tools(["ffprobe", "ffmpeg"])
    if missing:
        print("Missing required tools: " + ", ".join(missing), file=sys.stderr)
        return 2

    mkv_files = discover_mkvs(root, args.glob)
    if not mkv_files:
        print("No MKV files found for the provided glob patterns.", file=sys.stderr)
        return 2

    reports: List[FileReport] = []
    for path in mkv_files:
        reports.append(
            validate_one(
                media_path=path,
                left_pad=max(0, args.left_pad),
                right_pad=max(0, args.right_pad),
                pad_y_max=max(0, args.pad_y_max),
                pad_c_delta=max(0, args.pad_c_delta),
                strict_crop=bool(args.strict_crop),
                legal_tol=max(0, args.legal_tolerance_codes),
                studio_min=max(0, args.studio_min_code),
                studio_max=min(1023, args.studio_max_code),
            )
        )

    return print_report(reports, strict_warnings=bool(args.strict_warnings))


if __name__ == "__main__":
    raise SystemExit(main())
