/*
 * File:        child_process.cpp
 * Module:      child_process
 * Purpose:     Runs a shell command as a child process whose standard output
 *              is read through a pipe that no other child can inherit, and
 *              tears it down without waiting for it to finish its work.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/child_process.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

#if defined(__APPLE__)
#include <crt_externs.h>
#else
// POSIX process environment. Declared at global scope: a block-scope extern
// inside the namespaces below would name videosynth::(anonymous)::environ,
// which is never defined.
extern char** environ;  // NOLINT(readability-redundant-declaration)
#endif

namespace videosynth {
namespace {

// How long Terminate() lets a child act on SIGTERM before sending SIGKILL.
constexpr std::chrono::milliseconds kTerminateGrace{1000};
// How often Terminate() checks whether the child has exited.
constexpr std::chrono::milliseconds kTerminatePollInterval{5};

// Serialises creating a pipe and spawning its child. Without pipe2() the
// close-on-exec flag is set in a second step, and a child spawned by another
// thread in between would inherit the pipe; holding this lock across both
// steps, and across the spawn itself, rules that out for every child started
// through ChildProcessReader.
std::mutex& SpawnMutex() {
  static std::mutex mutex;
  return mutex;
}

char** Environment() {
#if defined(__APPLE__)
  return *_NSGetEnviron();
#else
  return ::environ;
#endif
}

std::string ErrnoText(const char* what, int error_number) {
  return std::string(what) + ": " + std::strerror(error_number);
}

// Creates a pipe with both ends close-on-exec. Call under SpawnMutex().
bool MakeCloseOnExecPipe(std::array<int, 2>* fds, int* error_number) {
#if defined(__linux__)
  if (pipe2(fds->data(), O_CLOEXEC) != 0) {
    *error_number = errno;
    return false;
  }
  return true;
#else
  if (pipe(fds->data()) != 0) {
    *error_number = errno;
    return false;
  }
  for (const int fd : *fds) {
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
      *error_number = errno;
      close((*fds)[0]);
      close((*fds)[1]);
      return false;
    }
  }
  return true;
#endif
}

// Waits for pid, retrying on EINTR. Returns the wait status or -1.
int WaitForPid(pid_t pid) {
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return -1;
    }
  }
  return status;
}

// Spawns `/bin/sh -c command` in a new process group with standard output on
// write_fd. Call under SpawnMutex(). Returns 0 or an errno value.
int SpawnShell(const std::string& command, int write_fd, pid_t* out_pid) {
  posix_spawn_file_actions_t actions;
  int result = posix_spawn_file_actions_init(&actions);
  if (result != 0) {
    return result;
  }
  posix_spawnattr_t attributes;
  result = posix_spawnattr_init(&attributes);
  if (result != 0) {
    posix_spawn_file_actions_destroy(&actions);
    return result;
  }

  // dup2 onto standard output clears close-on-exec on the copy only.
  result = posix_spawn_file_actions_adddup2(&actions, write_fd, STDOUT_FILENO);
  int flags = POSIX_SPAWN_SETPGROUP;
#if defined(__APPLE__)
  // Closes every descriptor the file actions do not name, so the child cannot
  // inherit a descriptor some other code left without close-on-exec either.
  flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
  if (result == 0) {
    result = posix_spawn_file_actions_addinherit_np(&actions, STDIN_FILENO);
  }
  if (result == 0) {
    result = posix_spawn_file_actions_addinherit_np(&actions, STDERR_FILENO);
  }
#endif
  if (result == 0) {
    result =
        posix_spawnattr_setflags(&attributes, static_cast<std::int16_t>(flags));
  }
  if (result == 0) {
    // Process group 0: the child leads a new group named by its own pid.
    result = posix_spawnattr_setpgroup(&attributes, 0);
  }
  if (result == 0) {
    std::string shell = "/bin/sh";
    std::string dash_c = "-c";
    std::string command_copy = command;
    char* argv[] = {shell.data(), dash_c.data(), command_copy.data(), nullptr};
    result = posix_spawn(out_pid, shell.c_str(), &actions, &attributes, argv,
                         Environment());
  }

  posix_spawnattr_destroy(&attributes);
  posix_spawn_file_actions_destroy(&actions);
  return result;
}

}  // namespace

ChildProcessReader::~ChildProcessReader() { Terminate(); }

bool ChildProcessReader::Start(const std::string& command, std::string* error) {
  Terminate();

  std::array<int, 2> fds{-1, -1};
  pid_t pid = -1;
  {
    const std::lock_guard<std::mutex> lock(SpawnMutex());
    int error_number = 0;
    if (!MakeCloseOnExecPipe(&fds, &error_number)) {
      if (error != nullptr) {
        *error = ErrnoText("Unable to create a pipe", error_number);
      }
      return false;
    }
    const int spawn_result = SpawnShell(command, fds[1], &pid);
    close(fds[1]);
    if (spawn_result != 0) {
      close(fds[0]);
      if (error != nullptr) {
        *error = ErrnoText("Unable to start a child process", spawn_result);
      }
      return false;
    }
  }

  stream_ = fdopen(fds[0], "r");
  pid_ = pid;
  if (stream_ == nullptr) {
    const int error_number = errno;
    close(fds[0]);
    Terminate();
    if (error != nullptr) {
      *error = ErrnoText("Unable to read a child process", error_number);
    }
    return false;
  }
  return true;
}

void ChildProcessReader::CloseStream() {
  if (stream_ != nullptr) {
    std::fclose(stream_);
    stream_ = nullptr;
  }
}

int ChildProcessReader::Wait() {
  CloseStream();
  if (pid_ <= 0) {
    return 0;
  }
  const int status = WaitForPid(pid_);
  pid_ = -1;
  return status;
}

void ChildProcessReader::Terminate() {
  // With the read end closed, a child still writing normally fails with
  // EPIPE; the signals cover one that does not, whatever the reason.
  CloseStream();
  if (pid_ <= 0) {
    return;
  }
  const pid_t pid = pid_;
  pid_ = -1;

  // The child leads its own group, so this reaches the shell and the program
  // it runs alike.
  kill(-pid, SIGTERM);
  const auto deadline = std::chrono::steady_clock::now() + kTerminateGrace;
  while (std::chrono::steady_clock::now() < deadline) {
    int status = 0;
    const pid_t reaped = waitpid(pid, &status, WNOHANG);
    if (reaped == pid || (reaped < 0 && errno != EINTR)) {
      return;
    }
    std::this_thread::sleep_for(kTerminatePollInterval);
  }
  // Still unreaped, so the group id cannot yet have been reused.
  kill(-pid, SIGKILL);
  WaitForPid(pid);
}

int RunAndCaptureOutput(const std::string& command, std::string* out_output,
                        std::string* error) {
  ChildProcessReader child;
  if (!child.Start(command, error)) {
    return -1;
  }
  std::array<char, 4096> buffer{};
  std::string output;
  std::size_t read = 0;
  while ((read = std::fread(buffer.data(), 1, buffer.size(), child.stream())) >
         0U) {
    output.append(buffer.data(), read);
  }
  const int status = child.Wait();
  if (out_output != nullptr) {
    *out_output = std::move(output);
  }
  return status;
}

}  // namespace videosynth
