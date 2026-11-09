/*
 * File:        child_process.h
 * Module:      child_process
 * Purpose:     Runs a shell command as a child process whose standard output
 *              is read through a pipe that no other child can inherit, and
 *              tears it down without waiting for it to finish its work.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <sys/types.h>

#include <cstdio>
#include <string>

namespace videosynth {

// One child process started with `/bin/sh -c <command>`, its standard output
// on a pipe the parent reads through stream(). A replacement for popen that
// stays safe when several children are started and stopped from different
// threads:
//
// - Both pipe ends are close-on-exec, and they are created and the child
//   spawned under one process-wide lock, so no other child started through
//   this class can inherit them. popen guarantees that only for streams
//   opened earlier on the same thread; a child holding another decoder's read
//   end stops that decoder's ffmpeg ever seeing EPIPE (issue #26).
// - The child runs in its own process group, so Terminate() can stop the
//   shell and the program it runs without relying on EPIPE at all.
//
// Thread-safety: an instance is NOT thread-safe; distinct instances may be
// started, read and stopped concurrently from different threads.
class ChildProcessReader {
 public:
  ChildProcessReader() = default;
  // Terminates a child that is still running.
  ~ChildProcessReader();

  // Owns a child process; neither copying nor moving is meaningful.
  ChildProcessReader(const ChildProcessReader&) = delete;
  ChildProcessReader& operator=(const ChildProcessReader&) = delete;
  ChildProcessReader(ChildProcessReader&&) = delete;
  ChildProcessReader& operator=(ChildProcessReader&&) = delete;

  // Starts `command` under /bin/sh, terminating any child this instance
  // already holds. The child inherits the parent's standard input and error.
  // Returns false and sets error when the pipe or the process cannot be made.
  bool Start(const std::string& command, std::string* error);

  // The read end of the child's standard output; nullptr when no child runs.
  // Owned by this instance and valid until Wait() or Terminate().
  FILE* stream() const { return stream_; }

  // True between a successful Start() and the next Wait() or Terminate().
  bool running() const { return pid_ > 0; }

  // The child's process id, which also names its process group; -1 when no
  // child runs.
  pid_t pid() const { return pid_; }

  // Closes the stream and waits for the child to exit. For a child that has
  // finished writing, i.e. once stream() has reached end of file. Returns
  // the wait status as pclose does (0 for a clean exit), 0 when no child
  // runs, or -1 when the child cannot be reaped.
  int Wait();

  // Stops a child that may still be working: closes the stream, sends its
  // process group SIGTERM and, if it has not exited within a short grace
  // period, SIGKILL, then reaps it. Returns promptly however much output the
  // child has left. No-op when no child runs.
  void Terminate();

 private:
  // Closes stream_ if open.
  void CloseStream();

  FILE* stream_ = nullptr;
  pid_t pid_ = -1;
};

// Runs `command` to completion and returns its whole standard output in
// out_output. Returns the wait status (0 for a clean exit) or -1 when the
// command cannot be started, setting error in that case.
// Thread-safety: safe to call concurrently.
int RunAndCaptureOutput(const std::string& command, std::string* out_output,
                        std::string* error);

}  // namespace videosynth
