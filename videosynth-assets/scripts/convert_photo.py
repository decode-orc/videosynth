#!/usr/bin/env python3
"""Converts a camera photograph to a planar float32 R'G'B' still.

Follows docs/exr-bt601-compliance-requirements.md for a picture source:

  1. Decode the JPEG (8-bit sRGB) and apply its EXIF orientation.
  2. Crop to the 4:3 picture videosynth maps onto the 52 us active line,
     at the given horizontal and vertical crop centre.
  3. Decode the sRGB transfer to linear light and downscale each channel to
     the raster with a triangle (linear) filter widened by the reduction
     ratio. The kernel is non-negative, so it adds no ringing or sharpening
     halos; no sharpening or noise reduction is applied.
  4. Re-encode with the sRGB transfer. The sRGB-encoded values are used as
     the gamma-corrected R'G'B' videosynth ingests; sRGB shares the BT.709
     primaries, which this conversion does not re-map to the EBU primaries.

Every step keeps the values inside [0, 1], so highlights and saturated
colours stay inside the legal gamut rather than being clipped by the
conversion (detail the camera itself clipped stays clipped).

Usage: convert_photo.py <photo.jpg> <crop_x> <crop_y> <width> <height> <out>
crop_x and crop_y are the crop centre as fractions of the image (0.5 = the
centre). Writes <out> as raw float32 in ffmpeg's gbrpf32le plane order
(G, B, R).
"""

import sys

import numpy as np
from PIL import Image, ImageOps

PICTURE_ASPECT = 4.0 / 3.0


def srgb_to_linear(v):
    # IEC 61966-2-1 section 5.2: sRGB decoding.
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(v):
    # IEC 61966-2-1 section 5.3: sRGB encoding.
    return np.where(v <= 0.0031308, v * 12.92,
                    1.055 * np.power(v, 1.0 / 2.4) - 0.055)


def crop_box(width, height, centre_x, centre_y):
    """Largest 4:3 box at the given crop centre, kept inside the image."""
    if width / height > PICTURE_ASPECT:
        crop_w, crop_h = round(height * PICTURE_ASPECT), height
    else:
        crop_w, crop_h = width, round(width / PICTURE_ASPECT)
    left = min(max(round(centre_x * width - crop_w / 2.0), 0), width - crop_w)
    top = min(max(round(centre_y * height - crop_h / 2.0), 0),
              height - crop_h)
    return (left, top, left + crop_w, top + crop_h)


def convert(path, centre_x, centre_y, width, height):
    image = ImageOps.exif_transpose(Image.open(path)).convert("RGB")
    image = image.crop(crop_box(image.width, image.height, centre_x,
                                centre_y))
    linear = srgb_to_linear(np.asarray(image, dtype=np.float64) / 255.0)
    planes = []
    for channel in range(3):
        plane = Image.fromarray(linear[:, :, channel].astype(np.float32),
                                mode="F")
        planes.append(np.asarray(plane.resize((width, height),
                                              Image.BILINEAR)))
    rgb = linear_to_srgb(np.clip(np.stack(planes, axis=2), 0.0, 1.0))
    assert rgb.min() >= 0.0 and rgb.max() <= 1.0
    return rgb


def main():
    if len(sys.argv) != 7:
        sys.exit(__doc__)
    path, centre_x, centre_y, width, height, out = sys.argv[1:]
    rgb = convert(path, float(centre_x), float(centre_y), int(width),
                  int(height))
    with open(out, "wb") as raw:
        for channel in (1, 2, 0):  # gbrpf32le plane order
            raw.write(np.ascontiguousarray(rgb[:, :, channel],
                                           dtype="<f4").tobytes())


if __name__ == "__main__":
    main()
