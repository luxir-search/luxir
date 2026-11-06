#!/usr/bin/env python3
"""Dependency identity contract tests, using tiny synthetic stage inputs."""
import importlib.util
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('deps_key', Path(__file__).with_name('deps-key.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

DOCKERFILE = '''FROM ubuntu:22.04 AS toolchain
COPY .devcontainer/check-toolchain.cpp .devcontainer/check-toolchain.sh /opt/luxir-checks/
FROM toolchain AS normal-dependencies
COPY deps/vcpkg.json deps/vcpkg-configuration.json /opt/luxir-deps/
COPY deps/ports /opt/luxir-deps/ports
COPY deps/triplets /opt/luxir-deps/triplets
ARG BUILD_JOBS=12
COPY test/blas_lapack_test.cpp /opt/luxir-checks/check-lapack.cpp
FROM normal-dependencies AS dependencies
RUN echo later-stage
FROM toolchain AS dev
COPY --from=dependencies /opt/vcpkg /opt/vcpkg
COPY --from=dependencies /opt/luxir-deps /opt/luxir-deps
RUN apt-get install wamerican
'''


class KeyContract(unittest.TestCase):
    def test_inputs_and_normalization(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            inputs = ['.devcontainer/check-toolchain.cpp', '.devcontainer/check-toolchain.sh',
                      'deps/vcpkg.json', 'deps/vcpkg-configuration.json',
                      'deps/ports/nested/portfile.cmake', 'deps/triplets/custom.cmake',
                      'test/blas_lapack_test.cpp']
            for name in inputs:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(name)
            dockerfile = root / '.devcontainer/Dockerfile'
            dockerfile.write_text(DOCKERFILE)
            (root / '.devcontainer/Dockerfile.dockerignore').write_text('**\n!deps/**\n')
            before = {p: p.read_bytes() for p in root.rglob('*') if p.is_file()}
            key = module.deps_key(root)
            self.assertEqual(before, {p: p.read_bytes() for p in root.rglob('*') if p.is_file()})
            self.assertEqual(key, module.deps_key(root), 'key calculation must be repeatable')
            for name in inputs:
                path = root / name
                old = path.read_text()
                path.write_text(old + '\nchanged')
                self.assertNotEqual(key, module.deps_key(root), name)
                path.write_text(old)
            (root / '.devcontainer/Dockerfile.dockerignore').write_text('changed context rules')
            self.assertEqual(key, module.deps_key(root))
            dockerfile.write_text(DOCKERFILE.replace('BUILD_JOBS=12', 'BUILD_JOBS=32'))
            self.assertEqual(key, module.deps_key(root))
            dockerfile.write_text(DOCKERFILE.replace('RUN echo later-stage', 'RUN echo changed-later'))
            (root / 'engine.cpp').write_text('unrelated engine source')
            self.assertNotEqual(key, module.deps_key(root))
            for old, new in [('RUN apt-get install wamerican', 'RUN apt-get install other'),
                             ('AS dev', 'AS other'),
                             ('--from=dependencies /opt/vcpkg', '--from=toolchain /opt/vcpkg'),
                             ('FROM normal-dependencies', 'FROM toolchain')]:
                dockerfile.write_text(DOCKERFILE.replace(old, new))
                if old.startswith('RUN'):
                    self.assertNotEqual(key, module.deps_key(root))
                else:
                    with self.assertRaises(ValueError):
                        module.deps_key(root)
            dockerfile.write_text(DOCKERFILE.replace('ubuntu:22.04', 'ubuntu:24.04'))
            self.assertNotEqual(key, module.deps_key(root))
            dockerfile.write_text(DOCKERFILE.replace('ARG BUILD_JOBS=12', 'COPY new-input /opt/new\nARG BUILD_JOBS=12'))
            with self.assertRaises(ValueError):
                module.deps_key(root)
            for extra in [' copy new-input /opt/new', ' add file /opt/file', ' ONBUILD RUN echo unsafe', 'ADD remote /tmp/remote']:
                dockerfile.write_text(DOCKERFILE.replace('ARG BUILD_JOBS=12', extra + '\nARG BUILD_JOBS=12'))
                with self.assertRaises(ValueError):
                    module.deps_key(root)
            dockerfile.write_text(DOCKERFILE)
            missing = root / inputs[0]
            missing.unlink()
            with self.assertRaisesRegex(ValueError, 'Missing dependency input'):
                module.deps_key(root)
            missing.symlink_to(root / inputs[1])
            with self.assertRaisesRegex(ValueError, 'Symlink input'):
                module.deps_key(root)
            missing.unlink()
            missing.write_text(inputs[0])
            # The candidate's earlier Dockerfile stores the check beside the Dockerfile.
            check = root / '.devcontainer/check-lapack.cpp'
            check.write_text('candidate LAPACK check')
            dockerfile.write_text(DOCKERFILE.replace(
                'test/blas_lapack_test.cpp /opt/luxir-checks/check-lapack.cpp',
                '.devcontainer/check-lapack.cpp /opt/luxir-checks/'))
            self.assertNotEqual(key, module.deps_key(root))


if __name__ == '__main__':
    unittest.main()
