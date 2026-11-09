/*
 * File:        test_child_process.cpp
 * Module:      child_process_tests
 * Purpose:     Functional tests for ChildProcessReader: pipes that no other
 *              child inherits, and teardown that never waits on a child
 *              blocked writing (issue #26). Spawns real processes.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <future>
#include <string>

#include "videosynth/child_process.h"

namespace videosynth {
namespace {

// Generous bound for teardown that should take milliseconds; a regression
// hangs forever, so any finite bound tells the two apart.
constexpr std::chrono::seconds kPromptBound{10};

// A command that writes without end, so it only stops when made to.
constexpr const char* kEndlessWriter = "yes videosynth";

void ReadSome(FILE* stream) {
  std::array<char, 256> buffer{};
  ASSERT_EQ(std::fread(buffer.data(), 1, buffer.size(), stream), buffer.size());
}

TEST(ChildProcessTest, WaitReturnsTheChildsExitStatus) {
  ChildProcessReader child;
  std::string error;
  ASSERT_TRUE(child.Start("exit 3", &error)) << error;
  const int status = child.Wait();
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 3);
  EXPECT_FALSE(child.running());
}

TEST(ChildProcessTest, RunAndCaptureOutputReturnsTheWholeOutput) {
  std::string output;
  std::string error;
  EXPECT_EQ(RunAndCaptureOutput("printf 'one\\ntwo'", &output, &error), 0)
      << error;
  EXPECT_EQ(output, "one\ntwo");
}

TEST(ChildProcessTest, TerminateStopsAChildWhoseReadEndIsHeldElsewhere) {
  ChildProcessReader child;
  std::string error;
  ASSERT_TRUE(child.Start(kEndlessWriter, &error)) << error;
  ReadSome(child.stream());

  // A second copy of the read end, standing in for one leaked into another
  // process: closing the stream no longer makes the child's writes fail, so
  // only the signals can stop it.
  const int held_read_end = dup(fileno(child.stream()));
  ASSERT_GE(held_read_end, 0);

  const auto start = std::chrono::steady_clock::now();
  child.Terminate();
  EXPECT_LT(std::chrono::steady_clock::now() - start, kPromptBound);
  EXPECT_FALSE(child.running());
  close(held_read_end);
}

TEST(ChildProcessTest, ALaterChildDoesNotInheritAnEarlierChildsPipe) {
  ChildProcessReader writer;
  std::string error;
  ASSERT_TRUE(writer.Start(kEndlessWriter, &error)) << error;
  ReadSome(writer.stream());

  // Were the writer's read end inherited, this child would keep it open and
  // the writer, its stream closed, would block on a full pipe for good.
  ChildProcessReader bystander;
  ASSERT_TRUE(bystander.Start("sleep 30", &error)) << error;

  // Wait() relies on EPIPE alone: it neither signals nor times out.
  const pid_t writer_pid = writer.pid();
  auto waited = std::async(std::launch::async, [&writer] { writer.Wait(); });
  const bool finished =
      waited.wait_for(kPromptBound) == std::future_status::ready;
  if (!finished) {
    // Ends a failing run instead of hanging it: the writer is still
    // unreaped, so its group id is still its own.
    kill(-writer_pid, SIGKILL);
  }
  waited.wait();
  EXPECT_TRUE(finished) << "the writer's pipe leaked into a later child";
}

TEST(ChildProcessTest, RunAndCaptureOutputReportsAFailingCommandsStatus) {
  std::string output;
  std::string error;
  const int status = RunAndCaptureOutput(
      "videosynth-no-such-command 2>/dev/null", &output, &error);
  ASSERT_GT(status, 0);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 127);
}

TEST(ChildProcessTest, DestroyingARunningChildReturnsPromptly) {
  const auto start = std::chrono::steady_clock::now();
  {
    ChildProcessReader child;
    std::string error;
    ASSERT_TRUE(child.Start(kEndlessWriter, &error)) << error;
    ReadSome(child.stream());
  }
  EXPECT_LT(std::chrono::steady_clock::now() - start, kPromptBound);
}

}  // namespace
}  // namespace videosynth
