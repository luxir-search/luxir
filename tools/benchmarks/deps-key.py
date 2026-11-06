#!/usr/bin/env python3
"""Hash the complete dev image build inputs; emit one SHA-256 tag."""
import hashlib
import re
import sys
from pathlib import Path


def deps_key(root):
    root = Path(root)
    dockerfile = (root / '.devcontainer/Dockerfile').read_text()
    # The dev target uses all four stages, including both dependency variants.
    # Require the reviewed graph; a new stage or changed edge needs key review.
    stages = re.findall(r'^FROM (.+)$', dockerfile, re.M)
    if (len(stages) != 4 or not stages[0].endswith(' AS toolchain')
            or stages[1:] != ['toolchain AS normal-dependencies',
                              'normal-dependencies AS dependencies', 'toolchain AS dev']):
        raise ValueError('Dockerfile stage graph changed; review deps-key.py')
    stage = dockerfile
    copies = re.findall(r'^[ \t]*COPY[ \t]+(.+)$', stage, re.M | re.I)
    expected = [
        '.devcontainer/check-toolchain.cpp .devcontainer/check-toolchain.sh /opt/luxir-checks/',
        'deps/vcpkg.json deps/vcpkg-configuration.json /opt/luxir-deps/',
        'deps/ports /opt/luxir-deps/ports',
        'deps/triplets /opt/luxir-deps/triplets',
        'test/blas_lapack_test.cpp /opt/luxir-checks/check-lapack.cpp',
        '--from=dependencies /opt/vcpkg /opt/vcpkg',
        '--from=dependencies /opt/luxir-deps /opt/luxir-deps',
    ]
    if 'COPY .devcontainer/check-lapack.cpp /opt/luxir-checks/' in stage:
        expected[4] = '.devcontainer/check-lapack.cpp /opt/luxir-checks/'
    if copies != expected or re.search(r'^[ \t]*(ADD|ONBUILD)\b', stage, re.M | re.I):
        raise ValueError('Dependency COPY/ADD contract changed; review deps-key.py')
    # BUILD_JOBS controls concurrency, not dependency identity.
    stage = re.sub(r'^ARG BUILD_JOBS=.*$', 'ARG BUILD_JOBS=<concurrency>', stage, flags=re.M)
    # Ignore rules affect whether COPY succeeds, not dependency identity.
    # Hash every declared input; the Docker build still validates its context.
    files = {'.devcontainer/Dockerfile': stage.encode()}
    for copy in expected:
        if copy.startswith('--from='):
            continue  # Internal stage output; its producing stages are hashed above.
        for source in copy.split()[:-1]:
            path = root / source
            if path.is_symlink():
                raise ValueError(f'Symlink input requires review: {path}')
            paths = sorted(path.rglob('*')) if path.is_dir() else [path]
            for item in paths:
                if item.is_symlink():
                    raise ValueError(f'Symlink input requires review: {item}')
                if not item.exists():
                    raise ValueError(f'Missing dependency input: {item}')
                if item.is_file():
                    files[item.relative_to(root).as_posix()] = item.read_bytes()
    digest = hashlib.sha256()
    for name, data in sorted(files.items()):
        for value in (name.encode(), data):
            digest.update(len(value).to_bytes(8, 'big'))
            digest.update(value)
    return digest.hexdigest()


if __name__ == '__main__':
    print(deps_key(sys.argv[1]))
