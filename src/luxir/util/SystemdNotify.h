// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <string_view>

// The systemd service-manager notification protocol (sd_notify), without
// libsystemd: one datagram per message to the AF_UNIX socket systemd names in
// NOTIFY_SOCKET.  Under Type=notify a unit becomes active on READY=1, and with
// WatchdogSec= it must send WATCHDOG=1 more often than WATCHDOG_USEC or it is
// aborted (SIGABRT by default, so a hang leaves a core) and restarted.
// Outside systemd every call is a no-op.
namespace luxir::systemd {

// Send `state` (newline-separated VAR=VALUE assignments, e.g. "READY=1").
// Returns true when a message was sent, false without NOTIFY_SOCKET or when
// the send failed.
bool notify(std::string_view state);

// The watchdog timeout the service manager asked this process for, or zero
// when there is none (WATCHDOG_USEC unset or invalid, or WATCHDOG_PID naming
// another process: the variables are inherited by children).
std::chrono::microseconds watchdogTimeout();

// GET /health on 127.0.0.1:port; true on a 200 response within `timeout`
// (connect, send and receive together).
bool probeHealth(int port, std::chrono::milliseconds timeout);

} // namespace luxir::systemd
