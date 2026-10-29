/*
 * File:        impairment_seed.h
 * Module:      impairment_seed
 * Purpose:     Derives independent per-frame RNG seeds for impairment layers
 *              from a user seed, a per-layer domain constant and a frame key.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstdint>

namespace videosynth {

// Per-layer domain constants. Each new impairment layer takes its own value,
// so two layers configured with the same user seed still draw independent
// streams.
constexpr std::uint64_t kSharedNoiseSeedDomain = 0x5052455353494E47ULL;
constexpr std::uint64_t kChromaJitterSeedDomain = 0x4348524F4D414A54ULL;
constexpr std::uint64_t kRandomDropoutSeedDomain = 0x52414E444F4D444FULL;
constexpr std::uint64_t kScratchDropoutSeedDomain = 0x5343524154434844ULL;

// splitmix64 finaliser: a bijective 64-bit mix with full avalanche.
constexpr std::uint64_t SplitMix64(std::uint64_t value) {
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31U);
}

// Seed for one frame of one impairment layer. frame_key is the layer's
// frame axis: the disc frame index for pressing impairments, the output frame
// index for capture impairments.
//
// Thread-safety: Pure function; safe to call concurrently.
constexpr std::uint64_t DeriveImpairmentSeed(std::uint64_t user_seed,
                                             std::uint64_t domain,
                                             std::uint64_t frame_key) {
  return SplitMix64(SplitMix64(user_seed ^ domain) ^ frame_key);
}

}  // namespace videosynth
