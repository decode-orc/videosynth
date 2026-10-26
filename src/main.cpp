/*
 * File:        main.cpp
 * Module:      main
 * Purpose:     Implements CLI entrypoint and runtime option parsing.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

#include "videosynth/audio_track_generator.h"
#include "videosynth/dropout_injection_stage.h"
#include "videosynth/generation_stage.h"
#include "videosynth/logger.h"
#include "videosynth/noise_injection_stage.h"
#include "videosynth/output_stage.h"
#include "videosynth/path_resolution.h"
#include "videosynth/pipeline.h"
#include "videosynth/progressive_frame_source_probe.h"
#include "videosynth/project_validator.h"
#include "videosynth/yaml_project_parser.h"
#include "videosynth_version.h"

namespace {

bool IsValidLogLevel(const std::string& log_level) {
  return log_level == "info" || log_level == "debug" || log_level == "trace";
}

videosynth::LogLevel ParseLogLevel(const std::string& log_level) {
  if (log_level == "debug") {
    return videosynth::LogLevel::kDebug;
  }
  if (log_level == "trace") {
    return videosynth::LogLevel::kTrace;
  }
  return videosynth::LogLevel::kInfo;
}

void PrintUsage() {
  std::cout
      << "Usage:\n"
      << "  videosynth --project <path> [options]\n"
      << "  videosynth --project <path> --validate [options]\n\n"
      << "Options:\n"
      << "  --project   Path to YAML project file (required).\n"
      << "  The output path is read from project YAML under output.video_path; "
         "the metadata sidecar and audio are colocated with it.\n"
      << "  --validate  Validate only; do not generate output.\n"
      << "  --version   Print the build version (git commit hash) and exit.\n"
      << "  --threads <n>  Frame synthesis worker threads (default: auto).\n"
      << "              Use 1 for the pure sequential path; output is\n"
      << "              byte-identical regardless of the thread count.\n"
      << "  --template-cache-mb <n>  Frame template cache capacity in MiB\n"
      << "              (default: 512, 0 disables). Output is byte-identical\n"
      << "              regardless of the capacity.\n"
      << "  --log-level <level>  Set log level: info, debug, or trace.\n"
      << "  --log-file <filename>  Write logs to a file as well as stderr.\n"
      << "  --asset-root <name>=<path>  Map the {name}/… logical asset root "
         "to\n"
      << "              <path> (repeatable). Overrides the built-in bundled/"
         "user roots.\n"
      << "  --output-root <path>  Directory the {output}/… root resolves to.\n"
         "              Defaults to the project file's own directory.\n";
}

// Parses a --asset-root "name=path" argument into the root map. Returns false
// when the value has no '=' or an empty name.
bool ParseAssetRoot(const std::string& value, videosynth::AssetRootMap* roots) {
  const std::size_t eq = value.find('=');
  if (eq == std::string::npos || eq == 0) {
    return false;
  }
  roots->roots[value.substr(0, eq)] = value.substr(eq + 1);
  return true;
}

// Parses the --template-cache-mb argument: a non-negative integer number of
// mebibytes (0 disables the cache).
bool ParseTemplateCacheMb(const std::string& value,
                          std::size_t* out_capacity_bytes) {
  if (value.empty() ||
      value.find_first_not_of("0123456789") != std::string::npos) {
    return false;
  }
  *out_capacity_bytes =
      static_cast<std::size_t>(std::atoll(value.c_str())) * 1024ULL * 1024ULL;
  return true;
}

// Parses the --threads argument: "auto" or a positive integer.
// Returns true and stores the RunOptions convention (0 = auto) on success.
bool ParseThreadCount(const std::string& value, int* out_threads) {
  if (value == "auto") {
    *out_threads = 0;
    return true;
  }
  if (value.empty() ||
      value.find_first_not_of("0123456789") != std::string::npos) {
    return false;
  }
  const int parsed = std::atoi(value.c_str());
  if (parsed < 1) {
    return false;
  }
  *out_threads = parsed;
  return true;
}

int RunCli(int argc, char** argv) {
  videosynth::RunOptions options;
  // CLI default is auto (0); the RunOptions member itself defaults to the
  // sequential path for library callers.
  options.threads = 0;
  // Built-in bundled/user asset roots; --asset-root overrides individual ones.
  options.asset_roots = videosynth::DefaultAssetRoots();

  std::size_t template_cache_bytes =
      videosynth::GenerationStage::kDefaultTemplateCacheCapacityBytes;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];

    if (arg == "--version") {
      std::cout << VIDEOSYNTH_VERSION << "\n";
      return 0;
    }

    if (arg == "--project" && i + 1 < argc) {
      options.project_path = argv[++i];
    } else if (arg == "--validate") {
      options.validate_only = true;
    } else if (arg == "--threads" && i + 1 < argc) {
      if (!ParseThreadCount(argv[++i], &options.threads)) {
        PrintUsage();
        return 2;
      }
    } else if (arg == "--template-cache-mb" && i + 1 < argc) {
      if (!ParseTemplateCacheMb(argv[++i], &template_cache_bytes)) {
        PrintUsage();
        return 2;
      }
    } else if (arg == "--log-level" && i + 1 < argc) {
      options.log_level = argv[++i];
    } else if (arg == "--log-file" && i + 1 < argc) {
      options.log_file = argv[++i];
    } else if (arg == "--asset-root" && i + 1 < argc) {
      if (!ParseAssetRoot(argv[++i], &options.asset_roots)) {
        PrintUsage();
        return 2;
      }
    } else if (arg == "--output-root" && i + 1 < argc) {
      // Sugar for "--asset-root output=<path>"; the {output} root is the one a
      // batch run redirects most often, so it gets its own flag.
      options.asset_roots.roots["output"] = argv[++i];
    } else {
      PrintUsage();
      return 2;
    }
  }

  if (!IsValidLogLevel(options.log_level)) {
    PrintUsage();
    return 2;
  }

  if (options.project_path.empty()) {
    PrintUsage();
    return 2;
  }

  videosynth::SpdlogLogger logger(ParseLogLevel(options.log_level),
                                  options.log_file);
  logger.Debug("Logging configured at level '" + options.log_level + "'.");
  if (!options.log_file.empty()) {
    logger.Debug("Log file enabled: " + options.log_file);
  }

  videosynth::YamlProjectParser parser(&logger);
  videosynth::ProgressiveFrameSourceProbe progressive_frame_source_probe;
  videosynth::FfprobeAudioFileProbe audio_file_probe;
  videosynth::ProjectValidator validator(&progressive_frame_source_probe,
                                         &logger, &audio_file_probe);
  videosynth::GenerationStage generation(&logger);
  generation.SetTemplateCacheCapacityBytes(template_cache_bytes);
  videosynth::NoiseInjectionStage noise_injection(&logger);
  videosynth::DropoutInjectionStage dropout_injection(&logger);
  videosynth::OutputStage output(&logger);
  // Passed unconditionally; the pipeline only emits WAV tracks when the project
  // declares audio channel pairs.
  videosynth::AudioTrackGenerator audio_generator(&logger);

  videosynth::VideoSynthPipeline pipeline(&parser, &validator, &generation,
                                          &noise_injection, &dropout_injection,
                                          &output, &logger, &audio_generator);
  return pipeline.Run(options) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return RunCli(argc, argv);
  } catch (const std::exception& ex) {
    std::cerr << "Fatal error: " << ex.what() << "\n";
    return 1;
  }
}
