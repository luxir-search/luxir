"""Check input rejection and API error handling without engine fixtures."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('vector_compare', ROOT / 'vector-compare.py')
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class InputChecks(unittest.TestCase):
    def test_nested_api_errors(self):
        for response in [{'error': 'bad'}, {'results': [{'total_errors': 1}]},
                         {'nested': {'status': 'error'}}, {'errors': ['bad']}]:
            with self.subTest(response=response), self.assertRaises(RuntimeError):
                bench.checked(response)
        response = {'docs': [{'id': '1'}], 'total_errors': 0}
        self.assertEqual(bench.checked(response), response)

    def test_settings_rejected_before_binary_or_dataset_access(self):
        for settings in ['[]', '{"dims":1}', '{"type":"string"}', '{"metric":"l2"}']:
            result = subprocess.run([
                sys.executable, str(ROOT / 'vector-compare.py'),
                '--base-binary', '/missing/base', '--candidate-binary', '/missing/candidate',
                '--base-revision', 'base', '--candidate-revision', 'candidate',
                '--candidate-field-settings', settings,
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn('without type/dims/metric overrides', result.stderr)

    def test_presets_and_cache_provenance(self):
        self.assertEqual(bench.DEFAULT_DATASET, 'dbpedia-openai3-1536-100k')
        self.assertEqual(len(bench.PRESETS), 2)
        cohere = bench.PRESETS['cohere-wikipedia-en-v3-1024-100k']
        self.assertEqual((cohere['subset'], cohere['dims'], cohere['vector_column']),
                         ('en', 1024, 'emb'))
        self.assertEqual(len(cohere['files']), 1)
        for name, preset in bench.PRESETS.items():
            self.assertEqual((preset['index_count'], preset['query_count']), (99000, 1000))
            self.assertEqual(len(preset['revision']), 40)
            self.assertTrue(all(len(f['sha256']) == 64 for f in preset['files']))
            with tempfile.TemporaryDirectory() as tmp:
                cache = Path(tmp) / name
                cache.mkdir()
                (cache / 'dataset.json').write_text('{}')
                with self.assertRaisesRegex(RuntimeError, 'different source or revision'):
                    bench.dataset(Path(tmp), name)
        with self.assertRaises(KeyError):
            bench.dataset(Path('/unused'), 'https://arbitrary.invalid/data')

    def test_vector_validation_and_metric(self):
        np = bench.np
        for a, message in [(np.ones((2, 3)), 'dimensions'),
                           (np.array([[float('nan'), 1]]), 'Nonfinite'),
                           (np.array([[float('inf'), 1]]), 'Nonfinite'),
                           (np.zeros((1, 2)), 'Zero')]:
            with self.assertRaisesRegex(RuntimeError, message):
                bench.validate_vectors(a, 2)
        self.assertEqual(bench.validate_vectors(np.eye(2), 2)[0], 'ip')
        self.assertEqual(bench.validate_vectors(np.eye(2) * 2, 2)[0], 'cosine')

    def test_checksum_rejection(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'shard.parquet'
            path.write_bytes(b'corrupt')
            with self.assertRaisesRegex(RuntimeError, 'Checksum mismatch'):
                bench.download('https://unused.invalid', path, '0' * 64)

    def test_source_options_rejected_without_network(self):
        with tempfile.TemporaryDirectory() as tmp:
            for repo, ref in [('bad;repo/name', 'main'), ('owner/repo', '--upload-pack=bad')]:
                work = Path(tmp) / ('work' + str(len(list(Path(tmp).iterdir()))))
                result = subprocess.run([str(ROOT / 'compare-refs')], env={
                    **os.environ, 'WORK_DIR': str(work), 'BASE_REPO': repo, 'BASE_REF': ref,
                    'CANDIDATE_REPO': 'owner/repo', 'CANDIDATE_REF': 'main',
                    'DEPS_REGISTRY': 'example.invalid/deps',
                }, capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((work / 'base/.git').exists())


if __name__ == '__main__':
    unittest.main()
