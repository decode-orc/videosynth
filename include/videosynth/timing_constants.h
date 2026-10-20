/*
 * File:        timing_constants.h
 * Module:      timing_constants
 * Purpose:     Provides PAL and NTSC timing and signal level constants.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include "videosynth/model.h"

namespace videosynth {

// Thread-safety: All functions and structs in this module are thread-safe.
// They are stateless and only operate on their parameters or return new
// values. May be called concurrently from multiple threads.
struct TimingConstants {
  int lines_per_frame = 0;
  int samples_per_line_4fsc = 0;
  double frame_rate_hz = 0.0;
  double sample_rate_4fsc_hz = 0.0;
};

struct SignalLevels {
  double sync_tip_mv = 0.0;
  double blanking_mv = 0.0;
  double black_mv = 0.0;
  double white_mv = 0.0;
};

inline TimingConstants GetTimingConstants(Standard standard) {
  if (Is625LineStandard(standard)) {
    // SECAM shares the 625-line timing and the PAL 4fsc storage rate (CVBS File
    // Format Specification v1.7.0, Preset: SECAM, Sampling rate).
    return TimingConstants{
        // ITU-R BT.1700 Annex 1 Part B Table 1 item 1: 625-line PAL.
        .lines_per_frame = 625,
        // EBU Tech. 3280-E Section 1.2: 1135.0064 samples/line nominal,
        // modelled here as 1135 integer samples plus phase-slip handling
        // elsewhere.
        .samples_per_line_4fsc = 1135,
        // ITU-R BT.1700 Annex 1 Part B Table 1 item 3: 2fH/625 = 25 frames/s.
        .frame_rate_hz = 25.0,
        // EBU Tech. 3280-E Section 1.1.1 Table 1: 4fsc = 17.734475 MHz.
        .sample_rate_4fsc_hz = 17734475.0,
    };
  }

  if (standard == Standard::kNtsc) {
    return TimingConstants{
        // SMPTE 170M-2004 Section 11.3: 525 lines/frame, 2:1 interlace.
        .lines_per_frame = 525,
        // SMPTE 244M-2003 Section 4.1.1: 910 samples/line.
        .samples_per_line_4fsc = 910,
        // SMPTE 170M-2004 Section 11.3: 30000/1001 frames/s.
        .frame_rate_hz = 30000.0 / 1001.0,
        // SMPTE 244M-2003 Section 3.4: 4fsc clock of 14.31818 MHz.
        .sample_rate_4fsc_hz = 14318180.0,
    };
  }

  if (standard == Standard::kPalM) {
    return TimingConstants{
        // ITU-R BT.470-6 Table 1 item 1: System M — 525 lines/frame.
        .lines_per_frame = 525,
        // ITU-R BT.470-6 Table 2 item 2.11b: M/PAL fsc = (909/4) × fH.
        // At 4fsc, this yields exactly 909 samples per line.
        .samples_per_line_4fsc = 909,
        // ITU-R BT.470-6 Table 1 item 2: System M field frequency 60 Hz
        // (59.94 for colour). M/PAL uses the same M-system frame rate.
        .frame_rate_hz = 30000.0 / 1001.0,
        // ITU-R BT.470-6 Table 2 item 2.11a: M/PAL nominal fsc =
        // 3 579 611.49 Hz; 4fsc = 14 318 445.96 Hz.
        .sample_rate_4fsc_hz = 14318445.96,
    };
  }

  throw std::invalid_argument(
      "Timing constants requested for unknown standard");
}

inline SignalLevels GetSignalLevels(Standard standard) {
  if (Is625LineStandard(standard)) {
    // ITU-R BT.1700 Annex 1 Part C Table 5: SECAM uses the same levels, with
    // the CVBS SECAM preset defaulting to no set-up (black = blanking).
    return SignalLevels{
        // ITU-R BT.1700 Annex 1 Part B Table 2 item 3: 625 PAL sync level.
        .sync_tip_mv = -300.0,
        // ITU-R BT.1700 Annex 1 Part B Table 2 item 1: blanking reference.
        .blanking_mv = 0.0,
        // ITU-R BT.1700 Annex 1 Part B Table 2 item 4: 625 PAL setup = 0 mV.
        .black_mv = 0.0,
        // ITU-R BT.1700 Annex 1 Part B Table 2 item 2: white level.
        .white_mv = 700.0,
    };
  }

  if (standard == Standard::kNtsc) {
    return SignalLevels{
        // SMPTE 170M-2004 Section 12.1/12.3 Table 1: sync is -40 IRE, and
        // 140 IRE = 1 V, so sync is -40/140 * 1000 = -285.7 mV.
        .sync_tip_mv = -285.7,
        // SMPTE 170M-2004 Section 12.2: blanking reference is 0 IRE.
        .blanking_mv = 0.0,
        // SMPTE 170M-2004 Section 12.3 Table 1: black is +7.5 IRE, and
        // 140 IRE = 1 V, so black is 7.5/140 * 1000 = 53.6 mV.
        .black_mv = 53.6,
        // SMPTE 170M-2004 Section 12.3 Table 1: white is +100 IRE, and
        // 140 IRE = 1 V, so white is 100/140 * 1000 = 714.3 mV.
        .white_mv = 714.3,
    };
  }

  if (standard == Standard::kPalM) {
    // ITU-R BT.470-6 Table 1 item 4: System M signal levels.
    // PAL-M uses System M sync/luminance levels identical to M/NTSC.
    return SignalLevels{
        // ITU-R BT.470-6 Table 1 item 4: sync level = -40 IRE.
        .sync_tip_mv = -285.7,
        // ITU-R BT.470-6 Table 1 item 4: blanking reference = 0 IRE.
        .blanking_mv = 0.0,
        // ITU-R BT.470-6 Table 1 item 4: black-blanking diff = 7.5 IRE.
        .black_mv = 53.6,
        // ITU-R BT.470-6 Table 1 item 4: peak white = 100 IRE.
        .white_mv = 714.3,
    };
  }

  throw std::invalid_argument("Signal levels requested for unknown standard");
}

inline SignalLevels GetSignalLevels(const CvbsPresets& presets) {
  const Standard standard = presets.video_standard_preset;
  // Both NTSC and PAL-M use System M signal levels with a configurable black
  // setup pedestal. PAL-M shares the M-system 7.5 IRE black setup.
  if (standard != Standard::kNtsc && standard != Standard::kPalM) {
    return GetSignalLevels(standard);
  }

  if (!IsSupportedNtscBlackSetupIre(presets.ntsc_black_setup_ire)) {
    throw std::invalid_argument(
        "Signal levels requested for unsupported NTSC black setup IRE");
  }

  SignalLevels levels = GetSignalLevels(standard);
  if (std::abs(presets.ntsc_black_setup_ire) < 1e-9) {
    levels.black_mv = levels.blanking_mv;
  }
  return levels;
}

inline int SamplesPerFrame4fsc(Standard standard) {
  if (Is625LineStandard(standard)) {
    // EBU Tech. 3280-E Section 1.2: PAL 4fsc has 1135.0064 samples/line,
    // yielding 709,379 samples/frame over 625 lines.
    return 709379;
  }

  if (standard == Standard::kNtsc) {
    // SMPTE 244M-2003 Section 4.1.1: 910 samples/line over 525 lines.
    return 910 * 525;
  }

  if (standard == Standard::kPalM) {
    // ITU-R BT.470-6 Table 2 item 2.11b: fsc/fH = 909/4 (exact), so 4fsc
    // yields exactly 909 samples per line over 525 System M lines.
    return 909 * 525;
  }

  throw std::invalid_argument(
      "Frame sample count requested for unknown standard");
}

// Audio is 48 kHz, clock-locked (synchronous) to video per the CVBS File Format
// Specification (Audio Data), following SMPTE 272M-1994 §1.2. All Video
// Standard Presets share the same 48000 Hz sampling clock; only the number of
// samples carried per video frame differs (SMPTE 272M §3.15, §14.3).

// Authoritative audio sample rate in Hz. Fixed at 48000 for every standard
// (SMPTE 272M §1.2 preferred implementation).
inline double AudioSampleRateHz(Standard standard) {
  if (Is625LineStandard(standard) || standard == Standard::kNtsc ||
      standard == Standard::kPalM) {
    return 48000.0;
  }
  throw std::invalid_argument(
      "Audio sample rate requested for unknown standard");
}

// Integer nSamplesPerSec written to the WAV `fmt ` chunk header. Always 48000
// (CVBS File Format Specification, WAV File Format).
inline int AudioHeaderSampleRateHz(Standard standard) {
  if (Is625LineStandard(standard) || standard == Standard::kNtsc ||
      standard == Standard::kPalM) {
    return 48000;
  }
  throw std::invalid_argument(
      "Audio header sample rate requested for unknown standard");
}

// Number of audio samples carried by stored video frame `frame_index`
// (zero-based, in output order). PAL and SECAM are a constant 1920
// samples/frame (48000 / 25). NTSC and PAL_M follow the SMPTE 272M §14.3 Table
// 1 five-frame audio-frame sequence at 48 kHz: 1602 samples in audio frame
// numbers 1, 3, 5 and 1601 in audio frame numbers 2, 4 — i.e. 1601 when
// (frame_index mod 5) is 1 or 3, otherwise 1602 (8008 samples per five-frame
// sequence).
inline int AudioSamplesForFrame(Standard standard, std::int64_t frame_index) {
  if (Is625LineStandard(standard)) {
    return 1920;
  }
  if (standard == Standard::kNtsc || standard == Standard::kPalM) {
    const std::int64_t position_in_sequence = frame_index % 5;
    return (position_in_sequence == 1 || position_in_sequence == 3) ? 1601
                                                                    : 1602;
  }
  throw std::invalid_argument(
      "Audio samples-per-frame requested for unknown standard");
}

// LaserDisc digital audio (EFM) is carried at its own sampling frequency,
// independent of the 48 kHz CVBS audio grid: the EFM path synthesises directly
// at 44.1 kHz rather than resampling the 48 kHz stream.

// Authoritative EFM audio sample rate in Hz. Both LaserDisc digital audio
// amendments lock the sampling frequency to the line frequency and both
// evaluate to exactly 44 100 Hz at the nominal line frequency:
// IEC 60856-1986 Amd 2 13.2: Fs = 1764/625 x fH = 1764/625 x 15 625 Hz (PAL).
// IEC 60857-1986 Amd 2 13.2: Fs = 7007/2500 x fH = 7007/2500 x (4.5 MHz/286)
// (NTSC).
inline double EfmAudioSampleRateHz() { return 44100.0; }

// Number of 44.1 kHz EFM audio samples carried by stored video frame
// `frame_index` (zero-based, in output order). Only PAL and NTSC define
// LaserDisc digital audio; other standards are rejected.
//
// PAL is a constant 1764 samples/frame (1764/625 x fH over 25 frames/s is
// exact, IEC 60856-1986 Amd 2 13.2). NTSC follows the SMPTE 272M-1994 Section
// 14.3 Table 1 audio-frame sequence for 44.1 kHz: a 100-frame sequence
// carrying 147 147 samples, where audio frame numbers (1-based within the
// sequence, frame number 1 at output frame 0) are 1472 samples for odd frames
// and 1471 for even frames, except frame numbers 23, 47 and 71 which carry
// 1471.
inline int EfmAudioSamplesForFrame(Standard standard,
                                   std::int64_t frame_index) {
  if (frame_index < 0) {
    throw std::invalid_argument(
        "EFM audio samples-per-frame requested for a negative frame index");
  }
  if (standard == Standard::kPal) {
    return 1764;
  }
  if (standard == Standard::kNtsc) {
    // SMPTE 272M-1994 Section 14.3 Table 1: 44.1 kHz, frame sequence 100.
    const std::int64_t frame_number = (frame_index % 100) + 1;
    if (frame_number % 2 == 0) {
      return 1471;
    }
    if (frame_number == 23 || frame_number == 47 || frame_number == 71) {
      return 1471;
    }
    return 1472;
  }
  throw std::invalid_argument(
      "EFM audio samples-per-frame requested for a standard without a "
      "LaserDisc digital audio specification");
}

inline double SampleRateHzForEncodingPreset(Standard standard,
                                            const std::string& preset) {
  const TimingConstants timing = GetTimingConstants(standard);
  if (Is4fscSampleEncodingPreset(preset)) {
    return timing.sample_rate_4fsc_hz;
  }
  if (preset == "RAW_S16_28M") {
    return 28000000.0;
  }
  if (preset == "RAW_S16_40M") {
    return 40000000.0;
  }
  return 0.0;
}

inline std::size_t SamplesPerFrameForEncodingPreset(Standard standard,
                                                    const std::string& preset) {
  const TimingConstants timing = GetTimingConstants(standard);
  const double sample_rate_hz = SampleRateHzForEncodingPreset(standard, preset);
  if (sample_rate_hz <= 0.0) {
    return 0U;
  }

  if (Is4fscSampleEncodingPreset(preset)) {
    return static_cast<std::size_t>(SamplesPerFrame4fsc(standard));
  }

  if (timing.frame_rate_hz <= 0.0) {
    return 0U;
  }

  return static_cast<std::size_t>(
      std::llround(sample_rate_hz / timing.frame_rate_hz));
}

}  // namespace videosynth
