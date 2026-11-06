// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <string>
#include <vector>
#include <stdexcept>
#include <unistd.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include "luxir/util/luxir_util.h"

extern char** environ;
extern char** gArgv;

namespace luxir::test {

// Re-exec the current test: fork alone would inherit the suite's worker threads' locks.
class TestProcess {
  pid_t pid = -1;
  int socket = -1;
  static constexpr int childSocket = STDIN_FILENO;
public:
  static const char* role() { return std::getenv("LUXIR_TEST_PROCESS_ROLE"); }
  static std::filesystem::path path() { return std::getenv("LUXIR_TEST_PROCESS_PATH"); }
  static void send(int fd, char value) {
#if defined(MSG_NOSIGNAL)
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    ssize_t count;
    do { count = ::send(fd, &value, 1, flags); } while (count < 0 && errno == EINTR);
    if (count != 1) throw std::runtime_error("test process send failed");
  }
  static char receive(int fd) {
    pollfd poller{fd, POLLIN, 0};
    int ready;
    do { ready = ::poll(&poller, 1, 30000); } while (ready < 0 && errno == EINTR);
    char value;
    if (ready <= 0 || ::read(fd, &value, 1) != 1) throw std::runtime_error("test process did not reply");
    return value;
  }
  static void reply(char value) { send(childSocket, value); }
  static char command() { return receive(childSocket); }

  TestProcess(const std::filesystem::path& path, std::string role) {
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) throw std::runtime_error("test socketpair failed");
    auto close = scope_guard([&] { ::close(fds[0]); ::close(fds[1]); });
    for (int fd : fds) {
      if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) throw std::runtime_error("test fcntl failed");
#if defined(SO_NOSIGPIPE)
      int enabled = 1;
      if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0) throw std::runtime_error("test setsockopt failed");
#endif
    }
    auto* test = testing::UnitTest::GetInstance()->current_test_info();
    std::vector<std::string> args{std::filesystem::absolute(gArgv[0]).string(),
        "--gtest_filter=" + std::string(test->test_suite_name()) + "." + test->name(),
        "--gtest_brief=1", "--gtest_print_time=0"};
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    std::vector<std::string> environment;
    for (char** entry = environ; *entry; ++entry) {
      if (!std::string_view(*entry).starts_with("LUXIR_TEST_PROCESS_")) environment.emplace_back(*entry);
    }
    environment.push_back("LUXIR_TEST_PROCESS_ROLE=" + role);
    environment.push_back("LUXIR_TEST_PROCESS_PATH=" + path.string());
    std::vector<char*> env;
    for (auto& entry : environment) env.push_back(entry.data());
    env.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    if (::posix_spawn_file_actions_init(&actions)) throw std::runtime_error("test spawn actions failed");
    auto destroy = scope_guard([&] { ::posix_spawn_file_actions_destroy(&actions); });
    if (::posix_spawn_file_actions_adddup2(&actions, fds[1], childSocket)
        || ::posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), env.data())) {
      throw std::runtime_error("test process spawn failed");
    }
    socket = fds[0];
    fds[0] = -1;
  }
  ~TestProcess() {
    if (pid > 0) { ::kill(pid, SIGKILL); while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {} }
    if (socket >= 0) ::close(socket);
  }
  TestProcess(const TestProcess&) = delete;
  void send(char value) { send(socket, value); }
  char receive() { return receive(socket); }
  int join() {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    int status = 0;
    for (;;) {
      auto result = ::waitpid(pid, &status, WNOHANG);
      if (result == pid) { pid = -1; return WIFEXITED(status) ? WEXITSTATUS(status) : -1; }
      if ((result < 0 && errno != EINTR) || std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("test process did not exit");
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
};
}
