#include "SoluxConfig.h"
#include <limits>
#include "spdlog/spdlog.h"

namespace solux {

/*
Rough hierarchy:
[store]          # storage backend
[index]          # indexing behavior (ram buffer, etc.)
[merge]          # merge policy/scheduling
[cache.filter]   # named cache instances
[cache.query]
[query]          # query parsing defaults
[request]        # request handling (threads, timeouts)
[search]         # search defaults (hits, etc.)
*/


void SoluxConfig::addOptions(CLI::App& app) {
  app.add_option("--log-level", log_level, "Log level (trace, debug, info, warn, error, critical)")
      ->default_val(log_level);
  app.add_option("--filter-cache-bytes", filterCacheBytes,
                 "Per-shard filter cache payload budget (0 disables it)")
      ->transform(CLI::AsSizeValue(false))
      ->default_str("64MB");

  // No default_val: leaving the bound field at its sentinel (<0) lets normalize()
  // derive the gRPC port as http.port + 1 unless the user sets one explicitly.
  app.add_option("--server.grpc.port", server.grpc.port, "gRPC listen port (default: HTTP port + 1)");
  app.add_option("--server.grpc.threads", server.grpc.threads, "Number of server threads (0 = auto)")
      ->default_val(server.grpc.threads);

  app.add_flag("--server.http.enabled,!--no-http", server.http.enabled, "Enable the HTTP/JSON server")
      ->default_val(server.http.enabled);
  // -p is the HTTP port: the HTTP/JSON API is the surface developers hit first.
  app.add_option("--server.http.port,-p", server.http.port, "HTTP/JSON listen port")
      ->default_val(server.http.port);
  app.add_option("--server.http.threads", server.http.threads, "Number of HTTP server threads (0 = auto)")
      ->default_val(server.http.threads);
  app.add_option("--server.stream_buffer_bytes", server.stream_buffer_bytes,
                 "Per-connection buffered response bytes before streaming producers pause")
      ->default_val(server.stream_buffer_bytes);

  app.add_option("--store.backend", store.backend, "Storage backend (ram, fs)")
      ->default_val(store.backend)
      ->check(CLI::IsMember({"ram", "fs"}));
  app.add_option("--store.data-dir", store.data_dir, "Base path for filesystem storage")
      ->default_val(store.data_dir);
  app.add_option("--store.checked-dir.sync", store.checked_dir.sync, "Check fsync correctness: off, warn, throw")
      ->default_val(store.checked_dir.sync)
      ->check(CLI::IsMember({"off", "warn", "throw"}));

  app.add_option("--index.max-inverter-ram-mb", index.max_inverter_ram_mb,
                 "Per-inverter RAM cap (MiB) before an auto-flush to a segment")
      ->default_val(index.max_inverter_ram_mb)
      ->check(CLI::PositiveNumber);
  app.add_option("--index.max-inverter-docs", index.max_inverter_docs,
                 "Per-inverter doc-count cap before an auto-flush to a segment")
      ->default_val(index.max_inverter_docs)
      ->check(CLI::PositiveNumber);
  app.add_option("--index.merge-factor", index.merge_factor,
                 "Same-level segments required to trigger a merge")
      ->default_val(index.merge_factor)
      ->check(CLI::PositiveNumber)
      ->check(CLI::Range(2, (std::numeric_limits<int>::max)()));
  app.add_option("--max-index-ram", index.max_index_ram_mb,
                 "Shared index RAM cap for merge admission (MiB, 0 = unlimited)")
      ->default_val(index.max_index_ram_mb)
      ->check(CLI::NonNegativeNumber);

  app.add_option("--ingest.max-request-body", ingest.max_request_body,
                 "Max buffered (non-streaming) request body; oversized -> 413 (e.g. 32MB)")
      ->transform(CLI::AsSizeValue(false))
      ->default_str("32MB");
  app.add_option("--ingest.max-record", ingest.max_record,
                 "Max size of one NDJSON record / document (default: max-request-body)")
      ->transform(CLI::AsSizeValue(false));
  app.add_option("--ingest.stream-batch-size", ingest.stream_batch_size,
                 "Streaming NDJSON: byte size for internal (non-atomic) mini-batches")
      ->transform(CLI::AsSizeValue(false))
      ->default_str("1MB");
  app.add_option("--ingest.stream-batch-docs", ingest.stream_batch_docs,
                 "Streaming NDJSON: doc count for internal (non-atomic) mini-batches")
      ->default_val(ingest.stream_batch_docs)
      ->check(CLI::PositiveNumber);
  app.add_option("--ingest.max-inflight-batches", ingest.max_inflight_batches,
                 "Streaming NDJSON: batches in flight per connection (0 = task arena concurrency + 2)")
      ->default_val(ingest.max_inflight_batches)
      ->check(CLI::NonNegativeNumber);
  app.add_flag("--ingest.auto-create-collection,!--no-ingest.auto-create-collection",
               ingest.auto_create_collection, "Create missing collections on first use")
      ->default_val(ingest.auto_create_collection);
}

void SoluxConfig::normalize() {
  // Default the gRPC port to one past the HTTP port unless it was set explicitly.
  if (server.grpc.port < 0) server.grpc.port = server.http.port + 1;

  if (store.backend == "ram" && store.data_dir != "solux_data") {
    spdlog::warn("store.data-dir is ignored when store.backend=ram");
  }
}

void SoluxConfig::apply() const {
  auto level = spdlog::level::from_str(log_level);
  spdlog::set_level(level);
}

} // namespace solux
