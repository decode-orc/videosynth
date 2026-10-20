/*
 * File:        noise_injection_stage.cpp
 * Module:      noise_injection
 * Purpose:     Applies per-section two-component Gaussian noise to fixed-point
 *              mV Y/C frame buffers before output quantisation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/noise_injection_stage.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>

#include "videosynth/fixed_point.h"
#include "videosynth/timing_constants.h"

namespace videosynth {

namespace {

// Per-standard legal mV bounds derived from the quantisation profiles used by
// OutputStage: (code - blanking_code) * millivolts_per_code.
//
// PAL:  blanking=256, min_code=4,  max_code=1019, 1.1905 mV/code.
// NTSC: blanking=240, min_code=16, max_code=1019, 1.2755 mV/code.
constexpr double kPalClampMinMv = -300.006;
constexpr double kPalClampMaxMv = 908.452;
constexpr double kNtscClampMinMv = -285.712;
constexpr double kNtscClampMaxMv = 993.984;

struct NoiseCoefficients {
  double sigma_f_mv;  // floor noise std dev in mV
  double k;           // proportional coefficient (dimensionless, ire/ire)
};

// Derives floor noise (sigma_f_mv) and proportional coefficient (k) from the
// project noise parameters and the standard's mV-per-IRE ratio.
//
// Parameter derivation follows noise-injection-design.md §Parameter Derivation.
NoiseCoefficients DeriveNoiseCoefficients(const NoiseParameters& noise,
                                          double white_mv) {
  // 1. Floor noise: sigma_f_ire = 100 / 10^(noise_db/20)
  //    sigma_f_mV = sigma_f_ire * (white_mV / 100)  — converts IRE to mV
  const double sigma_f_ire = 100.0 / std::pow(10.0, noise.noise_db / 20.0);
  const double sigma_f_mv = sigma_f_ire * (white_mv / 100.0);

  // 2. Proportional coefficient: solve sigma_w^2 = sigma_f^2 + (k * 100)^2
  double k = 0.0;
  if (noise.noise_spread_db > 0.0) {
    const double target_white_snr = noise.noise_db - noise.noise_spread_db;
    const double sigma_w_ire = 100.0 / std::pow(10.0, target_white_snr / 20.0);
    if (sigma_w_ire > sigma_f_ire) {
      const double sigma_prop_ire =
          std::sqrt(sigma_w_ire * sigma_w_ire - sigma_f_ire * sigma_f_ire);
      k = sigma_prop_ire / 100.0;
    }
  }

  return NoiseCoefficients{sigma_f_mv, k};
}

// Returns the section index for the given section pointer, or 0 if the pointer
// does not address an element of project.sections.
//
// Schedule items point into the project's contiguous section vector, so the
// index is recoverable by pointer arithmetic in O(1) — a linear scan here runs
// once per frame on every noise-enabled project.
std::size_t FindSectionIndex(const Project& project,
                             const Section* target_section) {
  const Section* first = project.sections.data();
  if (first == nullptr || target_section < first ||
      target_section >= (first + project.sections.size())) {
    return 0;
  }
  return static_cast<std::size_t>(target_section - first);
}

}  // namespace

NoiseInjectionStage::NoiseInjectionStage(ILogger* logger)
    : logger_(logger), run_base_seed_(std::random_device{}()) {}

void NoiseInjectionStage::InjectNoise(
    const Project& project,
    const std::vector<IGenerationStage::FrameScheduleItem>& schedule,
    std::size_t frame_offset, std::size_t frame_count,
    std::vector<SampleFixed>* y_mv, std::vector<SampleFixed>* c_mv) const {
  if (y_mv == nullptr || c_mv == nullptr) {
    return;
  }

  const Standard standard = project.cvbs_presets.video_standard_preset;
  const std::size_t samples_per_frame =
      static_cast<std::size_t>(SamplesPerFrameForEncodingPreset(
          standard, project.cvbs_presets.sample_encoding_preset));
  if (samples_per_frame == 0U) {
    return;
  }

  const SignalLevels levels = GetSignalLevels(project.cvbs_presets);
  const double white_mv = levels.white_mv;

  const SampleFixed clamp_min = MillivoltsToSampleFixed(
      Is625LineStandard(standard) ? kPalClampMinMv : kNtscClampMinMv);
  const SampleFixed clamp_max = MillivoltsToSampleFixed(
      Is625LineStandard(standard) ? kPalClampMaxMv : kNtscClampMaxMv);

  for (std::size_t i = 0; i < frame_count; ++i) {
    const std::size_t global_frame = frame_offset + i;
    if (global_frame >= schedule.size()) {
      break;
    }
    const Section* section = schedule[global_frame].section;
    if (section == nullptr || !section->noise.enabled) {
      continue;
    }

    const NoiseCoefficients coeffs =
        DeriveNoiseCoefficients(section->noise, white_mv);

    // Per-frame seed independent of batch size.
    // base_seed is the user-supplied noise_seed (deterministic across runs) or
    // run_base_seed_ (random per run, fixed within one pipeline invocation).
    // Two large primes decorrelate the section and frame axes.
    const std::size_t section_index = FindSectionIndex(project, section);
    const std::uint64_t base_seed =
        section->noise.noise_seed_specified
            ? static_cast<std::uint64_t>(section->noise.noise_seed)
            : run_base_seed_;
    const std::uint64_t seed =
        (base_seed ^
         (static_cast<std::uint64_t>(section_index) * 2654435761ULL)) ^
        (static_cast<std::uint64_t>(global_frame) * 2246822519ULL);
    std::mt19937_64 rng(seed);

    if (logger_ != nullptr) {
      logger_->Trace("NoiseInjectionStage: frame " +
                     std::to_string(global_frame) + " section '" +
                     section->name +
                     "' sigma_f=" + std::to_string(coeffs.sigma_f_mv) +
                     " mV k=" + std::to_string(coeffs.k));
    }

    const std::size_t frame_start = i * samples_per_frame;

    // One distribution for the whole frame region, drawing unit normals that
    // are then scaled to the sample's sigma. Constructing a distribution per
    // sample threw away half of every Box-Muller pair; a persistent one keeps
    // the second deviate and halves the underlying engine draws.
    std::normal_distribution<double> unit_normal(0.0, 1.0);

    // With no spread configured the proportional term vanishes and sigma is
    // constant across the whole frame, so neither the Y level nor the square
    // root is needed per sample.
    const bool sigma_is_signal_dependent = coeffs.k != 0.0;
    const double sigma_floor_squared = coeffs.sigma_f_mv * coeffs.sigma_f_mv;

    for (std::size_t s = 0; s < samples_per_frame; ++s) {
      const std::size_t idx = frame_start + s;

      // sigma_total combines the signal-independent floor and the
      // signal-proportional component driven by the current Y level.
      double sigma_total = coeffs.sigma_f_mv;
      if (sigma_is_signal_dependent) {
        const double prop_component =
            coeffs.k * SampleFixedToMillivolts((*y_mv)[idx]);
        sigma_total =
            std::sqrt(sigma_floor_squared + (prop_component * prop_component));
      }

      // Single noise draw shared by both channels — produces correlated Y/C
      // noise that matches the physical behaviour of a real analogue source.
      const SampleFixed noise_sample =
          MillivoltsToSampleFixed(sigma_total * unit_normal(rng));

      (*y_mv)[idx] =
          std::clamp((*y_mv)[idx] + noise_sample, clamp_min, clamp_max);
      (*c_mv)[idx] =
          std::clamp((*c_mv)[idx] + noise_sample, clamp_min, clamp_max);
    }
  }
}

}  // namespace videosynth
