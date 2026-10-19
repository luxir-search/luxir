// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "Stats.h"
#include "luxir/api/build.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef __linux__
#include <dirent.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

namespace luxir {
namespace {
using api::build::arenaStr;

// Each collector fills its own fields or is listed as unavailable; a missing
// source never hides the others.
struct Collectors {
  std::vector<std::string_view> available, unavailable;

  template<class F>
  bool collect(std::string_view name, F&& fn) {
    try {
      fn();
      available.push_back(name);
      return true;
    } catch (const std::exception&) {
      unavailable.push_back(name);
      return false;
    }
  }
  void finish(std::span<const std::string_view>& availableOut, std::span<const std::string_view>& unavailableOut,
              std::pmr::memory_resource& resource) {
    std::copy(available.begin(), available.end(), api::build::allocArray(availableOut, available.size(), resource));
    std::copy(unavailable.begin(), unavailable.end(),
              api::build::allocArray(unavailableOut, unavailable.size(), resource));
  }
};

#ifdef __linux__
std::ifstream input(const std::filesystem::path& path) {
  std::ifstream in(path);
  in.imbue(std::locale::classic());
  if (!in) throw std::runtime_error("cannot read system counter");
  return in;
}

// "key value" or "key: value [unit]" lines.
std::map<std::string, double> values(const std::filesystem::path& path) {
  auto in = input(path);
  std::map<std::string, double> result;
  std::string line, key;
  double value;
  while (std::getline(in, line)) {
    std::istringstream row(line);
    row.imbue(std::locale::classic());
    if (row >> key >> value) {
      if (key.ends_with(':')) key.pop_back();
      result[key] = value;
    }
  }
  return result;
}

double get(const std::map<std::string, double>& map, const char* key) {
  auto it = map.find(key);
  return it == map.end() ? 0 : it->second;
}

// /proc/stat, read once per request for both the host CPU and the process
// start time (which is relative to btime).
struct HostStat {
  std::vector<std::pair<std::string, std::array<uint64_t, 8>>> cpus;
  std::map<std::string, double> scalars;
};
HostStat hostStat() {
  auto in = input("/proc/stat");
  HostStat out;
  std::string line, key;
  while (std::getline(in, line)) {
    std::istringstream row(line);
    row >> key;
    if (key.starts_with("cpu") && key != "cpu") {
      auto& [name, ticks] = out.cpus.emplace_back(key.substr(3), std::array<uint64_t, 8>{});
      for (auto& tick : ticks) if (!(row >> tick)) throw std::runtime_error("invalid cpu stat");
    } else if (double value; row >> value) {
      out.scalars[key] = value;
    }
  }
  if (!out.scalars.contains("btime")) throw std::runtime_error("no btime");
  return out;
}

void process(api::ProcessStats& out, const HostStat& host) {
  auto status = values("/proc/self/status");
  out.resident_bytes = (uint64_t)get(status, "VmRSS") * 1024;
  out.virtual_bytes = (uint64_t)get(status, "VmSize") * 1024;
  out.swap_bytes = (uint64_t)get(status, "VmSwap") * 1024;
  out.resident_anonymous_bytes = (uint64_t)get(status, "RssAnon") * 1024;
  out.resident_file_bytes = (uint64_t)get(status, "RssFile") * 1024;
  out.resident_shared_bytes = (uint64_t)get(status, "RssShmem") * 1024;
  out.resident_peak_bytes = (uint64_t)get(status, "VmHWM") * 1024;
  out.threads = (uint64_t)status.at("Threads");
  // getrusage includes all threads, unlike context-switch fields in task status.
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage)) throw std::runtime_error("getrusage failed");
  out.cpu_seconds = usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 +
                    usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
  out.voluntary_context_switches = (uint64_t)usage.ru_nvcsw;
  out.involuntary_context_switches = (uint64_t)usage.ru_nivcsw;
  out.minor_page_faults = (uint64_t)usage.ru_minflt;
  out.major_page_faults = (uint64_t)usage.ru_majflt;
  rlimit limit{};
  if (getrlimit(RLIMIT_NOFILE, &limit)) throw std::runtime_error("getrlimit failed");
  out.max_fds = limit.rlim_cur == RLIM_INFINITY ? -1 : (int64_t)limit.rlim_cur;
  auto* directory = opendir("/proc/self/fd");
  if (!directory) throw std::runtime_error("cannot list descriptors");
  uint64_t count = 0;
  while (auto* entry = readdir(directory)) if (entry->d_name[0] != '.') ++count;
  closedir(directory);
  out.open_fds = count - 1; // excluding the descriptor of the listing itself

  auto stat = input("/proc/self/stat");
  std::string line;
  std::getline(stat, line);
  auto end = line.rfind(')'); // comm can contain spaces and parentheses.
  if (end == std::string::npos) throw std::runtime_error("invalid process stat");
  std::istringstream fields(line.substr(end + 2));
  std::string field;
  for (int i = 3; i <= 22; ++i) if (!(fields >> field)) throw std::runtime_error("short process stat");
  out.start_time_seconds = host.scalars.at("btime") + std::stod(field) / (double)sysconf(_SC_CLK_TCK);
}

void processIO(api::ProcessIoStats& out) {
  auto io = values("/proc/self/io");
  out.disk_read_bytes = (uint64_t)io.at("read_bytes");
  out.disk_written_bytes = (uint64_t)io.at("write_bytes");
  out.io_read_bytes = (uint64_t)io.at("rchar");
  out.io_written_bytes = (uint64_t)io.at("wchar");
  out.io_read_calls = (uint64_t)io.at("syscr");
  out.io_write_calls = (uint64_t)io.at("syscw");
}

void hostCPU(api::HostCpuStats& out, const HostStat& host, std::pmr::memory_resource& resource) {
  double tick = (double)sysconf(_SC_CLK_TCK);
  auto* cpus = api::build::allocArray(out.cpus, host.cpus.size(), resource);
  for (std::size_t i = 0; i < host.cpus.size(); i++) {
    const auto& [name, t] = host.cpus[i];
    cpus[i] = {arenaStr(resource, name), t[0] / tick, t[1] / tick, t[2] / tick, t[3] / tick,
               t[4] / tick, t[5] / tick, t[6] / tick, t[7] / tick};
  }
  out.context_switches = (uint64_t)get(host.scalars, "ctxt");
  out.forks = (uint64_t)get(host.scalars, "processes");
  out.procs_running = (uint64_t)get(host.scalars, "procs_running");
  out.procs_blocked = (uint64_t)get(host.scalars, "procs_blocked");
  out.boot_time_seconds = get(host.scalars, "btime");
  auto load = input("/proc/loadavg");
  if (!(load >> out.load1 >> out.load5 >> out.load15)) throw std::runtime_error("invalid loadavg");
  auto files = input("/proc/sys/fs/file-nr");
  uint64_t unused;
  if (!(files >> out.filefd_allocated >> unused >> out.filefd_maximum)) throw std::runtime_error("invalid file-nr");
}

void hostMemory(api::HostMemoryStats& out) {
  auto m = values("/proc/meminfo");
  auto kb = [&](const char* key) { return (uint64_t)get(m, key) * 1024; };
  out.total_bytes = kb("MemTotal");
  out.free_bytes = kb("MemFree");
  out.available_bytes = kb("MemAvailable");
  out.buffers_bytes = kb("Buffers");
  out.cached_bytes = kb("Cached");
  out.swap_cached_bytes = kb("SwapCached");
  out.swap_total_bytes = kb("SwapTotal");
  out.swap_free_bytes = kb("SwapFree");
  out.dirty_bytes = kb("Dirty");
  out.writeback_bytes = kb("Writeback");
  out.mapped_bytes = kb("Mapped");
  out.shmem_bytes = kb("Shmem");
  out.slab_bytes = kb("Slab");
  out.slab_reclaimable_bytes = kb("SReclaimable");
  out.slab_unreclaimable_bytes = kb("SUnreclaim");
  out.page_tables_bytes = kb("PageTables");
  out.kernel_stack_bytes = kb("KernelStack");
  out.anon_pages_bytes = kb("AnonPages");
  auto vm = values("/proc/vmstat");
  out.page_faults = (uint64_t)get(vm, "pgfault");
  out.major_page_faults = (uint64_t)get(vm, "pgmajfault");
  out.swap_in_pages = (uint64_t)get(vm, "pswpin");
  out.swap_out_pages = (uint64_t)get(vm, "pswpout");
  out.oom_kills = (uint64_t)get(vm, "oom_kill");
}

void network(std::span<const api::NetworkInterfaceStats>& out, std::pmr::memory_resource& resource) {
  auto in = input("/proc/net/dev");
  std::vector<std::pair<std::string, std::array<uint64_t, 16>>> rows;
  std::string line;
  while (std::getline(in, line)) {
    auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    auto device = line.substr(0, colon);
    device.erase(0, device.find_first_not_of(" \t"));
    std::istringstream row(line.substr(colon + 1));
    auto& [name, v] = rows.emplace_back(std::move(device), std::array<uint64_t, 16>{});
    for (auto& value : v) if (!(row >> value)) throw std::runtime_error("invalid network stat");
  }
  auto* dst = api::build::allocArray(out, rows.size(), resource);
  for (std::size_t i = 0; i < rows.size(); i++) {
    const auto& [name, v] = rows[i];
    dst[i] = {arenaStr(resource, name), v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7],
              v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15]};
  }
}

void tcp(api::TcpStats& out) {
  auto snmp = input("/proc/net/snmp");
  std::string header, line;
  while (std::getline(snmp, header) && std::getline(snmp, line)) {
    if (!header.starts_with("Tcp:")) continue;
    std::istringstream keys(header), vals(line);
    std::string key, group;
    keys >> group; vals >> group;
    double value;
    while (keys >> key && vals >> value) {
      if (key == "CurrEstab") out.curr_estab = (uint64_t)value;
      if (key == "RetransSegs") out.retrans_segs = (uint64_t)value;
      if (key == "InErrs") out.in_errs = (uint64_t)value;
      if (key == "OutRsts") out.out_rsts = (uint64_t)value;
      if (key == "AttemptFails") out.attempt_fails = (uint64_t)value;
    }
    return;
  }
  throw std::runtime_error("no tcp counters");
}

void disks(std::span<const api::DiskStats>& out, std::pmr::memory_resource& resource) {
  auto in = input("/proc/diskstats");
  std::vector<std::pair<std::string, std::array<uint64_t, 11>>> rows;
  std::string line, device;
  unsigned major, minor;
  while (std::getline(in, line)) {
    std::istringstream row(line);
    if (!(row >> major >> minor >> device)) continue;
    auto& [name, v] = rows.emplace_back(device, std::array<uint64_t, 11>{});
    for (auto& value : v) if (!(row >> value)) throw std::runtime_error("invalid disk stat");
  }
  auto* dst = api::build::allocArray(out, rows.size(), resource);
  for (std::size_t i = 0; i < rows.size(); i++) {
    const auto& [name, v] = rows[i];
    auto& d = dst[i];
    d.device = arenaStr(resource, name);
    d.reads_completed = v[0];
    d.reads_merged = v[1];
    d.read_bytes = v[2] * 512; // kernel sectors are always 512 bytes
    d.read_time_seconds = v[3] / 1000.0;
    d.writes_completed = v[4];
    d.writes_merged = v[5];
    d.written_bytes = v[6] * 512;
    d.write_time_seconds = v[7] / 1000.0;
    d.io_now = v[8];
    d.io_time_seconds = v[9] / 1000.0;
    d.io_time_weighted_seconds = v[10] / 1000.0;
  }
}

void filesystem(api::FilesystemStats& out, std::string_view directory, std::pmr::memory_resource& resource) {
  struct statvfs stat{};
  std::string path(directory);
  if (statvfs(path.c_str(), &stat) != 0) throw std::runtime_error("statvfs failed");
  out.path = arenaStr(resource, directory);
  out.size_bytes = (uint64_t)stat.f_blocks * stat.f_frsize;
  out.free_bytes = (uint64_t)stat.f_bfree * stat.f_frsize;
  out.avail_bytes = (uint64_t)stat.f_bavail * stat.f_frsize;
  out.files = (uint64_t)stat.f_files;
  out.files_free = (uint64_t)stat.f_ffree;
  out.readonly = (stat.f_flag & ST_RDONLY) != 0;
}

void pressure(std::span<const api::PressureStats>& out, std::pmr::memory_resource& resource) {
  constexpr std::array resources{"cpu", "memory", "io"};
  std::array<api::PressureStats, resources.size()> rows{};
  for (std::size_t i = 0; i < resources.size(); i++) {
    rows[i].resource = resources[i];
    auto in = input(std::string("/proc/pressure/") + resources[i]);
    std::string line, kind, field;
    while (std::getline(in, line)) {
      std::istringstream row(line);
      row >> kind;
      while (row >> field) if (field.starts_with("total=")) {
        double seconds = std::stod(field.substr(6)) / 1e6;
        if (kind == "some") rows[i].some_seconds = seconds;
        else if (kind == "full") rows[i].full_seconds = seconds;
      }
    }
  }
  auto* dst = api::build::allocArray(out, rows.size(), resource);
  std::copy(rows.begin(), rows.end(), dst);
}

// Match the current cgroup to its mount root, including cgroup namespaces and
// bind mounts. Never blindly report the host root as this process's cgroup.
std::filesystem::path cgroupDirectory() {
  auto membership = input("/proc/self/cgroup");
  std::string line, group;
  while (std::getline(membership, line)) if (line.starts_with("0::")) group = line.substr(3);
  if (group.empty()) throw std::runtime_error("cgroup v2 unavailable");
  auto mounts = input("/proc/self/mountinfo");
  while (std::getline(mounts, line)) {
    if (line.find(" - cgroup2 ") == std::string::npos) continue;
    std::istringstream row(line);
    std::string id, parent, device, root, mount;
    row >> id >> parent >> device >> root >> mount;
    // Mountinfo encodes whitespace and backslashes as octal escapes.
    auto unescape = [](std::string s) {
      for (size_t p = 0; p + 3 < s.size(); ++p) if (s[p] == '\\' &&
          s[p+1] >= '0' && s[p+1] <= '7' && s[p+2] >= '0' && s[p+2] <= '7' && s[p+3] >= '0' && s[p+3] <= '7') {
        char c = (char)((s[p+1]-'0')*64 + (s[p+2]-'0')*8 + s[p+3]-'0');
        s.replace(p, 4, 1, c);
      }
      return s;
    };
    root = unescape(root); mount = unescape(mount);
    if (root == "/" || group == root || group.starts_with(root + "/")) {
      std::string relative = root == "/" ? group.substr(1) : group.substr(root.size());
      while (relative.starts_with('/')) relative.erase(0, 1);
      auto path = (std::filesystem::path(mount) / relative).lexically_normal();
      auto rel = path.lexically_relative(mount);
      if (rel.empty() || *rel.begin() == "..") continue;
      return path;
    }
  }
  throw std::runtime_error("cgroup v2 mount unavailable");
}

// A cgroup limit file: a number, or "max" for unlimited (-1).
int64_t cgroupLimit(const std::filesystem::path& path) {
  auto in = input(path);
  std::string value;
  if (!(in >> value)) throw std::runtime_error("invalid cgroup scalar");
  return value == "max" ? -1 : std::stoll(value);
}

void cgroupMemory(api::CgroupMemoryStats& out, const std::filesystem::path& path) {
  out.current_bytes = (uint64_t)cgroupLimit(path / "memory.current");
  out.max_bytes = cgroupLimit(path / "memory.max");
  out.high_bytes = cgroupLimit(path / "memory.high");
  auto events = values(path / "memory.events");
  out.low_events = (uint64_t)get(events, "low");
  out.high_events = (uint64_t)get(events, "high");
  out.max_events = (uint64_t)get(events, "max");
  out.oom_events = (uint64_t)get(events, "oom");
  out.oom_kill_events = (uint64_t)get(events, "oom_kill");
}

void cgroupCPU(api::CgroupCpuStats& out, const std::filesystem::path& path) {
  auto stat = values(path / "cpu.stat");
  out.usage_seconds = get(stat, "usage_usec") / 1e6;
  out.throttled_seconds = get(stat, "throttled_usec") / 1e6;
  out.periods = (uint64_t)get(stat, "nr_periods");
  out.throttled_periods = (uint64_t)get(stat, "nr_throttled");
  auto cpu = input(path / "cpu.max");
  std::string quota;
  double period;
  if (!(cpu >> quota >> period) || period <= 0) throw std::runtime_error("invalid cpu.max");
  out.quota_cores = quota == "max" ? -1 : std::stod(quota) / period;
}

void cgroupTasks(api::CgroupTaskStats& out, const std::filesystem::path& path) {
  out.current = (uint64_t)cgroupLimit(path / "pids.current");
  out.max = cgroupLimit(path / "pids.max");
}
#endif
} // namespace

void gatherSystemStats(api::SystemStats* system, api::HostStats* host, std::string_view dataDirectory,
                       std::pmr::memory_resource& resource) {
#ifdef __linux__
  // /proc/stat serves both the process start time and the host CPU.
  std::optional<HostStat> stat;
  auto readStat = [&]() -> const HostStat& {
    if (!stat) stat = hostStat();
    return *stat;
  };
  if (system) {
    Collectors c;
    c.collect("process", [&] { api::ProcessStats process; luxir::process(process, readStat()); system->process = process; });
    c.collect("process_io", [&] { api::ProcessIoStats io; processIO(io); system->process_io = io; });
    // Only the data filesystem: statvfs on arbitrary remote mounts can hang.
    if (!dataDirectory.empty()) {
      c.collect("filesystem", [&] {
        api::FilesystemStats fs;
        filesystem(fs, dataDirectory, resource);
        system->filesystem = fs;
      });
    }
    std::filesystem::path cgroupPath;
    if (c.collect("cgroup", [&] { cgroupPath = cgroupDirectory(); })) {
      auto& cgroup = system->cgroup.emplace();
      cgroup.path = arenaStr(resource, cgroupPath.string());
      // Controllers are independent: a missing pids controller must not hide
      // otherwise available CPU and memory measurements.
      c.collect("cgroup_memory", [&] { api::CgroupMemoryStats m; cgroupMemory(m, cgroupPath); cgroup.memory = m; });
      c.collect("cgroup_cpu", [&] { api::CgroupCpuStats cpu; cgroupCPU(cpu, cgroupPath); cgroup.cpu = cpu; });
      c.collect("cgroup_tasks", [&] { api::CgroupTaskStats t; cgroupTasks(t, cgroupPath); cgroup.tasks = t; });
    }
    c.finish(system->available, system->unavailable, resource);
  }
  if (host) {
    Collectors c;
    c.collect("cpu", [&] { api::HostCpuStats cpu; hostCPU(cpu, readStat(), resource); host->cpu = cpu; });
    c.collect("memory", [&] { api::HostMemoryStats memory; hostMemory(memory); host->memory = memory; });
    c.collect("network", [&] { network(host->network, resource); });
    c.collect("tcp", [&] { api::TcpStats t; tcp(t); host->tcp = t; });
    c.collect("disk", [&] { disks(host->disks, resource); });
    c.collect("pressure", [&] { pressure(host->pressure, resource); });
    c.finish(host->available, host->unavailable, resource);
  }
#else
  (void)dataDirectory;
  if (system) {
    Collectors c{{}, {"system"}};
    c.finish(system->available, system->unavailable, resource);
  }
  if (host) {
    Collectors c{{}, {"host"}};
    c.finish(host->available, host->unavailable, resource);
  }
#endif
}
} // namespace luxir
