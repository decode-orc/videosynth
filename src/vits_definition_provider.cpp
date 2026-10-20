/*
 * File:        vits_definition_provider.cpp
 * Module:      vits
 * Purpose:     Implements lookup of built-in PAL and NTSC VITS waveform
 * definitions.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/vits_definition_provider.h"

#include <string>
#include <utility>
#include <vector>

namespace videosynth {

namespace {

VitsPrimitiveDefinition Primitive(
    std::string id, VitsPrimitiveType primitive_type,
    VitsSignalComponent signal_component, VitsCombineMode combine_mode,
    double start_us, double end_us, double rise_time_us,
    double level_or_amplitude, double dc_offset = 0.0,
    double frequency_mhz = 0.0, double subcarrier_lock_multiple = 0.0,
    double phase_deg = 0.0, int staircase_steps = 0,
    std::string continuity_group = "",
    VitsTransitionOutPolicy transition_out_policy =
        VitsTransitionOutPolicy::kNone,
    double transition_out_duration_us = 0.0) {
  return VitsPrimitiveDefinition{
      .id = std::move(id),
      .primitive_type = primitive_type,
      .signal_component = signal_component,
      .combine_mode = combine_mode,
      .continuity_group = std::move(continuity_group),
      .transition_out_policy = transition_out_policy,
      .transition_out_duration_us = transition_out_duration_us,
      .start_us = start_us,
      .end_us = end_us,
      .rise_time_us = rise_time_us,
      .level_or_amplitude = level_or_amplitude,
      .dc_offset = dc_offset,
      .frequency_mhz = frequency_mhz,
      .subcarrier_lock_multiple = subcarrier_lock_multiple,
      .phase_deg = phase_deg,
      .staircase_steps = staircase_steps,
  };
}

VitsCompositeDefinition Composite(std::string id, VitsCompositeMode mode,
                                  std::vector<std::string> children,
                                  std::string continuity_group = "",
                                  std::string baseline_anchor = "") {
  return VitsCompositeDefinition{
      .id = std::move(id),
      .mode = mode,
      .children = std::move(children),
      .continuity_group = std::move(continuity_group),
      .baseline_anchor = std::move(baseline_anchor),
  };
}

VitsDefinition Definition(Standard standard, std::string vits_type,
                          int recommended_frame_line,
                          VitsLevelsUnit levels_unit, double y_rise_time_us,
                          double c_rise_time_us) {
  return VitsDefinition{
      .standard = standard,
      .vits_type = std::move(vits_type),
      .recommended_frame_line = recommended_frame_line,
      .levels_unit = levels_unit,
      .timing_reference = VitsTimingReference::kSyncEdge,
      .y_rise_time_us = y_rise_time_us,
      .c_rise_time_us = c_rise_time_us,
      .primitives = {},
      .composites = {},
      .render_order = {},
  };
}

std::vector<VitsDefinition> BuildCatalog() {
  std::vector<VitsDefinition> catalog;
  catalog.reserve(11);

  {
    VitsDefinition definition = Definition(
        Standard::kPal, "vits17", 17, VitsLevelsUnit::kMillivolts, 0.20, 0.40);
    definition.primitives = {
        Primitive("white_reference", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 12.0,
                  22.0, 0.200, 700.0),
        Primitive("pulse_2t", VitsPrimitiveType::kSinSquaredPulse,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 25.8,
                  26.2, 0.200, 700.0),
        Primitive("modulated_y", VitsPrimitiveType::kSinSquaredPulse,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 30.0,
                  34.0, 0.200, 350.0),
        Primitive("modulated_c", VitsPrimitiveType::kCompositePulse,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 30.0, 34.0,
                  0.400, 350.0, 0.0, 0.0, 1.0, 90.0),
        Primitive("staircase_step_1", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 40.0,
                  44.0, 0.235, 140.0),
        Primitive("staircase_step_2", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 44.0,
                  48.0, 0.235, 280.0),
        Primitive("staircase_step_3", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 48.0,
                  52.0, 0.235, 420.0),
        Primitive("staircase_step_4", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 52.0,
                  56.0, 0.235, 560.0),
        Primitive("staircase_step_5", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 56.0,
                  62.0, 0.235, 700.0),
    };
    definition.composites = {
        Composite("modulated_pulse", VitsCompositeMode::kParallel,
                  {"modulated_y", "modulated_c"}),
    };
    definition.render_order = {"white_reference",  "pulse_2t",
                               "modulated_pulse",  "staircase_step_1",
                               "staircase_step_2", "staircase_step_3",
                               "staircase_step_4", "staircase_step_5"};
    catalog.push_back(std::move(definition));
  }

  {
    VitsDefinition definition =
        Definition(Standard::kPal, "itu-multiburst", 18,
                   VitsLevelsUnit::kMillivolts, 0.20, 0.40);
    definition.primitives = {
        Primitive("positive_reference_boost", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kAdd, 12.0, 16.0,
                  0.350, 210.0),
        Primitive("grey_pedestal", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 12.0,
                  62.0, 0.350, 350.0),
        Primitive("negative_reference_boost", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kAdd, 16.0, 20.0,
                  0.350, -210.0),
        Primitive("burst_0_5", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 24.0, 28.0,
                  0.200, 210.0, 0.0, 0.500),
        Primitive("burst_1_0", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 30.0, 35.0,
                  0.200, 210.0, 0.0, 1.000),
        Primitive("burst_2_0", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 36.0, 41.0,
                  0.200, 210.0, 0.0, 2.000),
        Primitive("burst_4_0", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 42.0, 47.0,
                  0.200, 210.0, 0.0, 4.000),
        Primitive("burst_4_8", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 48.0, 53.0,
                  0.200, 210.0, 0.0, 4.800, 0.0, 144.0),
        Primitive("burst_5_8", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 54.0, 60.0,
                  0.200, 210.0, 0.0, 5.800, 0.0, -144.0),
    };
    definition.composites = {
        Composite("reference_bar_pair", VitsCompositeMode::kSerial,
                  {"positive_reference_boost", "negative_reference_boost"},
                  "reference_bars", "grey_pedestal"),
        Composite("burst_train", VitsCompositeMode::kSerial,
                  {"burst_0_5", "burst_1_0", "burst_2_0", "burst_4_0",
                   "burst_4_8", "burst_5_8"}),
    };
    definition.render_order = {"grey_pedestal", "reference_bar_pair",
                               "burst_train"};
    catalog.push_back(std::move(definition));
  }

  {
    VitsDefinition definition =
        Definition(Standard::kPal, "uk-national", 19,
                   VitsLevelsUnit::kMillivolts, 0.20, 0.40);
    definition.primitives = {
        Primitive("white_reference", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 12.0,
                  22.0, 0.200, 700.0),
        Primitive("pulse_2t", VitsPrimitiveType::kSinSquaredPulse,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 25.8,
                  26.2, 0.200, 700.0),
        Primitive("modulated_c", VitsPrimitiveType::kCompositePulse,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 29.0, 31.0,
                  0.400, 350.0, 0.0, 0.0, 1.0, 90.0),
        Primitive("modulated_y", VitsPrimitiveType::kSinSquaredPulse,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 29.0,
                  31.0, 0.200, 350.0),
        Primitive("chroma_reference", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 34.0, 60.0,
                  1.000, 70.0, 0.0, 0.0, 1.0, 60.660),
        Primitive("staircase", VitsPrimitiveType::kStaircase,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 40.0,
                  60.0, 0.235, 700.0, 0.0, 0.0, 0.0, 0.0, 5),
        Primitive("black_reference", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 60.0,
                  62.0, 0.235, 700.0),
    };
    definition.composites = {
        Composite("modulated_pulse", VitsCompositeMode::kParallel,
                  {"modulated_y", "modulated_c"}),
    };
    definition.render_order = {"white_reference", "pulse_2t",
                               "modulated_pulse", "chroma_reference",
                               "staircase",       "black_reference"};
    catalog.push_back(std::move(definition));
  }

  {
    VitsDefinition definition = Definition(
        Standard::kPal, "vits20", 20, VitsLevelsUnit::kMillivolts, 0.20, 0.40);
    definition.primitives = {
        Primitive("grey_pedestal", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 12.0,
                  32.0, 0.235, 350.0),
        Primitive("chroma_reference_full", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 14.0, 28.0,
                  1.000, 350.0, 0.0, 0.0, 1.0, 60.660),
        Primitive("chroma_reference_low", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 34.0, 62.0,
                  1.000, 150.0, 0.0, 0.0, 1.0, 60.660),
    };
    definition.render_order = {"grey_pedestal", "chroma_reference_full",
                               "chroma_reference_low"};
    catalog.push_back(std::move(definition));
  }

  {
    VitsDefinition definition =
        Definition(Standard::kPal, "itu-composite", 330,
                   VitsLevelsUnit::kMillivolts, 0.20, 0.40);
    definition.primitives = {
        Primitive("white_reference", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 12.0,
                  22.0, 0.200, 700.0),
        Primitive("pulse_2t", VitsPrimitiveType::kSinSquaredPulse,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 25.8,
                  26.2, 0.200, 700.0),
        Primitive("chroma_reference", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 30.0, 60.0,
                  1.000, 140.0, 0.0, 0.0, 1.0, 60.660),
        Primitive("staircase_step_1", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 40.0,
                  44.0, 0.235, 140.0),
        Primitive("staircase_step_2", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 44.0,
                  48.0, 0.235, 280.0),
        Primitive("staircase_step_3", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 48.0,
                  52.0, 0.235, 420.0),
        Primitive("staircase_step_4", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 52.0,
                  56.0, 0.235, 560.0),
        Primitive("staircase_step_5", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 56.0,
                  62.0, 0.235, 700.0),
    };
    definition.render_order = {"white_reference",  "pulse_2t",
                               "chroma_reference", "staircase_step_1",
                               "staircase_step_2", "staircase_step_3",
                               "staircase_step_4", "staircase_step_5"};
    catalog.push_back(std::move(definition));
  }

  {
    VitsDefinition definition =
        Definition(Standard::kPal, "itu-combination", 331,
                   VitsLevelsUnit::kMillivolts, 0.20, 0.40);
    definition.primitives = {
        Primitive("grey_pedestal", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 12.0,
                  62.0, 0.235, 350.0),
        Primitive("chroma_step_1", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 14.0, 18.0,
                  1.000, 70.0, 0.0, 0.0, 1.0, 60.660, 0, "chroma_staircase",
                  VitsTransitionOutPolicy::kCrossfade, 1.0),
        Primitive("chroma_step_2", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 18.0, 22.0,
                  1.000, 210.0, 0.0, 0.0, 1.0, 60.660, 0, "chroma_staircase",
                  VitsTransitionOutPolicy::kCrossfade, 1.0),
        Primitive("chroma_step_3", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 22.0, 28.0,
                  1.000, 350.0, 0.0, 0.0, 1.0, 60.660),
        Primitive("sustained_reference", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 34.0, 60.0,
                  1.000, 210.0, 0.0, 0.0, 1.0, 60.660),
    };
    definition.composites = {
        Composite("chroma_staircase_group", VitsCompositeMode::kSerial,
                  {"chroma_step_1", "chroma_step_2", "chroma_step_3"},
                  "chroma_staircase", "grey_pedestal"),
    };
    definition.render_order = {"grey_pedestal", "chroma_staircase_group",
                               "sustained_reference"};
    catalog.push_back(std::move(definition));
  }

  {
    VitsDefinition definition =
        Definition(Standard::kNtsc, "ntc7-composite", 17, VitsLevelsUnit::kIre,
                   0.20, 0.40);
    definition.primitives = {
        Primitive("white_reference", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 12.000,
                  30.000, 0.250, 100.0),
        Primitive("pulse_2t", VitsPrimitiveType::kSinSquaredPulse,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 33.750,
                  34.250, 0.200, 100.0),
        Primitive("modulated_y", VitsPrimitiveType::kSinSquaredPulse,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 35.400,
                  38.600, 0.200, 50.0),
        Primitive("modulated_c", VitsPrimitiveType::kCompositePulse,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 35.400,
                  38.600, 0.400, 50.0, 0.0, 0.0, 1.0, 0.0),
        Primitive("chrominance_reference", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 42.000,
                  60.000, 0.250, 20.0, 0.0, 0.0, 1.0, 180.0),
        Primitive("staircase", VitsPrimitiveType::kStaircase,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 46.000,
                  60.000, 0.250, 90.0, 0.0, 0.0, 0.0, 0.0, 5,
                  "ntc7_staircase_join"),
        Primitive("staircase_terminus", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 60.000,
                  62.000, 0.250, 90.0, 0.0, 0.0, 0.0, 0.0, 0,
                  "ntc7_staircase_join"),
    };
    definition.composites = {
        Composite("modulated_pulse", VitsCompositeMode::kParallel,
                  {"modulated_y", "modulated_c"}),
    };
    definition.render_order = {"white_reference", "pulse_2t",
                               "modulated_pulse", "chrominance_reference",
                               "staircase",       "staircase_terminus"};
    catalog.push_back(std::move(definition));
  }

  {
    VitsDefinition definition =
        Definition(Standard::kNtsc, "ntc7-combination", 280,
                   VitsLevelsUnit::kIre, 0.20, 0.40);
    definition.primitives = {
        Primitive("grey_reference_boost", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kAdd, 12.000,
                  16.000, 0.250, 50.0),
        Primitive("grey_background", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 12.000,
                  62.000, 0.250, 50.0),
        Primitive("mb_0_5mhz", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 18.000,
                  23.000, 0.200, 25.0, 0.0, 0.500),
        Primitive("mb_1_0mhz", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 24.000,
                  27.000, 0.200, 25.0, 0.0, 1.000),
        Primitive("mb_2_0mhz", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 28.000,
                  31.000, 0.200, 25.0, 0.0, 2.000),
        Primitive("mb_3_0mhz", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 32.000,
                  35.000, 0.200, 25.0, 0.0, 3.000),
        Primitive("mb_3_58mhz", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 36.000,
                  39.000, 0.200, 25.0, 0.0, 0.0, 1.0),
        Primitive("mb_4_2mhz", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 40.000,
                  43.000, 0.200, 25.0, 0.0, 4.200),
        Primitive("chroma_zone_1", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 46.000,
                  50.000, 1.000, 10.0, 0.0, 0.0, 1.0, 90.0, 0, "chroma_stair",
                  VitsTransitionOutPolicy::kCrossfade, 1.0),
        Primitive("chroma_zone_2", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 50.000,
                  54.000, 1.000, 20.0, 0.0, 0.0, 1.0, 90.0, 0, "chroma_stair",
                  VitsTransitionOutPolicy::kCrossfade, 1.0),
        Primitive("chroma_zone_3", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 54.000,
                  60.000, 1.000, 40.0, 0.0, 0.0, 1.0, 90.0, 0, "chroma_stair",
                  VitsTransitionOutPolicy::kNone, 0.0),
    };
    definition.composites = {
        Composite("multiburst_sweep", VitsCompositeMode::kSerial,
                  {"mb_0_5mhz", "mb_1_0mhz", "mb_2_0mhz", "mb_3_0mhz",
                   "mb_3_58mhz", "mb_4_2mhz"}),
        Composite("chroma_staircase", VitsCompositeMode::kSerial,
                  {"chroma_zone_1", "chroma_zone_2", "chroma_zone_3"},
                  "chroma_stair", "grey_background"),
    };
    definition.render_order = {"grey_background", "grey_reference_boost",
                               "multiburst_sweep", "chroma_staircase"};
    catalog.push_back(std::move(definition));
  }

  {
    VitsDefinition definition =
        Definition(Standard::kNtsc, "fcc-multiburst", 18, VitsLevelsUnit::kIre,
                   0.20, 0.40);
    definition.primitives = {
        Primitive("white_reference_boost", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kAdd, 9.200, 15.700,
                  0.250, 60.0),
        Primitive("grey_pedestal", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 9.200,
                  62.000, 0.250, 40.0),
        Primitive("burst_0_5", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 18.200,
                  26.700, 0.200, 30.0, 0.0, 0.500),
        Primitive("burst_1_25", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 28.200,
                  34.200, 0.200, 30.0, 0.0, 1.250),
        Primitive("burst_2_0", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 35.200,
                  40.200, 0.200, 30.0, 0.0, 2.000),
        Primitive("burst_3_0", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 41.200,
                  46.200, 0.200, 30.0, 0.0, 3.000),
        Primitive("burst_3_58", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 47.200,
                  52.200, 0.200, 30.0, 0.0, 0.0, 1.0),
        Primitive("burst_4_1", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 53.200,
                  58.200, 0.200, 30.0, 0.0, 4.100),
    };
    definition.composites = {
        Composite("burst_train", VitsCompositeMode::kSerial,
                  {"burst_0_5", "burst_1_25", "burst_2_0", "burst_3_0",
                   "burst_3_58", "burst_4_1"}),
    };
    definition.render_order = {"grey_pedestal", "white_reference_boost",
                               "burst_train"};
    catalog.push_back(std::move(definition));
  }

  {
    VitsDefinition definition =
        Definition(Standard::kNtsc, "fcc-composite", 281, VitsLevelsUnit::kIre,
                   0.20, 0.40);
    definition.primitives = {
        Primitive("chroma_reference_zone", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 9.500, 28.000,
                  0.400, 20.0, 0.0, 0.0, 1.0, 180.0),
        Primitive("staircase", VitsPrimitiveType::kStaircase,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 13.000,
                  28.000, 0.250, 80.0, 0.0, 0.0, 0.0, 0.0, 5,
                  "fcc_staircase_join"),
        Primitive("staircase_terminus", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 28.000,
                  30.000, 0.250, 80.0, 0.0, 0.0, 0.0, 0.0, 0,
                  "fcc_staircase_join"),
        Primitive("pulse_2t", VitsPrimitiveType::kSinSquaredPulse,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 35.250,
                  35.750, 0.200, 100.0),
        Primitive("modulated_y", VitsPrimitiveType::kSinSquaredPulse,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 37.900,
                  41.100, 0.200, 50.0),
        Primitive("modulated_c", VitsPrimitiveType::kCompositePulse,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 37.900,
                  41.100, 0.400, 50.0, 0.0, 0.0, 1.0, 180.0),
        Primitive("white_reference", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 43.900,
                  62.000, 0.200, 100.0),
    };
    definition.composites = {
        Composite("modulated_pulse", VitsCompositeMode::kParallel,
                  {"modulated_y", "modulated_c"}),
    };
    definition.render_order = {"chroma_reference_zone", "staircase",
                               "staircase_terminus",    "pulse_2t",
                               "modulated_pulse",       "white_reference"};
    catalog.push_back(std::move(definition));
  }

  {
    VitsDefinition definition = Definition(Standard::kNtsc, "virs", 0,
                                           VitsLevelsUnit::kIre, 0.20, 0.40);
    definition.primitives = {
        Primitive("virs_first_zone", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 9.150,
                  35.500, 0.250, 68.0, 0.0, 0.0, 0.0, 0.0, 0, "virs_sequence"),
        Primitive("virs_chroma_ref", VitsPrimitiveType::kBurst,
                  VitsSignalComponent::kC, VitsCombineMode::kAdd, 10.100,
                  34.500, 1.000, 22.0, 0.0, 0.0, 1.0, 180.0, 0),
        Primitive("virs_second_zone", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 35.500,
                  48.700, 0.250, 46.0, 0.0, 0.0, 0.0, 0.0, 0, "virs_sequence"),
        Primitive("virs_post_blank", VitsPrimitiveType::kColourBar,
                  VitsSignalComponent::kY, VitsCombineMode::kReplace, 48.700,
                  62.000, 0.250, 0.0, 0.0, 0.0, 0.0, 0.0, 0, "virs_sequence"),
    };
    definition.composites = {
        Composite("virs_zone_1", VitsCompositeMode::kParallel,
                  {"virs_first_zone", "virs_chroma_ref"}),
    };
    definition.render_order = {"virs_zone_1", "virs_second_zone",
                               "virs_post_blank"};
    catalog.push_back(std::move(definition));
  }

  return catalog;
}

const std::vector<VitsDefinition>& Catalog() {
  static const std::vector<VitsDefinition> kCatalog = BuildCatalog();
  return kCatalog;
}

}  // namespace

bool VitsDefinitionProvider::TryGetDefinition(Standard standard,
                                              const std::string& vits_type,
                                              VitsDefinition* out_definition,
                                              std::string* error) const {
  for (const VitsDefinition& definition : Catalog()) {
    if (definition.standard == standard && definition.vits_type == vits_type) {
      if (out_definition != nullptr) {
        *out_definition = definition;
      }
      if (error != nullptr) {
        error->clear();
      }
      return true;
    }
  }

  // PAL-M uses System M line structure, so fall back to M/NTSC VITS
  // definitions when no PAL-M-specific entry exists in the catalog.
  if (standard == Standard::kPalM) {
    for (const VitsDefinition& definition : Catalog()) {
      if (definition.standard == Standard::kNtsc &&
          definition.vits_type == vits_type) {
        if (out_definition != nullptr) {
          *out_definition = definition;
          out_definition->standard = Standard::kPalM;
        }
        if (error != nullptr) {
          error->clear();
        }
        return true;
      }
    }
  }

  // SECAM shares the 625-line structure and has no VITS catalogue of its own,
  // so the PAL definitions apply unchanged. The definition keeps the PAL
  // standard, so chroma packets stay PAL-modulated; the project validator
  // warns about this.
  if (standard == Standard::kSecam) {
    for (const VitsDefinition& definition : Catalog()) {
      if (definition.standard == Standard::kPal &&
          definition.vits_type == vits_type) {
        if (out_definition != nullptr) {
          *out_definition = definition;
        }
        if (error != nullptr) {
          error->clear();
        }
        return true;
      }
    }
  }

  if (error != nullptr) {
    *error = "Unsupported vits_type '" + vits_type + "' for standard '" +
             StandardToString(standard) + "'.";
  }
  return false;
}

std::vector<std::string> VitsDefinitionProvider::ListVitsTypes(
    Standard standard) const {
  // PAL-M uses System M line structure, so the M/NTSC catalogue applies, and
  // SECAM uses the 625-line PAL catalogue (mirrors the TryGetDefinition
  // fallbacks).
  Standard lookup = standard;
  if (standard == Standard::kPalM) {
    lookup = Standard::kNtsc;
  } else if (standard == Standard::kSecam) {
    lookup = Standard::kPal;
  }

  std::vector<std::string> vits_types;
  for (const VitsDefinition& definition : Catalog()) {
    if (definition.standard == lookup) {
      vits_types.push_back(definition.vits_type);
    }
  }
  return vits_types;
}

}  // namespace videosynth