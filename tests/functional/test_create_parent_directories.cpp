/*
 * File:        test_create_parent_directories.cpp
 * Module:      path_resolution_tests
 * Purpose:     Validates output directory creation on the real filesystem,
 *              including bare file names written to the working directory.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include "videosynth/path_resolution.h"

namespace videosynth {
namespace {

TEST(CreateParentDirectoriesTest, CreatesMissingNestedDirectories) {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "videosynth_parent_dirs_test";
  std::filesystem::remove_all(root);
  const std::filesystem::path file = root / "a" / "b" / "out.cvbs";

  std::error_code ec;
  ASSERT_TRUE(CreateParentDirectories(file.string(), &ec)) << ec.message();
  EXPECT_TRUE(std::filesystem::is_directory(root / "a" / "b"));
  // Existing directories are not an error.
  EXPECT_TRUE(CreateParentDirectories(file.string(), &ec)) << ec.message();

  std::filesystem::remove_all(root);
}

TEST(CreateParentDirectoriesTest, ReportsAParentThatIsAFile) {
  const std::filesystem::path blocker =
      std::filesystem::temp_directory_path() / "videosynth_parent_dirs_blocker";
  std::filesystem::remove_all(blocker);
  {
    std::FILE* f = std::fopen(blocker.string().c_str(), "w");
    std::fclose(f);
  }

  std::error_code ec;
  EXPECT_FALSE(CreateParentDirectories((blocker / "out.cvbs").string(), &ec));
  EXPECT_TRUE(ec);

  std::filesystem::remove(blocker);
}

}  // namespace
}  // namespace videosynth
