// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "Prometheus.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <stdexcept>

namespace luxir {
namespace {
std::string number(double value) {
  if (std::isinf(value)) return value > 0 ? "+Inf" : "-Inf";
  if (std::isnan(value)) return "NaN";
  char buffer[64];
  auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (result.ec != std::errc{}) throw std::runtime_error("metric conversion failed");
  return {buffer, result.ptr};
}
std::string line(std::string_view name, double value, std::string_view labels) {
  return std::string(name) + (labels.empty() ? "" : "{" + std::string(labels) + "}") + " " + number(value) + "\n";
}
std::string join(std::string_view labels, std::string_view more) {
  if (labels.empty()) return std::string(more);
  return std::string(labels) + "," + std::string(more);
}

class Renderer {
  PrometheusText& out;
  std::span<const double> bounds;
public:
  Renderer(PrometheusText& out, std::span<const double> bounds) : out(out), bounds(bounds) {}
  void gauge(std::string_view name, std::string_view help, double value, std::string_view labels = {}) {
    out.sample(name, help, "gauge", value, labels);
  }
  void counter(std::string_view name, std::string_view help, double value, std::string_view labels = {}) {
    out.sample(name, help, "counter", value, labels);
  }
  void activity(std::string_view prefix, const api::ActivityStats& a, std::string_view labels = {}) {
    std::string name(prefix);
    gauge(name + "_in_flight", "Operations currently in flight.", (double)a.in_flight, labels);
    constexpr std::string_view help = "Completed operations by outcome.";
    counter(name + "_total", help, (double)a.ok, join(labels, "outcome=\"ok\""));
    counter(name + "_total", help, (double)a.error, join(labels, "outcome=\"error\""));
    counter(name + "_total", help, (double)a.cancelled, join(labels, "outcome=\"cancelled\""));
    out.histogram(name + "_duration_seconds", "Operation duration in seconds, including failed operations.",
                  bounds, a.duration, labels);
  }
  void histogram(std::string_view name, std::string_view help, const api::HistogramStats& h) {
    out.histogram(name, help, bounds, h);
  }
};

void service(Renderer& r, const api::ServiceStats& s) {
  if (!s.version.empty()) r.gauge("luxir_build_info", "Luxir build version.", 1, PrometheusText::label("version", s.version));
  r.gauge("luxir_node_read_only", "Node is configured read only.", s.read_only);
  for (auto [transportName, transport] : {std::pair{"http", &s.http}, {"grpc", &s.grpc}}) {
    auto t = PrometheusText::label("transport", transportName);
    for (const auto& [operation, activity] : transport->operations)
      r.activity("luxir_requests", activity, t + "," + PrometheusText::label("operation", operation));
    for (const auto& [kind, count] : transport->errors)
      r.counter("luxir_request_errors_total", "First application error per HTTP request or gRPC call, including in-band errors.",
                (double)count, t + "," + PrometheusText::label("kind", kind));
    r.counter("luxir_received_body_bytes_total", "Received application payload bytes, excluding transport framing.", (double)transport->received_bytes, t);
    r.counter("luxir_response_body_bytes_total", "Application payload bytes submitted for sending, including failed deliveries.", (double)transport->sent_bytes, t);
    r.counter("luxir_stream_pauses_total", "Response producers paused by flow control.", (double)transport->stream_pauses, t);
  }
  r.gauge("luxir_http_connections", "Live HTTP sessions, including idle keep-alive connections.", (double)s.http.connections);
  r.gauge("luxir_http_io_threads", "Running HTTP connection shard threads (excludes the accept thread).", (double)s.http.io_threads);
  r.activity("luxir_search", s.search);
  r.histogram("luxir_search_execution_duration_seconds", "Search body execution, excluding scheduler waits and later streaming drains.", s.search_execution);
  r.counter("luxir_search_resource_exhausted_total", "Searches rejected by resource limits, including the request memory breaker.", (double)s.search_resource_exhausted);
  r.counter("luxir_search_warnings_total", "Searches completing with at least one declared degradation warning.", (double)s.search_warnings);
  r.activity("luxir_index_batches", s.index_batches);
  constexpr std::string_view docs = "Documents accepted into indexing or rejected/rolled back; acceptance is not durable acknowledgment.";
  r.counter("luxir_ingest_documents_total", docs, (double)s.documents_accepted, "outcome=\"accepted\"");
  r.counter("luxir_ingest_documents_total", docs, (double)s.documents_rejected, "outcome=\"rejected\"");
  r.counter("luxir_ingest_delete_ids_total", "Accepted delete IDs, not the number of matching documents removed.", (double)s.delete_ids_accepted);
  for (auto [kind, activity] : {std::pair{"flush", &s.flush}, {"commit", &s.commit}, {"merge", &s.merge},
                                {"vector_build", &s.vector_build}, {"reader_refresh", &s.reader_refresh}})
    r.activity("luxir_background", *activity, PrometheusText::label("kind", kind));
}

void memory(Renderer& r, const api::MemoryStats& m) {
  r.gauge("luxir_search_request_memory_limit_bytes", "Per-request tracked search memory ceiling; zero disables rejection.", (double)m.search_request_limit_bytes);
  r.gauge("luxir_node_memory_budget_bytes", "Configured budget for managed memory; not an RSS ceiling. Zero means unlimited.", (double)m.node_budget_bytes);
  const auto& ram = m.indexing_budget;
  r.gauge("luxir_index_ram_reserved_bytes", "Indexing RAM reservations, which may temporarily exceed the soft cap.", (double)ram.reserved_bytes);
  r.gauge("luxir_index_ram_budget_bytes", "Shared indexing RAM cap; zero means unlimited.", (double)ram.limit_bytes);
  r.gauge("luxir_index_ram_draining_bytes", "Reserved indexing RAM currently draining through flushes.", (double)ram.draining_bytes);
  r.gauge("luxir_index_ram_pending_merge_bytes", "Pending merge demand, capped at the indexing RAM budget when limited.", (double)ram.pending_merge_bytes);
  r.gauge("luxir_storage_ram_bytes", "RAM storage bytes owned by the node; not disk usage.", (double)m.storage_ram.used_bytes);
  r.gauge("luxir_storage_ram_limit_bytes", "RAM storage cap; zero means unlimited or not applicable.", (double)m.storage_ram.limit_bytes);
  const auto& f = m.field_metadata;
  for (auto [part, count, bytes] : {std::tuple{"directory", f.directories, f.directory_bytes}, {"page", f.pages, f.page_bytes},
                                    {"entry", f.entries, f.entry_bytes}, {"name", f.names, f.name_bytes}}) {
    auto labels = PrometheusText::label("part", part);
    r.gauge("luxir_field_metadata_objects", "Cached field metadata objects.", (double)count, labels);
    r.gauge("luxir_field_metadata_bytes", "Cached field metadata payload bytes, excluding allocator overhead.", (double)bytes, labels);
  }
  r.gauge("luxir_allocator_stats_available", "Whether jemalloc statistics are available.", m.jemalloc.has_value());
  if (!m.jemalloc) return;
  const auto& j = *m.jemalloc;
  for (auto [kind, value] : {std::pair{"allocated", j.allocated_bytes}, {"active", j.active_bytes},
      {"metadata", j.metadata_bytes}, {"resident", j.resident_bytes}, {"mapped", j.mapped_bytes},
      {"retained", j.retained_bytes}, {"dirty", j.dirty_bytes}})
    r.gauge("luxir_allocator_bytes", "Jemalloc accounting; resident is an allocator estimate, not process RSS.", (double)value, PrometheusText::label("kind", kind));
  for (auto [name, arena] : {std::pair{"indexing", &j.indexing}, {"big_buffer", &j.big_buffer}}) {
    r.gauge("luxir_allocator_arena_allocated_bytes", "Live arena allocations.", (double)arena->allocated_bytes, PrometheusText::label("arena", name));
    r.gauge("luxir_allocator_arena_resident_bytes", "Allocator estimate of resident arena memory.", (double)arena->resident_bytes, PrometheusText::label("arena", name));
  }
}

void cache(Renderer& r, const api::QueryCacheStats& c, const std::string& labels) {
  r.gauge("luxir_filter_cache_resident_bytes", "Resident filter-cache payload bytes.", (double)c.resident_bytes, labels);
  r.gauge("luxir_filter_cache_metadata_bytes", "Filter-cache metadata bytes.", (double)c.metadata_bytes, labels);
  r.gauge("luxir_filter_cache_budget_bytes", "Filter-cache payload budget; zero disables caching.", (double)c.max_bytes, labels);
  constexpr std::string_view lookups = "Shared filter-cache lookups; reset by cache-control reset_counters.";
  r.counter("luxir_filter_cache_lookups_total", lookups, (double)c.hits, labels + ",result=\"hit\"");
  r.counter("luxir_filter_cache_lookups_total", lookups, (double)c.misses, labels + ",result=\"miss\"");
  for (auto [name, value] : {std::pair{"builds", c.builds}, {"evictions", c.evictions}, {"purges", c.purges},
      {"admissions", c.admissions}, {"publish_rejects", c.publish_rejects}, {"oversized_key_bypasses", c.oversized_key_bypasses},
      {"build_attempts", c.build_attempts}, {"byproduct_inserts", c.byproduct_inserts},
      {"dead_builds", c.capacity_dead_builds}, {"thrash_skips", c.thrash_build_skips},
      {"reader_dead_builds", c.reader_dead_builds}, {"reader_thrash_skips", c.reader_thrash_build_skips},
      {"reader_stable_hits", c.reader_stable_hits}, {"reader_stable_refreshes", c.reader_stable_refreshes},
      {"reader_stable_retires", c.reader_stable_retires}})
    r.counter(std::string("luxir_filter_cache_") + name + "_total", "Filter-cache events; reset by cache-control reset_counters.", (double)value, labels);
}

void collections(Renderer& r, const api::StatsResponse& stats, bool files) {
  uint64_t available = 0, unavailable = 0;
  for (const auto& c : stats.collections) {
    auto labels = PrometheusText::label("tenant", c.tenant) + "," + PrometheusText::label("collection", c.name);
    bool ok = !c.error && !c.shards.empty();
    ++(ok ? available : unavailable);
    r.gauge("luxir_collection_available", "Collection is available for requests.", ok, labels);
    r.gauge("luxir_collection_storage_ram_bytes", "RAM storage bytes owned by the collection.", (double)c.storage_ram_bytes, labels);
    if (!ok) continue;
    const auto& index = c.shards[0].index;
    const auto& t = index.totals;
    constexpr std::string_view documents = "Documents in flushed index segments; excludes documents still in inverters.";
    r.gauge("luxir_index_documents", documents, (double)t.live_docs, labels + ",state=\"live\"");
    r.gauge("luxir_index_documents", documents, (double)t.deleted_docs, labels + ",state=\"deleted\"");
    r.gauge("luxir_index_segments", "Current flushed index segments.", (double)t.segments, labels);
    // A merge can leave the writer with fewer segments than the last commit.
    r.gauge("luxir_index_uncommitted_segments", "Flushed segments not in the last published snapshot.",
            t.segments > t.committed_segments ? (double)(t.segments - t.committed_segments) : 0, labels);
    r.gauge("luxir_index_committed_bytes", "Bytes in the current snapshot file descriptors; excludes in-flight and retained files.", (double)t.committed_bytes, labels);
    if (files) r.gauge("luxir_index_directory_bytes", "Bytes in the index directory listing, including metadata and in-flight files.", (double)t.bytes, labels);
    r.gauge("luxir_index_last_commit_timestamp_seconds", "Last published commit time since Unix epoch; age alone does not imply stale data.", index.commit_time / 1e6, labels);
    r.gauge("luxir_snapshot_pins", "Current snapshot pins.", (double)index.snapshot_pins, labels);
    r.gauge("luxir_snapshot_retained_bytes", "Retired bytes held only by snapshot pins.", (double)index.pin_retained_bytes, labels);
    r.counter("luxir_snapshot_pin_drops_total", "Snapshot pins dropped before release.", (double)index.pin_idle_drops, labels + ",reason=\"idle\"");
    r.counter("luxir_snapshot_pin_drops_total", "Snapshot pins dropped before release.", (double)index.pin_budget_drops, labels + ",reason=\"budget\"");
    r.gauge("luxir_merge_tasks", "Outstanding merge tasks, including queued work.", (double)index.active_merges, labels);
    r.gauge("luxir_inverters", "Indexing inverters by state.", (double)index.inverters_busy, labels + ",state=\"busy\"");
    r.gauge("luxir_inverters", "Indexing inverters by state.", (double)index.inverters_idle, labels + ",state=\"idle\"");
    r.gauge("luxir_inverters", "Indexing inverters by state.", (double)index.inverters_flushing, labels + ",state=\"flushing\"");
    r.counter("luxir_index_commits_total", "Commits since the writer opened.", (double)index.commits, labels);
    cache(r, index.query_cache, labels);
  }
  r.gauge("luxir_collections", "Collections by availability.", (double)available, "state=\"available\"");
  r.gauge("luxir_collections", "Collections by availability.", (double)unavailable, "state=\"unavailable\"");
  r.gauge("luxir_stats_collection_errors", "Collections whose statistics could not be read in this request.", (double)stats.collection_errors);
}

void systemStats(Renderer& r, const api::SystemStats& s) {
  if (s.process) {
    const auto& p = *s.process;
    constexpr std::string_view mem = "Process memory from procfs in bytes.";
    for (auto [name, value] : {std::pair{"process_resident_memory_bytes", p.resident_bytes},
        {"process_virtual_memory_bytes", p.virtual_bytes}, {"process_swap_memory_bytes", p.swap_bytes},
        {"process_resident_anonymous_bytes", p.resident_anonymous_bytes}, {"process_resident_file_bytes", p.resident_file_bytes},
        {"process_resident_shared_bytes", p.resident_shared_bytes}, {"process_resident_memory_peak_bytes", p.resident_peak_bytes}})
      r.gauge(name, mem, (double)value);
    r.gauge("process_threads", "Current process threads.", (double)p.threads);
    r.counter("process_cpu_seconds_total", "CPU time used by all process threads.", p.cpu_seconds);
    r.counter("process_context_switches_total", "Process context switches.", (double)p.voluntary_context_switches, "kind=\"voluntary\"");
    r.counter("process_context_switches_total", "Process context switches.", (double)p.involuntary_context_switches, "kind=\"involuntary\"");
    r.counter("process_page_faults_total", "Process page faults.", (double)p.minor_page_faults, "kind=\"minor\"");
    r.counter("process_page_faults_total", "Process page faults.", (double)p.major_page_faults, "kind=\"major\"");
    r.gauge("process_max_fds", "Soft file descriptor limit; -1 means unlimited.", (double)p.max_fds);
    r.gauge("process_open_fds", "Open file descriptors.", (double)p.open_fds);
    r.gauge("process_start_time_seconds", "Process start time since Unix epoch.", p.start_time_seconds);
    constexpr std::string_view io = "Process I/O accounting from procfs.";
    for (auto [name, value] : {std::pair{"process_disk_read_bytes_total", p.disk_read_bytes},
        {"process_disk_written_bytes_total", p.disk_written_bytes}, {"process_io_read_bytes_total", p.io_read_bytes},
        {"process_io_written_bytes_total", p.io_written_bytes}, {"process_io_read_calls_total", p.io_read_calls},
        {"process_io_write_calls_total", p.io_write_calls}})
      r.counter(name, io, (double)value);
  }
  if (s.cpu) {
    const auto& c = *s.cpu;
    for (const auto& cpu : c.cpus) {
      auto labels = PrometheusText::label("cpu", cpu.cpu);
      for (auto [mode, value] : {std::pair{"user", cpu.user}, {"nice", cpu.nice}, {"system", cpu.system},
          {"idle", cpu.idle}, {"iowait", cpu.iowait}, {"irq", cpu.irq}, {"softirq", cpu.softirq}, {"steal", cpu.steal}})
        r.counter("node_cpu_seconds_total", "CPU time by logical CPU and mode; user includes guest time.", value,
                  labels + "," + PrometheusText::label("mode", mode));
    }
    r.counter("node_context_switches_total", "Host context switches.", (double)c.context_switches);
    r.counter("node_forks_total", "Host tasks created.", (double)c.forks);
    r.gauge("node_procs_running", "Runnable tasks.", (double)c.procs_running);
    r.gauge("node_procs_blocked", "Tasks blocked on I/O.", (double)c.procs_blocked);
    r.gauge("node_boot_time_seconds", "Host boot time since Unix epoch.", c.boot_time_seconds);
    r.gauge("node_load1", "Host load average.", c.load1);
    r.gauge("node_load5", "Host load average.", c.load5);
    r.gauge("node_load15", "Host load average.", c.load15);
    r.gauge("node_filefd_allocated", "Host allocated file handles.", (double)c.filefd_allocated);
    r.gauge("node_filefd_maximum", "Host file handle limit.", (double)c.filefd_maximum);
  }
  if (s.memory) {
    const auto& m = *s.memory;
    for (auto [key, value] : {std::pair{"MemTotal", m.total_bytes}, {"MemFree", m.free_bytes},
        {"MemAvailable", m.available_bytes}, {"Buffers", m.buffers_bytes}, {"Cached", m.cached_bytes},
        {"SwapCached", m.swap_cached_bytes}, {"SwapTotal", m.swap_total_bytes}, {"SwapFree", m.swap_free_bytes},
        {"Dirty", m.dirty_bytes}, {"Writeback", m.writeback_bytes}, {"Mapped", m.mapped_bytes},
        {"Shmem", m.shmem_bytes}, {"Slab", m.slab_bytes}, {"SReclaimable", m.slab_reclaimable_bytes},
        {"SUnreclaim", m.slab_unreclaimable_bytes}, {"PageTables", m.page_tables_bytes},
        {"KernelStack", m.kernel_stack_bytes}, {"AnonPages", m.anon_pages_bytes}})
      r.gauge(std::string("node_memory_") + key + "_bytes", "Host memory from procfs in bytes.", (double)value);
    for (auto [key, value] : {std::pair{"pgfault", m.page_faults}, {"pgmajfault", m.major_page_faults},
        {"pswpin", m.swap_in_pages}, {"pswpout", m.swap_out_pages}, {"oom_kill", m.oom_kills}})
      r.counter(std::string("node_vmstat_") + key + "_total", "Kernel VM event count.", (double)value);
  }
  for (const auto& n : s.network) {
    auto labels = PrometheusText::label("device", n.device);
    for (auto [name, value] : {std::pair{"receive_bytes", n.receive_bytes}, {"receive_packets", n.receive_packets},
        {"receive_errs", n.receive_errs}, {"receive_drop", n.receive_drop}, {"receive_fifo", n.receive_fifo},
        {"receive_frame", n.receive_frame}, {"receive_compressed", n.receive_compressed},
        {"receive_multicast", n.receive_multicast}, {"transmit_bytes", n.transmit_bytes},
        {"transmit_packets", n.transmit_packets}, {"transmit_errs", n.transmit_errs}, {"transmit_drop", n.transmit_drop},
        {"transmit_fifo", n.transmit_fifo}, {"transmit_colls", n.transmit_colls},
        {"transmit_carrier", n.transmit_carrier}, {"transmit_compressed", n.transmit_compressed}})
      r.counter(std::string("node_network_") + name + "_total", "Network namespace interface counter.", (double)value, labels);
  }
  if (s.tcp) {
    r.gauge("node_netstat_Tcp_CurrEstab", "Established TCP connections in the network namespace.", (double)s.tcp->curr_estab);
    for (auto [name, value] : {std::pair{"RetransSegs", s.tcp->retrans_segs}, {"InErrs", s.tcp->in_errs},
        {"OutRsts", s.tcp->out_rsts}, {"AttemptFails", s.tcp->attempt_fails}})
      r.counter(std::string("node_netstat_Tcp_") + name + "_total", "TCP namespace event counter.", (double)value);
  }
  for (const auto& d : s.disks) {
    auto labels = PrometheusText::label("device", d.device);
    r.counter("node_disk_reads_completed_total", "Completed disk reads.", (double)d.reads_completed, labels);
    r.counter("node_disk_reads_merged_total", "Merged disk reads.", (double)d.reads_merged, labels);
    r.counter("node_disk_read_bytes_total", "Disk bytes read.", (double)d.read_bytes, labels);
    r.counter("node_disk_read_time_seconds_total", "Cumulative disk read time.", d.read_time_seconds, labels);
    r.counter("node_disk_writes_completed_total", "Completed disk writes.", (double)d.writes_completed, labels);
    r.counter("node_disk_writes_merged_total", "Merged disk writes.", (double)d.writes_merged, labels);
    r.counter("node_disk_written_bytes_total", "Disk bytes written.", (double)d.written_bytes, labels);
    r.counter("node_disk_write_time_seconds_total", "Cumulative disk write time.", d.write_time_seconds, labels);
    r.gauge("node_disk_io_now", "Disk requests in progress.", (double)d.io_now, labels);
    r.counter("node_disk_io_time_seconds_total", "Time disk had I/O in progress.", d.io_time_seconds, labels);
    r.counter("node_disk_io_time_weighted_seconds_total", "Weighted time spent doing I/O.", d.io_time_weighted_seconds, labels);
  }
  if (s.filesystem) {
    const auto& f = *s.filesystem;
    auto labels = PrometheusText::label("path", f.path);
    r.gauge("luxir_data_filesystem_size_bytes", "Data filesystem capacity.", (double)f.size_bytes, labels);
    r.gauge("luxir_data_filesystem_free_bytes", "Data filesystem free bytes including reserved blocks.", (double)f.free_bytes, labels);
    r.gauge("luxir_data_filesystem_avail_bytes", "Data filesystem bytes available to this process.", (double)f.avail_bytes, labels);
    r.gauge("luxir_data_filesystem_files", "Data filesystem inode capacity.", (double)f.files, labels);
    r.gauge("luxir_data_filesystem_files_free", "Data filesystem free inodes.", (double)f.files_free, labels);
    r.gauge("luxir_data_filesystem_readonly", "Data filesystem is mounted read only.", f.readonly, labels);
  }
  for (const auto& p : s.pressure) {
    constexpr std::string_view help = "Time stalled on a resource (PSI some/full).";
    auto labels = PrometheusText::label("resource", p.resource);
    r.counter("node_pressure_stall_seconds_total", help, p.some_seconds, labels + ",kind=\"some\"");
    if (p.full_seconds) r.counter("node_pressure_stall_seconds_total", help, *p.full_seconds, labels + ",kind=\"full\"");
  }
  if (s.cgroup) {
    if (const auto& m = s.cgroup->memory) {
      r.gauge("luxir_cgroup_memory_current_bytes", "Cgroup memory including page cache and descendants.", (double)m->current_bytes);
      r.gauge("luxir_cgroup_memory_max_bytes", "Local cgroup memory limit; -1 means unlimited. Ancestors may impose stricter limits.", (double)m->max_bytes);
      r.gauge("luxir_cgroup_memory_high_bytes", "Local cgroup memory throttle threshold; -1 means unlimited.", (double)m->high_bytes);
      for (auto [event, value] : {std::pair{"low", m->low_events}, {"high", m->high_events}, {"max", m->max_events},
          {"oom", m->oom_events}, {"oom_kill", m->oom_kill_events}})
        r.counter("luxir_cgroup_memory_events_total", "Cgroup memory events.", (double)value, PrometheusText::label("event", event));
    }
    if (const auto& c = s.cgroup->cpu) {
      r.counter("luxir_cgroup_cpu_seconds_total", "Cgroup CPU time.", c->usage_seconds);
      r.counter("luxir_cgroup_cpu_throttled_seconds_total", "Cgroup CPU throttling time.", c->throttled_seconds);
      r.counter("luxir_cgroup_cpu_periods_total", "Cgroup CPU scheduling periods.", (double)c->periods);
      r.counter("luxir_cgroup_cpu_throttled_periods_total", "Throttled CPU periods.", (double)c->throttled_periods);
      r.gauge("luxir_cgroup_cpu_quota_cores", "Local CPU quota in cores; -1 means unlimited. Ancestors may impose stricter limits.", c->quota_cores);
    }
    if (const auto& t = s.cgroup->tasks) {
      r.gauge("luxir_cgroup_tasks", "Cgroup task count (including threads).", (double)t->current);
      r.gauge("luxir_cgroup_tasks_max", "Local cgroup task limit; -1 means unlimited.", (double)t->max);
    }
  }
  // One series per collector that ran, so a failing source is alertable.
  auto failed = [&](std::string_view name) { return std::ranges::find(s.unavailable, name) != s.unavailable.end(); };
  constexpr std::string_view help = "Whether the system collector could read its sources; unavailable data is omitted.";
  for (auto collector : s.unavailable) r.gauge("luxir_stats_collector_success", help, 0, PrometheusText::label("collector", collector));
  for (std::string_view collector : {"host_stat", "process", "process_io", "cpu", "memory", "network", "tcp", "disk",
                                     "filesystem", "pressure", "cgroup", "cgroup_memory", "cgroup_cpu", "cgroup_tasks"}) {
    if (failed(collector) || failed("system")) continue;
    if (collector == "filesystem" && !s.filesystem) continue;
    if (collector.starts_with("cgroup_") && !s.cgroup) continue;
    if ((collector == "process" || collector == "process_io" || collector == "cpu") && failed("host_stat")) continue;
    r.gauge("luxir_stats_collector_success", help, 1, PrometheusText::label("collector", collector));
  }
}
} // namespace

PrometheusText::Family& PrometheusText::family(std::string_view name, std::string_view help, std::string_view type) {
  auto [it, inserted] = families.try_emplace(std::string(name), Family{std::string(help), std::string(type), {}});
  if (!inserted && it->second.type != type) throw std::logic_error("conflicting metric types");
  return it->second;
}
std::string PrometheusText::label(std::string_view key, std::string_view value) {
  std::string result(key);
  result += "=\"";
  for (char c : value) {
    if (c == '\\' || c == '"') result += '\\';
    if (c == '\n') result += "\\n";
    else result += c;
  }
  result += '"';
  return result;
}
void PrometheusText::sample(std::string_view name, std::string_view help, std::string_view type,
                            double value, std::string_view labels) {
  family(name, help, type).samples += line(name, value, labels);
}
void PrometheusText::histogram(std::string_view name, std::string_view help, std::span<const double> bounds,
                               const api::HistogramStats& histogram, std::string_view labels) {
  auto& f = family(name, help, "histogram");
  // counts is cumulative with +Inf last, and empty when nothing was observed.
  for (size_t i = 0; i <= bounds.size(); ++i) {
    uint64_t count = i < histogram.counts.size() ? histogram.counts[i] : 0;
    auto le = label("le", i == bounds.size() ? "+Inf" : number(bounds[i]));
    f.samples += line(std::string(name) + "_bucket", (double)count, join(labels, le));
  }
  f.samples += line(std::string(name) + "_sum", histogram.sum, labels);
  f.samples += line(std::string(name) + "_count", (double)histogram.count, labels);
}
std::string PrometheusText::str() const {
  std::string result;
  for (const auto& [name, family] : families) {
    result += "# HELP " + name + " " + family.help + "\n# TYPE " + name + " " + family.type + "\n" + family.samples;
  }
  return result;
}

std::string renderPrometheus(const api::StatsResponse& stats, bool files) {
  PrometheusText out;
  std::span<const double> bounds;
  if (stats.service) bounds = stats.service->duration_bounds;
  Renderer r(out, bounds);
  if (stats.service) service(r, *stats.service);
  if (stats.memory) memory(r, *stats.memory);
  if (stats.totals) collections(r, stats, files);
  if (stats.system) systemStats(r, *stats.system);
  r.counter("luxir_multiterm_expansion_refills_total", "Process-wide multi-term expansion refills.", (double)stats.multiterm_expansion_refills);
  r.gauge("luxir_stats_duration_seconds", "Time gathering this response, excluding serialization and transmission.", stats.duration_seconds);
  return out.str();
}
} // namespace luxir
