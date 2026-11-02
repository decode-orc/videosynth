# Shared helper, sourced by the still generators: wraps a planar float32
# R'G'B' frame (ffmpeg's gbrpf32le plane order G, B, R) as an EXR still.
#
# ffmpeg writes the EXR without a colour conversion (gbrpf32le is the EXR
# encoder's native format), and exrstdattr adds the metadata required by
# docs/exr-bt601-compliance-requirements.md (FLOAT RGB, no compression, gamma 1,
# framesPerSecond and pixelAspectRatio for the raster's standard). Needs ffmpeg
# and openexr on PATH.

# write_exr <raw.gbrpf32le> <out.exr> <width> <height>
write_exr() {
  local raw="$1" out="$2" width="$3" height="$4"
  local fps pixel_aspect
  # ITU-R BT.601-5 Part A: 625-line 128/117 and 525-line 108/119 pixel aspect.
  if [[ "${height}" == "576" ]]; then
    fps=(25 1)
    pixel_aspect=1.094017
  else
    fps=(30000 1001)
    pixel_aspect=0.907563
  fi
  local encoded
  encoded="$(mktemp --suffix=.exr)"
  ffmpeg -v error -f rawvideo -pix_fmt gbrpf32le -s:v "${width}x${height}" \
    -i "${raw}" -frames:v 1 -c:v exr -compression none -format float \
    -gamma 1 -y "${encoded}"
  mkdir -p "$(dirname "${out}")"
  exrstdattr -framesPerSecond "${fps[0]}" "${fps[1]}" \
    -pixelAspectRatio "${pixel_aspect}" "${encoded}" "${out}"
  rm -f "${encoded}"
  echo "Wrote ${out}"
}

# read_exr <in.exr> <out.gbrpf32le>: decodes an EXR still back to the planar
# float32 frame, for generators that compose over an existing asset.
read_exr() {
  ffmpeg -v error -i "$1" -frames:v 1 -f rawvideo -pix_fmt gbrpf32le -y "$2"
}
