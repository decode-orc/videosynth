/*
 * File:        test_yaml_project_emitter.cpp
 * Module:      yaml_project_emitter_tests
 * Purpose:     Verifies YAML emission round-trips and canonical form.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "videosynth/model.h"
#include "videosynth/project_validator.h"
#include "videosynth/results.h"
#include "videosynth/yaml_project_emitter.h"
#include "videosynth/yaml_project_parser.h"

namespace videosynth {
namespace {

// Parses `yaml`, emits the parsed project, re-parses the emission, and
// asserts the two parsed projects are equal and the emission is stable
// (emitting the re-parsed project reproduces the same text).
void ExpectRoundTrip(const std::string& yaml) {
  YamlProjectParser parser;
  YamlProjectEmitter emitter;

  const ParseResult first = parser.ParseString(yaml);
  ASSERT_TRUE(first.ok) << (first.errors.empty() ? "" : first.errors.front());

  const std::string emitted = emitter.EmitString(first.project);
  const ParseResult second = parser.ParseString(emitted);
  ASSERT_TRUE(second.ok) << "Re-parse failed for emitted YAML:\n"
                         << emitted << "\nError: "
                         << (second.errors.empty() ? ""
                                                   : second.errors.front());

  EXPECT_TRUE(first.project == second.project)
      << "Round-trip project mismatch. Emitted YAML:\n"
      << emitted;

  // Canonical stability: emit(parse(emit(p))) == emit(p).
  EXPECT_EQ(emitter.EmitString(second.project), emitted);
}

constexpr const char* kMinimalPalYaml = R"(
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbs
sections:
  - name: Bars
    type: progressive
    source: assets/bars.exr
    duration_frames: 10
)";

TEST(YamlProjectEmitterTest, MinimalProjectRoundTripsToEqualProject) {
  ExpectRoundTrip(kMinimalPalYaml);
}

TEST(YamlProjectEmitterTest, MinimalProjectOmitsUnsetOptionalBlocks) {
  YamlProjectParser parser;
  const ParseResult parsed = parser.ParseString(kMinimalPalYaml);
  ASSERT_TRUE(parsed.ok);

  const std::string emitted = YamlProjectEmitter().EmitString(parsed.project);
  EXPECT_EQ(emitted.find("noise"), std::string::npos);
  EXPECT_EQ(emitted.find("dropouts"), std::string::npos);
  EXPECT_EQ(emitted.find("osd"), std::string::npos);
  EXPECT_EQ(emitted.find("audio"), std::string::npos);
  EXPECT_EQ(emitted.find("line_injections"), std::string::npos);
  EXPECT_EQ(emitted.find("signal_type"), std::string::npos);
  EXPECT_EQ(emitted.find("start_frame"), std::string::npos);
  EXPECT_EQ(emitted.find("ntsc_black_setup_ire"), std::string::npos);
  EXPECT_EQ(emitted.find("secam_field_identification"), std::string::npos);
}

TEST(YamlProjectEmitterTest, LaserdiscVitsPlacementRoundTrips) {
  ExpectRoundTrip(R"(
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbs
line_injections:
  placement: laserdisc
  vits:
    - vits_type: uk-national
      target_lines: [19, 332]
    - vits_type: vits20
      target_lines: [20, 333]
sections:
  - name: Bars
    type: progressive
    source: assets/bars.exr
    duration_frames: 10
)");
}

TEST(YamlProjectEmitterTest, CustomVitsPlacementIsEmitted) {
  YamlProjectParser parser;
  const ParseResult parsed = parser.ParseString(R"(
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbs
line_injections:
  placement: custom
  vits:
    - vits_type: vits17
      target_lines: [21]
sections:
  - name: Bars
    type: progressive
    source: assets/bars.exr
    duration_frames: 10
)");
  ASSERT_TRUE(parsed.ok);

  const std::string emitted = YamlProjectEmitter().EmitString(parsed.project);
  EXPECT_NE(emitted.find("placement: custom"), std::string::npos);
}

TEST(YamlProjectEmitterTest, StandardVitsPlacementOmitsPlacementKey) {
  YamlProjectParser parser;
  const ParseResult parsed = parser.ParseString(R"(
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbs
line_injections:
  vits:
    - vits_type: vits17
      target_lines: [17]
sections:
  - name: Bars
    type: progressive
    source: assets/bars.exr
    duration_frames: 10
)");
  ASSERT_TRUE(parsed.ok);

  const std::string emitted = YamlProjectEmitter().EmitString(parsed.project);
  EXPECT_NE(emitted.find("line_injections"), std::string::npos);
  EXPECT_EQ(emitted.find("placement"), std::string::npos);
}

TEST(YamlProjectEmitterTest, AssetRootTokenSourceRoundTrips) {
  const std::string yaml = R"(
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbs
sections:
  - name: Bars
    type: progressive
    source: "{bundled}/exr/720x576/75_BARS.exr"
    duration_frames: 10
)";
  ExpectRoundTrip(yaml);

  YamlProjectParser parser;
  const ParseResult parsed = parser.ParseString(yaml);
  ASSERT_TRUE(parsed.ok);
  EXPECT_EQ(parsed.project.sections[0].source,
            "{bundled}/exr/720x576/75_BARS.exr");
}

TEST(YamlProjectEmitterTest, MinimalProjectKeepsCanonicalTopLevelOrder) {
  YamlProjectParser parser;
  const ParseResult parsed = parser.ParseString(kMinimalPalYaml);
  ASSERT_TRUE(parsed.ok);

  const std::string emitted = YamlProjectEmitter().EmitString(parsed.project);
  const std::size_t presets_pos = emitted.find("cvbs_presets:");
  const std::size_t output_pos = emitted.find("output:");
  const std::size_t sections_pos = emitted.find("sections:");
  ASSERT_NE(presets_pos, std::string::npos);
  ASSERT_NE(output_pos, std::string::npos);
  ASSERT_NE(sections_pos, std::string::npos);
  EXPECT_LT(presets_pos, output_pos);
  EXPECT_LT(output_pos, sections_pos);
}

TEST(YamlProjectEmitterTest, ProjectMetadataRoundTripsNameVersionDescription) {
  ExpectRoundTrip(R"(
project:
  name: "Example PAL CVBS Output"
  version: "1.0"
  description: "A test output with colour bars and line injections"
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbs
sections:
  - name: Bars
    type: progressive
    source: assets/bars.exr
    duration_frames: 10
)");
}

TEST(YamlProjectEmitterTest, CvbsPresetsRoundTripsNtscSetupAndBurstFlags) {
  ExpectRoundTrip(R"(
cvbs_presets:
  video_standard_preset: NTSC
  sample_encoding_preset: CVBS_U16_4FSC
  signal_state_preset: STANDARD_STABLE_LOCKED
  ntsc_black_setup_ire: 0.0
output:
  video_path: out/video.cvbs
sections:
  - name: Bars
    type: progressive
    source: assets/bars.exr
    duration_frames: 10
)");
}

TEST(YamlProjectEmitterTest, PalPilotBurstRoundTrips) {
  ExpectRoundTrip(R"(
cvbs_presets:
  video_standard_preset: PAL
  sample_encoding_preset: CVBS_S16_4FSC
  pal_laserdisc_pilot_burst: true
output:
  video_path: out/video.cvbs
sections:
  - name: Bars
    type: progressive
    source: assets/bars.exr
    duration_frames: 10
)");
}

TEST(YamlProjectEmitterTest, SecamPresetRoundTripsWithUnlockedSignalState) {
  ExpectRoundTrip(R"(
cvbs_presets:
  video_standard_preset: SECAM
  sample_encoding_preset: CVBS_U10_4FSC
  signal_state_preset: STANDARD_STABLE_UNLOCKED
output:
  video_path: out/video.cvbs
sections:
  - name: Bars
    type: progressive
    source: assets/bars.exr
    duration_frames: 10
)");
}

TEST(YamlProjectEmitterTest, SecamFieldIdentificationDisabledRoundTrips) {
  constexpr const char* kYaml = R"(
cvbs_presets:
  video_standard_preset: SECAM
  sample_encoding_preset: CVBS_U10_4FSC
  signal_state_preset: STANDARD_STABLE_UNLOCKED
  secam_field_identification: false
output:
  video_path: out/video.cvbs
sections:
  - name: Bars
    type: progressive
    source: assets/bars.exr
    duration_frames: 10
)";
  ExpectRoundTrip(kYaml);

  YamlProjectParser parser;
  const ParseResult parsed = parser.ParseString(kYaml);
  ASSERT_TRUE(parsed.ok);
  const std::string emitted = YamlProjectEmitter().EmitString(parsed.project);
  EXPECT_NE(emitted.find("video_standard_preset: SECAM"), std::string::npos);
  EXPECT_NE(emitted.find("secam_field_identification: false"),
            std::string::npos);
}

TEST(YamlProjectEmitterTest, YcOutputRoundTripsSignalType) {
  ExpectRoundTrip(R"(
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbsy
  signal_type: yc
sections:
  - name: Bars
    type: progressive
    source: assets/bars.exr
    duration_frames: 10
)");
}

TEST(YamlProjectEmitterTest, EfmAudioOutputRoundTripsSelectedPair) {
  ExpectRoundTrip(R"(
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbs
  efm_audio:
    pair: 2
sections:
  - name: Bars
    type: progressive
    source: assets/bars.exr
    duration_frames: 10
)");
}

TEST(YamlProjectEmitterTest, DisabledEfmAudioOmitsOutputKey) {
  YamlProjectParser parser;
  const ParseResult parsed = parser.ParseString(kMinimalPalYaml);
  ASSERT_TRUE(parsed.ok);

  const std::string emitted = YamlProjectEmitter().EmitString(parsed.project);
  EXPECT_EQ(emitted.find("efm_audio"), std::string::npos);
}

TEST(YamlProjectEmitterTest, DurationFramesAllRoundTrips) {
  ExpectRoundTrip(R"(
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbs
sections:
  - name: WholeSource
    type: progressive
    source: assets/clip.mkv
    duration_frames: all
    start_frame: 5
)");
}

TEST(YamlProjectEmitterTest, DurationRepeatRoundTripsWithAllSourceFrames) {
  ExpectRoundTrip(R"(
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbs
sections:
  - name: WholeSource
    type: progressive
    source: assets/clip.mkv
    duration_frames: all
    duration_repeat: 3
)");
}

TEST(YamlProjectEmitterTest, EndFrameRoundTrips) {
  ExpectRoundTrip(R"(
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbs
sections:
  - name: Excerpt
    type: progressive
    source: assets/capture.mp4
    duration_frames: all
    start_frame: 1500
    end_frame: 2999
)");
}

TEST(YamlProjectEmitterTest, OmittedDurationIsEmittedAsAll) {
  YamlProjectParser parser;
  YamlProjectEmitter emitter;
  const ParseResult parsed = parser.ParseString(R"(
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbs
sections:
  - name: Excerpt
    type: progressive
    source: assets/capture.mp4
    start_frame: 10
    end_frame: 19
)");
  ASSERT_TRUE(parsed.ok);
  const std::string emitted = emitter.EmitString(parsed.project);
  EXPECT_NE(emitted.find("duration_frames: all"), std::string::npos) << emitted;
  EXPECT_NE(emitted.find("end_frame: 19"), std::string::npos) << emitted;
  ExpectRoundTrip(emitted);
}

TEST(YamlProjectEmitterTest, UnsetEndFrameIsOmitted) {
  YamlProjectParser parser;
  YamlProjectEmitter emitter;
  const ParseResult parsed = parser.ParseString(kMinimalPalYaml);
  ASSERT_TRUE(parsed.ok);
  EXPECT_EQ(emitter.EmitString(parsed.project).find("end_frame"),
            std::string::npos);
}

TEST(YamlProjectEmitterTest, DurationRepeatDefaultOfOneIsOmitted) {
  YamlProjectParser parser;
  YamlProjectEmitter emitter;
  const ParseResult parsed = parser.ParseString(R"(
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbs
sections:
  - name: WholeSource
    type: progressive
    source: assets/clip.mkv
    duration_frames: all
    duration_repeat: 1
)");
  ASSERT_TRUE(parsed.ok);
  const std::string emitted = emitter.EmitString(parsed.project);
  EXPECT_EQ(emitted.find("duration_repeat"), std::string::npos)
      << "A repeat of 1 must not be serialised:\n"
      << emitted;
}

TEST(YamlProjectEmitterTest, VitsInjectionRoundTripsTargetLines) {
  ExpectRoundTrip(R"(
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbs
line_injections:
  vits:
    - vits_type: "pal_ccir330"
      target_lines: [19, 332]
sections:
  - name: Vits
    type: progressive
    source: assets/bars.exr
    duration_frames: 10
)");
}

TEST(YamlProjectEmitterTest, LaserdiscInjectionRoundTripsAllCodeFields) {
  ExpectRoundTrip(R"(
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbs
line_injections:
  disc_type: CAV
sections:
  - name: Chapter0
    type: progressive
    source: assets/chapter0.mkv
    duration_frames: 500
    section_type: programme_area
    line_injections:
      - type: laserdisc
        codes:
          - code_type: picture_number
            start_value: 1
          - code_type: chapter_number
            chapter: 5
          - code_type: picture_stop
          - code_type: programme_status
            programme_status: "0x8DC000"
          - code_type: users_code
            users_code: "0x80D000"
)");
}

TEST(YamlProjectEmitterTest, SectionTypesRoundTripLeadInAndLeadOut) {
  ExpectRoundTrip(R"(
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbs
sections:
  - name: LeadIn
    type: progressive
    source: assets/bars.exr
    duration_frames: 938
    section_type: lead_in
  - name: Programme
    type: progressive
    source: assets/bars.exr
    duration_frames: 500
    section_type: programme_area
  - name: LeadOut
    type: progressive
    source: assets/bars.exr
    duration_frames: 1250
    section_type: lead_out
)");
}

TEST(YamlProjectEmitterTest, NoiseBlockRoundTripsLevelsAndSeed) {
  ExpectRoundTrip(R"(
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbs
sections:
  - name: Noisy
    type: progressive
    source: assets/bars.exr
    duration_frames: 10
    noise:
      noise_db: 48.5
      noise_spread_db: 4.25
      noise_seed: 1234567890123
  - name: SeedOnly
    type: progressive
    source: assets/bars.exr
    duration_frames: 10
    noise:
      noise_seed: -7
)");
}

TEST(YamlProjectEmitterTest, NoiseSpectrumAndSharedLayerRoundTrip) {
  ExpectRoundTrip(R"(
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbs
sections:
  - name: BothLayers
    type: progressive
    source: assets/bars.exr
    duration_frames: 10
    noise:
      noise_db: 46.0
      noise_spread_db: 4.0
      noise_seed: 1001
      spectrum:
        - [0.0, 0.0]
        - [3.5, 4.0]
        - [6.0, 10.25]
      shared:
        noise_db: 44.0
        noise_seed: 77
        spectrum:
          - [1.5, -3.0]
          - [5.0, 6.0]
  - name: SharedOnly
    type: progressive
    source: assets/bars.exr
    duration_frames: 10
    noise:
      shared:
        noise_db: 50.0
        noise_seed: 5
)");
}

TEST(YamlProjectEmitterTest, ChannelBlockRoundTrip) {
  ExpectRoundTrip(R"(
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbs
channel:
  timing_offset_samples: -0.3
sections:
  - name: Bars
    type: progressive
    source: assets/bars.exr
    duration_frames: 10
)");
}

TEST(YamlProjectEmitterTest, ChannelEdgeSmearAndFmBeatRoundTrip) {
  ExpectRoundTrip(R"(
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbs
channel:
  timing_offset_samples: 0.25
  edge_smear:
    fraction: 0.12
    tau_samples: 3.5
  fm_beat:
    carrier_black_hz: 7079000
    level_db: -24.0
sections:
  - name: Bars
    type: progressive
    source: assets/bars.exr
    duration_frames: 10
)");
  // A block without a timing offset round-trips without one.
  ExpectRoundTrip(R"(
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbs
channel:
  fm_beat:
    level_db: -30.0
sections:
  - name: Bars
    type: progressive
    source: assets/bars.exr
    duration_frames: 10
)");
}

TEST(YamlProjectEmitterTest, AbsentChannelBlockIsNotEmitted) {
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.output.video_path = "out/video.cvbs";
  const std::string yaml = YamlProjectEmitter().EmitString(project);
  EXPECT_EQ(yaml.find("channel"), std::string::npos) << yaml;
}

TEST(YamlProjectEmitterTest, ChromaErrorBlockRoundTrip) {
  ExpectRoundTrip(R"(
cvbs_presets:
  video_standard_preset: NTSC
output:
  video_path: out/video.cvbs
chroma_error:
  gain: 1.05
  phase_deg: 2.0
  line_jitter:
    phase_deg: 0.5
    gain: 0.01
    seed: 5
sections:
  - name: Bars
    type: progressive
    source: assets/bars.exr
    duration_frames: 10
)");
}

TEST(YamlProjectEmitterTest, ChromaErrorIncludeBurstRoundTrip) {
  ExpectRoundTrip(R"(
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbs
chroma_error:
  gain: 0.9
  phase_deg: 0.0
  include_burst: true
sections:
  - name: Bars
    type: progressive
    source: assets/bars.exr
    duration_frames: 10
)");
}

TEST(YamlProjectEmitterTest, ChromaErrorWithUnseededJitterRoundTrip) {
  ExpectRoundTrip(R"(
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbs
chroma_error:
  phase_deg: -3.5
  line_jitter:
    gain: 0.02
sections:
  - name: Bars
    type: progressive
    source: assets/bars.exr
    duration_frames: 10
)");
}

TEST(YamlProjectEmitterTest, AbsentChromaErrorBlockIsNotEmitted) {
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.output.video_path = "out/video.cvbs";
  const std::string yaml = YamlProjectEmitter().EmitString(project);
  EXPECT_EQ(yaml.find("chroma_error"), std::string::npos) << yaml;
}

TEST(YamlProjectEmitterTest, SharedNoiseOnlyBlockEmitsNoCaptureKeys) {
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.output.video_path = "out/video.cvbs";
  Section section;
  section.name = "SharedOnly";
  section.type = "progressive";
  section.source = "assets/bars.exr";
  section.duration_frames = 10;
  section.noise.shared.enabled = true;
  section.noise.shared.noise_db = 50.0;
  section.noise.shared.noise_seed = 5;
  section.noise.shared.noise_seed_specified = true;
  project.sections.push_back(section);

  const std::string emitted = YamlProjectEmitter().EmitString(project);
  EXPECT_NE(emitted.find("shared:"), std::string::npos);
  EXPECT_EQ(emitted.find("noise_spread_db"), std::string::npos);
  EXPECT_EQ(emitted.find("spectrum"), std::string::npos);

  const ParseResult reparsed = YamlProjectParser().ParseString(emitted);
  ASSERT_TRUE(reparsed.ok);
  EXPECT_FALSE(reparsed.project.sections[0].noise.enabled);
  EXPECT_EQ(reparsed.project.sections[0].noise, section.noise);
}

TEST(YamlProjectEmitterTest, DropoutBlocksRoundTripScalesAndSeeds) {
  ExpectRoundTrip(R"(
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbs
sections:
  - name: Damaged
    type: progressive
    source: assets/bars.exr
    duration_frames: 50
    dropouts:
      random:
        scale: 5
        seed: 42
      scratch:
        scale: 3
  - name: ScratchOnly
    type: progressive
    source: assets/bars.exr
    duration_frames: 50
    dropouts:
      scratch:
        scale: 20
        seed: -99
)");
}

TEST(YamlProjectEmitterTest, LumaBandwidthRoundTripsWithAndWithoutTransition) {
  ExpectRoundTrip(R"(
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbs
sections:
  - name: Soft
    type: progressive
    source: assets/bars.exr
    duration_frames: 10
    luma_bandwidth:
      cutoff_mhz: 3.2
  - name: Softer
    type: progressive
    source: assets/bars.exr
    duration_frames: 10
    luma_bandwidth:
      cutoff_mhz: 2.5
      transition_mhz: 0.8
)");
}

TEST(YamlProjectEmitterTest, OsdOverlaysRoundTripTokensAndGeometry) {
  ExpectRoundTrip(R"(
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbs
sections:
  - name: Osd
    type: progressive
    source: assets/bars.exr
    duration_frames: 10
    osd:
      overlays:
        - text: "{picture_number} {biphase_hex}"
          x: 32
          y: 48
          scale: 2
          fg_luma: light_grey
          bg_luma: dark_grey
        - text: "plain"
)");
}

TEST(YamlProjectEmitterTest, AudioChannelPairsRoundTripToneRampAndSilence) {
  ExpectRoundTrip(R"(
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbs
sections:
  - name: StereoTone
    type: progressive
    source: assets/bars.exr
    duration_frames: 8
    audio:
      channel_pairs:
        - pair: 0
          description: Analogue stereo
          left:
            waveform: sine
            frequency: 997.5
            amplitude: 0.25
          right:
            waveform: sine
            frequency: 997.5
            amplitude: 0.25
  - name: MultiTrack
    type: progressive
    source: assets/bars.exr
    duration_frames: 8
    audio:
      channel_pairs:
        - pair: 0
          left:
            waveform: sawtooth
            ramp:
              start: 200.0
              end: 4000.0
              mode: bounce
              period: 0.5
        - pair: 3
          description: Commentary
          left:
            waveform: square
            frequency: 440.0
)");
}

TEST(YamlProjectEmitterTest, AudioChannelPairsRoundTripFileAndVideoAudio) {
  ExpectRoundTrip(R"(
cvbs_presets:
  video_standard_preset: PAL
output:
  video_path: out/video.cvbs
sections:
  - name: Programme
    type: progressive
    source: "{project}/capture.mp4"
    audio:
      channel_pairs:
        - pair: 0
          description: Programme audio
          video_audio:
            stream: 1
        - pair: 1
          description: Narration
          file:
            path: "{project}/narration.flac"
            repeat: true
        - pair: 2
          file:
            path: music.wav
)");
}

TEST(YamlProjectEmitterTest, EmittedProjectValidatesCleanly) {
  YamlProjectParser parser;
  const ParseResult parsed = parser.ParseString(R"(
project:
  name: ValidProject
cvbs_presets:
  video_standard_preset: PAL
  sample_encoding_preset: CVBS_U10_4FSC
  signal_state_preset: STANDARD_STABLE_LOCKED
output:
  video_path: out/video.cvbs
sections:
  - name: Bars
    type: progressive
    source: assets/bars.exr
    duration_frames: 10
    noise:
      noise_db: 48.0
)");
  ASSERT_TRUE(parsed.ok);

  const std::string emitted = YamlProjectEmitter().EmitString(parsed.project);
  const ParseResult reparsed = parser.ParseString(emitted);
  ASSERT_TRUE(reparsed.ok);

  ProjectValidator validator;
  const ValidationResult validation = validator.Validate(reparsed.project);
  EXPECT_TRUE(validation.is_valid)
      << (validation.errors.empty() ? "" : validation.errors.front());
}

}  // namespace
}  // namespace videosynth
