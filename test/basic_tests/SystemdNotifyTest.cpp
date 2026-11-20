// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "test/LuxirTest.h"
#include "luxir/server/HttpServer.h"
#include "luxir/util/SystemdNotify.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace luxir::test {
namespace {

// Restores an environment variable on scope exit.
class EnvVar {
  std::string name;
  std::optional<std::string> saved;

public:
  EnvVar(std::string name, const char* value) : name(std::move(name)) {
    if (const char* old = std::getenv(this->name.c_str())) saved = old;
    if (value) ::setenv(this->name.c_str(), value, 1);
    else ::unsetenv(this->name.c_str());
  }
  ~EnvVar() {
    if (saved) ::setenv(name.c_str(), saved->c_str(), 1);
    else ::unsetenv(name.c_str());
  }
};

// A datagram socket standing in for systemd's notify socket.
class NotifySocket {
  int fd;

public:
  std::string path;

  NotifySocket() {
    path = (std::filesystem::temp_directory_path() /
            ("luxir_notify_" + std::to_string(::getpid()))).string();
    ::unlink(path.c_str());
    fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    EXPECT_EQ(0, ::bind(fd, (const sockaddr*)&addr, sizeof(addr)));
    timeval tv{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }
  ~NotifySocket() {
    ::close(fd);
    ::unlink(path.c_str());
  }

  std::string receive() {
    char buf[256];
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    return n > 0 ? std::string(buf, (size_t)n) : std::string();
  }
};

class SystemdNotifyTest : public LuxirTest {};

TEST_F(SystemdNotifyTest, SendsStateToNotifySocket) {
  NotifySocket socket;
  {
    EnvVar env("NOTIFY_SOCKET", nullptr);
    EXPECT_FALSE(systemd::notify("READY=1"));
  }
  EnvVar env("NOTIFY_SOCKET", socket.path.c_str());
  ASSERT_TRUE(systemd::notify("READY=1"));
  EXPECT_EQ("READY=1", socket.receive());
  ASSERT_TRUE(systemd::notify("WATCHDOG=1"));
  EXPECT_EQ("WATCHDOG=1", socket.receive());
}

TEST_F(SystemdNotifyTest, WatchdogTimeoutIsForThisProcessOnly) {
  EnvVar usec("WATCHDOG_USEC", "60000000");
  {
    EnvVar pid("WATCHDOG_PID", nullptr);
    EXPECT_EQ(std::chrono::seconds(60), systemd::watchdogTimeout());
  }
  {
    EnvVar pid("WATCHDOG_PID", std::to_string(::getpid()).c_str());
    EXPECT_EQ(std::chrono::seconds(60), systemd::watchdogTimeout());
  }
  {
    EnvVar pid("WATCHDOG_PID", std::to_string(::getpid() + 1).c_str());
    EXPECT_EQ(0, systemd::watchdogTimeout().count());
  }
  EnvVar pid("WATCHDOG_PID", nullptr);
  {
    EnvVar bad("WATCHDOG_USEC", "60s");
    EXPECT_EQ(0, systemd::watchdogTimeout().count());
  }
  EnvVar none("WATCHDOG_USEC", nullptr);
  EXPECT_EQ(0, systemd::watchdogTimeout().count());
}

TEST_F(SystemdNotifyTest, ProbeHealthNeedsALiveServer) {
  int port;
  {
    LuxirNode node;
    HttpServer server(node, 1, 0);
    server.start();
    port = server.getPort();
    EXPECT_TRUE(systemd::probeHealth(port, std::chrono::seconds(5)));
    server.shutdown();
  }
  EXPECT_FALSE(systemd::probeHealth(port, std::chrono::seconds(1)));
}

} // namespace
} // namespace luxir::test
