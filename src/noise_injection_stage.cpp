/*
 * File:        noise_injection_stage.cpp
 * Module:      noise_injection
 * Purpose:     Applies per-section capture and shared (pressing) Gaussian
 *              noise to fixed-point mV Y/C frame buffers before output
 *              quantisation.
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

#include "videosynth/channel_stage.h"
#include "videosynth/fixed_point.h"
#include "videosynth/frame_enrichment.h"
#include "videosynth/impairment_seed.h"
#include "videosynth/sample_clamp.h"
#include "videosynth/timing_constants.h"
#include "videosynth/timing_offset_filter.h"

namespace videosynth {

namespace {

struct NoiseCoefficients {
  double sigma_f_mv;  // floor noise std dev in mV
  double k;           // proportional coefficient (dimensionless, ire/ire)
};

// Converts a noise level in dB to a signal-independent sigma in mV:
// sigma_ire = 100 / 10^(noise_db/20), then IRE to mV via white_mV / 100.
double FloorSigmaMv(double noise_db, double white_mv) {
  return (100.0 / std::pow(10.0, noise_db / 20.0)) * (white_mv / 100.0);
}

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

// Draws count + taps - 1 unit normals and shapes them into count samples, so
// the shaped sequence has unit variance and no filter edge effects.
std::vector<double> DrawShapedUnitNormals(
    const NoiseSpectrumFilter& filter, std::size_t count, std::mt19937_64* rng,
    std::normal_distribution<double>* unit_normal) {
  std::vector<double> white(count + filter.kernel().size() - 1U);
  for (double& value : white) {
    value = (*unit_normal)(*rng);
  }
  std::vector<double> shaped;
  filter.Apply(white, count, &shaped);
  return shaped;
}

// Amplitude gain of a level curve at level_mv: the curve is linear in dB
// between breakpoints and holds its end values beyond them. An empty curve is
// 0 dB everywhere. O(points) per call; curves hold a handful of points.
double LevelCurveGain(const std::vector<NoiseLevelPoint>& curve,
                      double level_mv) {
  if (curve.empty()) {
    return 1.0;
  }
  double gain_db = curve.back().relative_db;
  if (level_mv <= curve.front().level_mv) {
    gain_db = curve.front().relative_db;
  } else {
    for (std::size_t i = 1; i < curve.size(); ++i) {
      if (level_mv <= curve[i].level_mv) {
        const NoiseLevelPoint& lower = curve[i - 1];
        const NoiseLevelPoint& upper = curve[i];
        const double fraction =
            (level_mv - lower.level_mv) / (upper.level_mv - lower.level_mv);
        gain_db = lower.relative_db +
                  (fraction * (upper.relative_db - lower.relative_db));
        break;
      }
    }
  }
  return std::pow(10.0, gain_db / 20.0);
}

}  // namespace

NoiseInjectionStage::NoiseInjectionStage(ILogger* logger)
    : logger_(logger), run_base_seed_(std::random_device{}()) {}

bool NoiseInjectionStage::NeedsPressingLuma(const Project& project) {
  if (!ChannelStage::IsActive(project.channel)) {
    return false;
  }
  return std::any_of(project.sections.begin(), project.sections.end(),
                     [](const Section& section) {
                       return section.noise.shared.enabled &&
                              section.noise.shared.level_term.enabled;
                     });
}

void NoiseInjectionStage::InjectNoise(
    const Project& project,
    const std::vector<IGenerationStage::FrameScheduleItem>& schedule,
    std::size_t frame_offset, std::size_t frame_count,
    std::vector<SampleFixed>* y_mv, std::vector<SampleFixed>* c_mv) const {
  InjectNoise(project, schedule, frame_offset, frame_count, nullptr, y_mv,
              c_mv);
}

void NoiseInjectionStage::InjectNoise(
    const Project& project,
    const std::vector<IGenerationStage::FrameScheduleItem>& schedule,
    std::size_t frame_offset, std::size_t frame_count,
    const std::vector<SampleFixed>* pressing_y_mv,
    std::vector<SampleFixed>* y_mv, std::vector<SampleFixed>* c_mv) const {
  if (y_mv == nullptr || c_mv == nullptr) {
    return;
  }
  // The shared level term reads the pressing's picture. Without a separate
  // copy, Y as it arrives (before any noise) stands in for it, which is exact
  // when no channel stage has altered the picture.
  const std::vector<SampleFixed>& pressing_luma =
      (pressing_y_mv != nullptr && pressing_y_mv->size() == y_mv->size())
          ? *pressing_y_mv
          : *y_mv;

  const Standard standard = project.cvbs_presets.video_standard_preset;
  // Generation always fills 4fsc frames; the output stage resamples RAW
  // encodings afterwards, so frames and the spectrum are both at 4fsc here.
  const auto samples_per_frame =
      static_cast<std::size_t>(SamplesPerFrame4fsc(standard));

  const SignalLevels levels = GetSignalLevels(project.cvbs_presets);
  const double white_mv = levels.white_mv;
  const double sample_rate_hz =
      GetTimingConstants(standard).sample_rate_4fsc_hz;

  const SampleClampBounds bounds = SampleClampBoundsFor(standard);
  const SampleFixed clamp_min = bounds.min;
  const SampleFixed clamp_max = bounds.max;

  // noise_db is the level of each output signal. Y/C output carries the noise
  // on both files, correlated sample for sample; composite output is Y + C, so
  // adding the sample to both would double it (+6 dB) and it goes on Y alone.
  const bool noise_on_chroma = (project.output.signal_type == "yc");

  // The pressing noise is read off the disc with the picture, so a capture's
  // timing offset, applied to the picture by the channel stage, delays the
  // shared layer too. The capture layer belongs to the capture alone and stays
  // on the output grid.
  const bool delay_shared = ChannelStage::IsActive(project.channel) &&
                            project.channel.timing_offset_samples != 0.0;
  const TimingOffsetFilter shared_delay(
      delay_shared ? project.channel.timing_offset_samples : 0.0);
  std::vector<SampleFixed> shared_delayed;

  for (std::size_t i = 0; i < frame_count; ++i) {
    const std::size_t global_frame = frame_offset + i;
    if (global_frame >= schedule.size()) {
      break;
    }
    const Section* section = schedule[global_frame].section;
    if (section == nullptr ||
        (!section->noise.enabled && !section->noise.shared.enabled)) {
      continue;
    }
    const NoiseParameters& noise = section->noise;
    const std::size_t frame_start = i * samples_per_frame;

    // Shared (pressing) layer first, into its own buffer: it is keyed by disc
    // position only. Its constant component does not depend on the signal;
    // the optional level term follows the pressing's own picture, so it too
    // is identical in every capture of the pressing.
    std::vector<SampleFixed> shared_noise;
    if (noise.shared.enabled) {
      const IGenerationStage::FrameScheduleItem& item = schedule[global_frame];
      const std::uint64_t disc_frame_key =
          item.enrichment != nullptr
              ? static_cast<std::uint64_t>(item.enrichment->disc_frame_index)
              : static_cast<std::uint64_t>(global_frame);
      std::mt19937_64 shared_rng(DeriveImpairmentSeed(
          static_cast<std::uint64_t>(noise.shared.noise_seed),
          kSharedNoiseSeedDomain, disc_frame_key));
      std::normal_distribution<double> shared_normal(0.0, 1.0);
      const double sigma_shared_mv =
          FloorSigmaMv(noise.shared.noise_db, white_mv);

      shared_noise.resize(samples_per_frame);
      if (noise.shared.spectrum.empty()) {
        for (SampleFixed& value : shared_noise) {
          value = MillivoltsToSampleFixed(sigma_shared_mv *
                                          shared_normal(shared_rng));
        }
      } else {
        const std::vector<double> shaped = DrawShapedUnitNormals(
            *FilterFor(noise.shared.spectrum, sample_rate_hz),
            samples_per_frame, &shared_rng, &shared_normal);
        for (std::size_t s = 0; s < samples_per_frame; ++s) {
          shared_noise[s] =
              MillivoltsToSampleFixed(sigma_shared_mv * shaped[s]);
        }
      }
      // The level term is a second pressing component on the pressing's own
      // grid, drawn from its own seed domain so adding it leaves the constant
      // component unchanged. Both are delayed together below.
      const SharedNoiseLevelTerm& term = noise.shared.level_term;
      if (term.enabled) {
        std::mt19937_64 term_rng(DeriveImpairmentSeed(
            static_cast<std::uint64_t>(noise.shared.noise_seed),
            kSharedLevelNoiseSeedDomain, disc_frame_key));
        std::normal_distribution<double> term_normal(0.0, 1.0);
        std::vector<double> term_unit;
        if (term.spectrum.empty()) {
          term_unit.resize(samples_per_frame);
          for (double& value : term_unit) {
            value = term_normal(term_rng);
          }
        } else {
          term_unit =
              DrawShapedUnitNormals(*FilterFor(term.spectrum, sample_rate_hz),
                                    samples_per_frame, &term_rng, &term_normal);
        }
        const double sigma_term_mv = FloorSigmaMv(term.noise_db, white_mv);
        for (std::size_t s = 0; s < samples_per_frame; ++s) {
          // The curve is in mV above blanking, the same on every standard.
          const double level_mv =
              SampleFixedToMillivolts(pressing_luma[frame_start + s]) -
              levels.blanking_mv;
          shared_noise[s] += MillivoltsToSampleFixed(
              sigma_term_mv * LevelCurveGain(term.level_db, level_mv) *
              term_unit[s]);
        }
      }

      if (delay_shared) {
        shared_delayed.resize(samples_per_frame);
        shared_delay.Apply(shared_noise.data(), samples_per_frame,
                           shared_delayed.data());
        shared_noise.swap(shared_delayed);
      }

      if (logger_ != nullptr) {
        logger_->Trace("NoiseInjectionStage: frame " +
                       std::to_string(global_frame) + " section '" +
                       section->name +
                       "' shared sigma=" + std::to_string(sigma_shared_mv) +
                       " mV level_term=" + (term.enabled ? "on" : "off") +
                       " disc_frame=" + std::to_string(disc_frame_key));
      }
    }

    if (!noise.enabled) {
      for (std::size_t s = 0; s < samples_per_frame; ++s) {
        const std::size_t idx = frame_start + s;
        (*y_mv)[idx] =
            std::clamp((*y_mv)[idx] + shared_noise[s], clamp_min, clamp_max);
        if (noise_on_chroma) {
          (*c_mv)[idx] += shared_noise[s];
        }
      }
      continue;
    }

    const NoiseCoefficients coeffs = DeriveNoiseCoefficients(noise, white_mv);

    // Per-frame seed independent of batch size.
    // base_seed is the user-supplied noise_seed (deterministic across runs) or
    // run_base_seed_ (random per run, fixed within one pipeline invocation).
    // Two large primes decorrelate the section and frame axes.
    const std::size_t section_index = FindSectionIndex(project, section);
    const std::uint64_t base_seed =
        noise.noise_seed_specified
            ? static_cast<std::uint64_t>(noise.noise_seed)
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

    // One distribution for the whole frame region, drawing unit normals that
    // are then scaled to the sample's sigma. Constructing a distribution per
    // sample threw away half of every Box-Muller pair; a persistent one keeps
    // the second deviate and halves the underlying engine draws.
    std::normal_distribution<double> unit_normal(0.0, 1.0);

    // A configured spectrum shapes the unit normals before the per-sample
    // sigma is applied, so the level-dependent term modulates shaped noise.
    std::vector<double> shaped_unit;
    if (!noise.spectrum.empty()) {
      shaped_unit =
          DrawShapedUnitNormals(*FilterFor(noise.spectrum, sample_rate_hz),
                                samples_per_frame, &rng, &unit_normal);
    }
    const bool shaped = !shaped_unit.empty();

    // With no spread configured the proportional term vanishes and sigma is
    // constant across the whole frame, so neither the Y level nor the square
    // root is needed per sample.
    const bool sigma_is_signal_dependent = coeffs.k != 0.0;
    const double sigma_floor_squared = coeffs.sigma_f_mv * coeffs.sigma_f_mv;

    // A spread_spectrum gives the level-dependent term its own shape, so the
    // noise can grow with level in some bands and not others. The two terms
    // are then drawn independently and summed, keeping the total variance
    // sigma_f^2 + (k * Y)^2: floor = sigma_f * u_floor, level term =
    // k * Y * u_spread. The floor's unit normals come first from the RNG,
    // drawn into a buffer when white so the draw order does not interleave.
    std::vector<double> spread_unit;
    if (sigma_is_signal_dependent && !noise.spread_spectrum.empty()) {
      if (!shaped) {
        shaped_unit.resize(samples_per_frame);
        for (double& value : shaped_unit) {
          value = unit_normal(rng);
        }
      }
      spread_unit = DrawShapedUnitNormals(
          *FilterFor(noise.spread_spectrum, sample_rate_hz), samples_per_frame,
          &rng, &unit_normal);
    }
    const bool separate_spread = !spread_unit.empty();

    for (std::size_t s = 0; s < samples_per_frame; ++s) {
      const std::size_t idx = frame_start + s;

      double noise_mv = 0.0;
      if (separate_spread) {
        noise_mv =
            (coeffs.sigma_f_mv * shaped_unit[s]) +
            (coeffs.k * SampleFixedToMillivolts((*y_mv)[idx]) * spread_unit[s]);
      } else {
        // sigma_total combines the signal-independent floor and the
        // signal-proportional component driven by the current Y level.
        double sigma_total = coeffs.sigma_f_mv;
        if (sigma_is_signal_dependent) {
          const double prop_component =
              coeffs.k * SampleFixedToMillivolts((*y_mv)[idx]);
          sigma_total = std::sqrt(sigma_floor_squared +
                                  (prop_component * prop_component));
        }
        const double unit = shaped ? shaped_unit[s] : unit_normal(rng);
        noise_mv = sigma_total * unit;
      }

      // Single noise value shared by both channels on Y/C output — correlated
      // Y/C noise that matches the physical behaviour of a real analogue
      // source.
      SampleFixed noise_sample = MillivoltsToSampleFixed(noise_mv);
      if (!shared_noise.empty()) {
        noise_sample += shared_noise[s];
      }

      // The bounds are composite limits. C is zero-centred and saturated
      // chroma legitimately exceeds them, so only Y is clamped here; the
      // output stage clamps Y + C and the chroma file.
      (*y_mv)[idx] =
          std::clamp((*y_mv)[idx] + noise_sample, clamp_min, clamp_max);
      if (noise_on_chroma) {
        (*c_mv)[idx] += noise_sample;
      }
    }
  }
}

std::shared_ptr<const NoiseSpectrumFilter> NoiseInjectionStage::FilterFor(
    const std::vector<NoiseSpectrumPoint>& spectrum,
    double sample_rate_hz) const {
  std::vector<double> key;
  key.reserve(1U + (2U * spectrum.size()));
  key.push_back(sample_rate_hz);
  for (const NoiseSpectrumPoint& point : spectrum) {
    key.push_back(point.frequency_mhz);
    key.push_back(point.relative_db);
  }

  const std::lock_guard<std::mutex> lock(filter_cache_mutex_);
  std::shared_ptr<const NoiseSpectrumFilter>& filter = filter_cache_[key];
  if (filter == nullptr) {
    filter =
        std::make_shared<const NoiseSpectrumFilter>(spectrum, sample_rate_hz);
    if (logger_ != nullptr) {
      logger_->Debug("NoiseInjectionStage: designed " +
                     std::to_string(kNoiseSpectrumTaps) +
                     "-tap noise spectrum filter (" +
                     std::to_string(spectrum.size()) + " breakpoints).");
    }
  }
  return filter;
}

}  // namespace videosynth
