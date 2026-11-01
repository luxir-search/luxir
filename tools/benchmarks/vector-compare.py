#!/usr/bin/env python3
"""Interleaved base/candidate HTTP vector comparison. Requires numpy, pyarrow, requests."""
import argparse
import copy
from contextlib import ExitStack
from types import SimpleNamespace
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import socket
import statistics
import subprocess
import time

import numpy as np
import pyarrow.parquet as pq
import requests

DEFAULT_DATASET = 'dbpedia-openai3-1536-100k'
PRESETS = {
    'dbpedia-openai3-1536-100k': {'dataset': 'Qdrant/dbpedia-entities-openai3-text-embedding-3-large-1536-100K',
     'revision': 'e8931e5eeba5b31bb9481f98687cf0ced14c2442',
     'subset': 'default',
     'files': [{'path': 'data/train-00000-of-00003.parquet',
                'sha256': '793fa64beb2d89added80b2a4640719e6a26216f31122d222f7b37ee2dbc24cb'},
               {'path': 'data/train-00001-of-00003.parquet',
                'sha256': 'd9831c504ad11a88c9ebe1b5e1b5379cee6b025eccf76f0f3594e6511993fc21'},
               {'path': 'data/train-00002-of-00003.parquet',
                'sha256': 'af9407b804c593e4f988a6463ea66409b9f3ec91fa110e7b3e2e9c175baa6efb'}],
     'vector_column': 'text-embedding-3-large-1536-embedding',
     'dims': 1536,
     'index_count': 99000,
     'query_count': 1000,
     'license': {'card': 'undeclared', 'api': 'undeclared'}},
    'cohere-wikipedia-en-v3-1024-100k': {'dataset': 'CohereLabs/wikipedia-2023-11-embed-multilingual-v3',
     'revision': 'ade45fb52bd549f5e8c065636fe4160a43c2af36',
     'subset': 'en',
     'files': [{'path': 'en/0000.parquet',
                'sha256': '2c6abfffa7dd336113251b3e6f3fe4ee16688ead7c67b99593cbadc5589e28b3'}],
     'vector_column': 'emb',
     'dims': 1024,
     'index_count': 99000,
     'query_count': 1000,
     'license': {'card': 'undeclared', 'api': 'undeclared'}},
}


# Luxir has no listen-address flag. Restrict this owned process to loopback
# without changing host firewall/services or the public source under test.
LOOPBACK_BIND = r"""
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
int bind(int fd, const struct sockaddr *addr, socklen_t len) {
    int (*next_bind)(int, const struct sockaddr *, socklen_t) = dlsym(RTLD_NEXT, "bind");
    struct sockaddr_storage copy;
    if (len <= sizeof(copy) && (addr->sa_family == AF_INET || addr->sa_family == AF_INET6)) {
        memset(&copy, 0, sizeof(copy));
        memcpy(&copy, addr, len);
        if (addr->sa_family == AF_INET)
            ((struct sockaddr_in *)&copy)->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        else
            ((struct sockaddr_in6 *)&copy)->sin6_addr = in6addr_loopback;
        return next_bind(fd, (const struct sockaddr *)&copy, len);
    }
    return next_bind(fd, addr, len);
}
"""


def download(url, path, sha=None):
    if not path.exists():
        tmp = path.with_suffix('.partial')
        with requests.get(url, stream=True, timeout=120) as r:
            r.raise_for_status()
            with tmp.open('wb') as f:
                for chunk in r.iter_content(1024 * 1024):
                    f.write(chunk)
        tmp.replace(path)
    with path.open('rb') as f:
        digest = hashlib.file_digest(f, 'sha256').hexdigest()
    if sha and digest != sha:
        raise RuntimeError(f'Checksum mismatch: {path}')
    return digest


def validate_vectors(a, dims):
    if a.ndim != 2 or a.shape[1] != dims:
        raise RuntimeError('Wrong vector dimensions')
    if not np.isfinite(a).all():
        raise RuntimeError('Nonfinite vector')
    norms = np.linalg.norm(a, axis=1)
    if np.any(norms == 0):
        raise RuntimeError('Zero vector')
    metric = 'ip' if np.allclose(norms, 1, rtol=0, atol=0.001) else 'cosine'
    return metric, {'min': float(norms.min()), 'max': float(norms.max()),
                    'max_unit_error': float(np.abs(norms - 1).max()), 'unit_tolerance': 0.001}


def dataset(cache, name=DEFAULT_DATASET):
    # Each preset has its own cache namespace; immutable metadata is checked.
    preset = PRESETS[name]
    cache = cache / name
    cache.mkdir(parents=True, exist_ok=True)
    meta = copy.deepcopy(preset)
    meta['preset'] = name
    manifest = cache / 'dataset.json'
    if manifest.exists() and json.loads(manifest.read_text()) != meta:
        raise RuntimeError('Dataset cache has a different source or revision')
    manifest.write_text(json.dumps(meta, indent=2) + '\n')
    count = meta['index_count'] + meta['query_count']
    dims = meta['dims']
    a = np.empty((count, dims), dtype=np.float32)
    offset = 0
    for item in meta['files']:
        if offset == count:
            break
        path = cache / Path(item['path']).name
        download(f'https://huggingface.co/datasets/{meta["dataset"]}/resolve/{meta["revision"]}/{item["path"]}', path, item['sha256'])
        parquet = pq.ParquetFile(path)
        if meta['vector_column'] not in parquet.schema_arrow.names:
            raise RuntimeError('Missing vector column')
        for batch in parquet.iter_batches(batch_size=1024, columns=[meta['vector_column']]):
            take = min(batch.num_rows, count - offset)
            column = batch.column(0).slice(0, take)
            if not np.all(column.value_lengths().to_numpy() == dims):
                raise RuntimeError('Wrong vector dimensions')
            a[offset:offset + take] = column.flatten().to_numpy().reshape(-1, dims)
            offset += take
            if offset == count:
                break
    if offset != count:
        raise RuntimeError(f'Unexpected dataset rows: {offset}')
    metric, meta['norms'] = validate_vectors(a, dims)
    stream = cache / 'train.ndjson'
    if not stream.exists():
        tmp = cache / 'train.partial'
        with tmp.open('w') as f:
            for i, v in enumerate(a[:meta['index_count']]):
                f.write(json.dumps({'id': str(i), 'embedding_v': v.tolist()}, separators=(',', ':'), allow_nan=False) + '\n')
            f.write('{"_end_":{"commit":{}}}\n')
        tmp.replace(stream)
    meta['ndjson_sha256'] = hashlib.file_digest(stream.open('rb'), 'sha256').hexdigest()
    return a[meta['index_count']:].copy(), stream, metric, meta


def checked(body):
    if isinstance(body, dict):
        if body.get('error') or body.get('errors') or body.get('total_errors', 0) or body.get('status') == 'error':
            raise RuntimeError(str(body))
        for v in body.values():
            if isinstance(v, (dict, list)):
                checked(v)
    elif isinstance(body, list):
        for v in body:
            checked(v)
    return body


def variant(args, queries, stream, metric, meta):
    args.output.mkdir(parents=True, exist_ok=True)
    data = args.output.resolve() / 'data'
    if data.exists():
        raise RuntimeError('Use a fresh output directory; never reuse collection state')
    # Reserve both ports until immediately before starting the owned process.
    sockets = []
    for _ in range(2):
        s = socket.socket()
        s.bind(('127.0.0.1', 0))
        sockets.append(s)
    port, grpc = [s.getsockname()[1] for s in sockets]
    cmd = [str(Path(args.binary).resolve()), '--store.backend=fs', f'--store.data-dir={data}', f'--server.http.port={port}', f'--server.grpc.port={grpc}', f'--server.workers={args.workers}', '--max-ram-mb=4096']
    session = requests.Session()
    base = f'http://127.0.0.1:{port}'

    def call(path, body=None):
        r = session.post(base + path, json=body, timeout=7200) if body is not None else session.get(base + path, timeout=120)
        r.raise_for_status()
        return checked(r.json())

    def search(v, nprobe=0, exact=False):
        knn = {'field': 'embedding_v', 'query': v, 'k': 10, 'exact': exact, 'ivf': {'nprobe': nprobe}}
        return {'query': {'knn': knn}, 'limit': 10, 'fields': ['id']}

    cpu = Path('/proc/cpuinfo').read_text()
    (args.output / 'cpuinfo.txt').write_text(cpu)
    result = {'label': args.label, 'source_revision': args.source_revision, 'binary_sha256': hashlib.file_digest(Path(args.binary).open('rb'), 'sha256').hexdigest(), 'harness_sha256': hashlib.file_digest(Path(__file__).open('rb'), 'sha256').hexdigest(), 'dataset': meta, 'metric': metric, 'index_count': meta['index_count'], 'query_count': meta['query_count'], 'k': 10, 'workers': args.workers, 'runner': {k: os.getenv(k) for k in ['RUNNER_NAME', 'RUNNER_OS', 'RUNNER_ARCH', 'GITHUB_RUN_ID']}, 'host': platform.uname()._asdict(), 'cpu_model': next((x.split(':', 1)[1].strip() for x in cpu.splitlines() if x.startswith('model name')), platform.machine()), 'simd': {x: x in cpu.lower() for x in ['avx2', 'avx512f', 'neon', 'asimd']}, 'command': cmd, 'rows': []}
    log = (args.output / 'server.log').open('w')
    shim_source = args.output.resolve() / 'loopback-bind.c'
    shim = args.output.resolve() / 'loopback-bind.so'
    shim_source.write_text(LOOPBACK_BIND)
    subprocess.run(['cc', '-shared', '-fPIC', '-O2', str(shim_source), '-ldl', '-o', str(shim)], check=True)
    result['loopback_bind_sha256'] = hashlib.file_digest(shim.open('rb'), 'sha256').hexdigest()
    for s in sockets:
        s.close()
    proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, env={**os.environ, 'OMP_NUM_THREADS': str(args.workers), 'OPENBLAS_NUM_THREADS': '1', 'LD_PRELOAD': str(shim)})
    try:
        deadline = time.monotonic() + 120
        while True:
            if proc.poll() is not None:
                raise RuntimeError('Server exited; see server.log')
            try:
                call('/collections')
                break
            except requests.RequestException:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.2)
        name = args.variant
        print(f'{name}: schema/ingest', flush=True)
        schema = {'fields': {'embedding_v': {'type': 'vector', 'dims': meta['dims'], 'metric': metric, **copy.deepcopy(args.settings)}}}
        (args.output / f'{name}-schema.json').write_text(json.dumps(schema, indent=2))
        call(f'/collections/{name}/_schema', schema)
        start = time.perf_counter()
        with stream.open('rb') as f:
            r = session.post(base + f'/collections/{name}/_update', data=f, headers={'Content-Type': 'application/x-ndjson'}, timeout=7200)
            r.raise_for_status()
            updates = [checked(json.loads(line)) for line in r.text.splitlines() if line]
        if not updates:
            raise RuntimeError('Empty ingest response')
        visible = call(f'/collections/{name}/_search', {'query': '*:*', 'limit': 0, 'count_limit': -1})
        (args.output / f'{name}-ingest.json').write_text(json.dumps({'updates': updates, 'visible': visible}, indent=2))
        if visible.get('found') != meta['index_count']:
            raise RuntimeError(f'Expected {meta["index_count"]} visible docs: {visible}')
        ingest = time.perf_counter() - start
        print(f'{name}: ANN build', flush=True)
        start = time.perf_counter()
        build = call(f'/collections/{name}/_update', {'commit': {'build_aux_indexes': ['*'], 'max_segments': 1, 'wait_for_merges': True}})
        build_s = time.perf_counter() - start
        stats = call(f'/collections/{name}/_stats?files=true&segments=true')
        (args.output / f'{name}-build.json').write_text(json.dumps({'response': build, 'stats': stats}, indent=2))
        collections = stats['collections']
        index = collections[0]['shards'][0]['index']
        segments = index.get('segments', [])
        if len(segments) != 1 or int(index['totals']['live_docs']) != meta['index_count']:
            raise RuntimeError(f'Expected one {meta["index_count"]}-document segment: {stats}')
        overlays = segments[0].get('overlays', [])
        if not any(x.get('field') == 'embedding_v' and x.get('files') for x in overlays):
            raise RuntimeError(f'ANN overlay missing; refusing to benchmark exact fallback: {stats}')
        committed_bytes = int(index['totals']['committed_bytes'])
        # Persist the entire stats response for independent audit.
        directory = data / 't' / 't0' / name
        files = [x for x in directory.rglob('*') if x.is_file()]
        size = sum(x.stat().st_size for x in files)
        allocated = sum(x.stat().st_blocks * 512 for x in files)
        (args.output / f'{name}-files.json').write_text(json.dumps({str(x.relative_to(directory)): x.stat().st_size for x in files}, indent=2))
        print(f'{name}: exact ground truth', flush=True)
        vectors = queries.tolist()
        truth = []
        for v in vectors:
            docs = call(f'/collections/{name}/_search', search(v, exact=True))['docs']
            ids = {d['id'] for d in docs}
            if len(ids) != 10:
                raise RuntimeError('Exact top ten underfilled')
            truth.append(ids)
        (args.output / f'{name}-truth.json').write_text(json.dumps([sorted(x) for x in truth]))
        # Yield after preparation, then one full pass at a time. The caller
        # rotates variant order on every pass; all three processes stay alive.
        yield None
        all_passes = {n: [] for n in [0, 16, 64]}
        for nprobe in [0, 16, 64]:
            payloads = [json.dumps(search(v, nprobe), separators=(',', ':')).encode() for v in vectors]
            passes = all_passes[nprobe]
            for pass_no in range(4):
                times, recalls = [], []
                for payload, expected in zip(payloads, truth):
                    start = time.perf_counter_ns()
                    r = session.post(base + f'/collections/{name}/_search', data=payload, headers={'Content-Type': 'application/json'}, timeout=120)
                    elapsed = (time.perf_counter_ns() - start) / 1e6
                    r.raise_for_status()
                    ids = {d['id'] for d in checked(r.json())['docs']}
                    if len(ids) != 10:
                        raise RuntimeError('ANN top ten underfilled')
                    times.append(elapsed)
                    recalls.append(len(ids & expected) / 10)
                if pass_no:
                    passes.append({'p50_ms': float(np.percentile(times, 50)), 'p99_ms': float(np.percentile(times, 99)), 'recall_at_10': statistics.mean(recalls), 'latencies_ms': times})
                print(f'{name} nprobe={nprobe} pass={pass_no} done', flush=True)
                if pass_no == 3:
                    row = {'variant': name, 'field_settings': args.settings, 'nprobe': 'adaptive' if nprobe == 0 else nprobe, 'ingest_s': ingest, 'ann_build_s': build_s, 'index_bytes': size, 'committed_bytes': committed_bytes, 'allocated_bytes': allocated, **{k: statistics.median(x[k] for x in passes) for k in ['p50_ms', 'p99_ms', 'recall_at_10']}, 'passes': passes}
                    result['rows'].append(row)
                    (args.output / 'results.json').write_text(json.dumps(result, indent=2))
                yield {'variant': name, 'nprobe': nprobe, 'pass': pass_no}
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        log.close()
        session.close()
        listeners_closed = {}
        for listener in [port, grpc]:
            with socket.socket() as probe:
                probe.settimeout(1)
                listeners_closed[str(listener)] = probe.connect_ex(('127.0.0.1', listener)) != 0
        (args.output / 'shutdown.json').write_text(json.dumps({'exit_code': proc.returncode, 'listeners_closed': listeners_closed}, indent=2))


def main():
    def interrupted(signum, _frame):
        raise SystemExit(128 + signum)

    signal.signal(signal.SIGTERM, interrupted)
    p = argparse.ArgumentParser()
    p.add_argument('--base-binary', required=True)
    p.add_argument('--candidate-binary', required=True)
    p.add_argument('--base-revision', required=True)
    p.add_argument('--candidate-revision', required=True)
    p.add_argument('--candidate-field-settings', default='{}', help='JSON merged into candidate vector field; base/default receive none')
    p.add_argument('--dataset', choices=PRESETS, default=DEFAULT_DATASET)
    p.add_argument('--cache', type=Path, default=Path('.vector-dataset'))
    p.add_argument('--output', type=Path, default=Path('vector-results'))
    p.add_argument('--label', default='local')
    p.add_argument('--workers', type=int, default=2)
    args = p.parse_args()
    settings = json.loads(args.candidate_field_settings)
    if not isinstance(settings, dict) or any(k in settings for k in ['type', 'dims', 'metric']):
        p.error('Candidate settings must be an object without type/dims/metric overrides')
    args.output.mkdir(parents=True, exist_ok=False)
    queries, stream, metric, meta = dataset(args.cache, args.dataset)
    specs = [('base', args.base_binary, args.base_revision, {}), ('candidate_feature', args.candidate_binary, args.candidate_revision, settings), ('candidate_default', args.candidate_binary, args.candidate_revision, {})]
    schedule = []
    with ExitStack() as stack:
        generators = []
        for name, binary, revision, field_settings in specs:
            config = SimpleNamespace(**{**vars(args), 'variant': name, 'binary': binary, 'source_revision': revision, 'settings': field_settings, 'output': args.output / name})
            gen = variant(config, queries, stream, metric, meta)
            stack.callback(gen.close)
            next(gen)  # prepare collection and exact ground truth
            generators.append(gen)
        for turn in range(12):
            order = list(range(3))
            order = order[turn % 3:] + order[:turn % 3]
            for i in order:
                schedule.append(next(generators[i]))
    results = [json.loads((args.output / name / 'results.json').read_text()) for name, *_ in specs]
    truths = [json.loads((args.output / name / f'{name}-truth.json').read_text()) for name, *_ in specs]
    if any(t != truths[0] for t in truths):
        raise RuntimeError('Exact ground truth differs between revisions/variants')
    rows = []
    for result in results:
        for row in result['rows']:
            base = next(r for r in results[0]['rows'] if r['nprobe'] == row['nprobe'])
            row['delta_percent_vs_base'] = {k: 100 * (row[k] / base[k] - 1) for k in ['ingest_s', 'ann_build_s', 'index_bytes', 'p50_ms', 'p99_ms']}
            row['recall_delta_points_vs_base'] = 100 * (row['recall_at_10'] - base['recall_at_10'])
            rows.append(row)
    output = {'label': args.label, 'variants': results, 'rows': rows, 'pass_schedule': schedule, 'exact_truth_equal': True}
    (args.output / 'results.json').write_text(json.dumps(output, indent=2))
    lines = [f'## {args.label}: base vs candidate', '', f'Base `{args.base_revision}`; candidate `{args.candidate_revision}`.', f'{results[0]["cpu_model"]}; SIMD `{results[0]["simd"]}`; {meta["index_count"]} indexed / {meta["query_count"]} queries, {meta["dims"]}d, {metric}; dataset `{args.dataset}`.', '', 'Single measurements in base, feature, default order; percent change vs base.', '', '| Variant | Ingest s | Merge + ANN build s | Index MiB |', '|---|---:|---:|---:|']
    for result in results:
        row = result['rows'][0]
        d = row['delta_percent_vs_base']
        lines.append(f'| {row["variant"]} | {row["ingest_s"]:.2f} ({d["ingest_s"]:+.1f}%) | {row["ann_build_s"]:.2f} ({d["ann_build_s"]:+.1f}%) | {row["index_bytes"] / 2**20:.1f} ({d["index_bytes"]:+.2f}%) |')
    lines += ['', 'Median of three timed passes per nprobe after one warm-up; variant order rotates every pass. Latency change is percent vs base; recall change is in percentage points.', '', '| Variant | nprobe | p50 ms | p99 ms | Recall@10 |', '|---|---|---:|---:|---:|']
    for row in rows:
        d = row['delta_percent_vs_base']
        lines.append(f'| {row["variant"]} | {row["nprobe"]} | {row["p50_ms"]:.3f} ({d["p50_ms"]:+.1f}%) | {row["p99_ms"]:.3f} ({d["p99_ms"]:+.1f}%) | {row["recall_at_10"]:.4f} ({row["recall_delta_points_vs_base"]:+.2f} pp) |')
    lines += ['', 'Shared-runner ratios carry noise; recall uses identical exact:true ground truth.']
    markdown = '\n'.join(lines) + '\n'
    (args.output / 'summary.md').write_text(markdown)
    if os.getenv('GITHUB_STEP_SUMMARY'):
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as f:
            f.write(markdown)
    print(markdown)


if __name__ == '__main__':
    main()
