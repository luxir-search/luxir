// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <csignal>
#include <filesystem>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <gtest/gtest.h>

extern char** environ;

namespace fs = std::filesystem;

namespace {

// Signals are process-wide, so this drives the real `luxir` binary built next
// to luxir_test rather than luxir_main in-process.
class ServerShutdownTest : public ::testing::Test {
protected:
  fs::path binary = fs::read_symlink("/proc/self/exe").parent_path() / "luxir";
  fs::path dataDir = fs::temp_directory_path() / ("luxir_shutdown_" + std::to_string(getpid()));

  void SetUp() override {
    if (!fs::exists(binary)) GTEST_SKIP() << "no luxir binary at " << binary;
    fs::remove_all(dataDir);
  }
  void TearDown() override { fs::remove_all(dataDir); }

  struct Server {
    pid_t pid = -1;
    int out = -1;
    std::string output;
  };

  Server start() {
    int fds[2];
    EXPECT_EQ(0, pipe(fds));
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    std::vector<std::string> args{binary.string(), "--no-http", "--server.grpc.port", "0",
        "--store.backend", "fs", "--store.data-dir", dataDir.string()};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    Server s;
    EXPECT_EQ(0, posix_spawn(&s.pid, argv[0], &actions, nullptr, argv.data(), environ));
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    s.out = fds[0];
    return s;
  }

  // Reads output until `text` appears or the stream ends / times out.
  static bool readUntil(Server& s, const std::string& text, int timeoutMs = 30000) {
    char buf[4096];
    while (s.output.find(text) == std::string::npos) {
      pollfd p{s.out, POLLIN, 0};
      if (poll(&p, 1, timeoutMs) <= 0) return false;
      ssize_t n = read(s.out, buf, sizeof(buf));
      if (n <= 0) return false;
      s.output.append(buf, (size_t)n);
    }
    return true;
  }

  // SIGTERMs a running server and returns its wait status.
  static int terminate(Server& s) {
    kill(s.pid, SIGTERM);
    readUntil(s, "\x04");  // drain output to EOF
    close(s.out);
    int status = 0;
    waitpid(s.pid, &status, 0);
    return status;
  }
};

TEST_F(ServerShutdownTest, sigtermShutsDownCleanly) {
  // Twice: the second start would fail if the first left the write lock held.
  for (int i = 0; i < 2; i++) {
    auto s = start();
    ASSERT_TRUE(readUntil(s, "GRPCServer listening")) << s.output;
    int status = terminate(s);
    ASSERT_TRUE(WIFEXITED(status)) << s.output;
    EXPECT_EQ(0, WEXITSTATUS(status)) << s.output;
    EXPECT_NE(std::string::npos, s.output.find("Shutdown complete")) << s.output;
  }
}

} // namespace
