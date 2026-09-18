# Documentation

The Markdown files in `docs/` are shared by GitHub and the Luxir website.
Use relative links between pages. `docs/README.md` defines the website's
sidebar groups and page order.

## Protobuf API reference

[The API reference](../reference/protobuf.md) is generated from
`protos/luxir.proto` and `protos/luxir_types.proto`, including their comments.
Edit those sources, then regenerate the checked-in Markdown. The internal
index format is excluded.

The generator uses `protoc` descriptors, including source locations and
comments, and the Python protobuf runtime. It does not require a Luxir build.
Install its Python dependency in a virtual environment:

```bash
python3 -m venv tools/proto-docs/.venv
tools/proto-docs/.venv/bin/pip install -r tools/proto-docs/requirements.txt
```

Use `protoc` 33.4 (the version used by CI), either on `PATH` or selected with
`--protoc`. For the native GCC/vcpkg environment:

```bash
tools/proto-docs/.venv/bin/python tools/proto-docs/generate.py \
  --protoc /opt/vcpkg/installed-native/x64-linux-luxir-native/tools/protobuf/protoc
```

For other presets, use `Protobuf_PROTOC_EXECUTABLE` from that build's
`CMakeCache.txt`. The generator finds the compiler installation's well-known
types; `-I` adds another import directory when needed. `PROTOC` can also select
the executable.

To check for stale output without rewriting it, add `--check`. CI runs this
check and the generator tests whenever its sources or generated page change.
Run the tests locally with the same environment:

```bash
PROTOC=/path/to/protoc tools/proto-docs/.venv/bin/python \
  -m unittest discover -s tools/proto-docs -p 'test_*.py'
```

Links use a symbol-kind prefix and the lowercase fully qualified name:

```markdown
[UpdateRequest](../reference/protobuf.md#message-luxir.updaterequest)
[allow_dups](../reference/protobuf.md#field-luxir.updaterequest.allow_dups)
[Update RPC](../reference/protobuf.md#method-luxir.indexer.update)
```

Messages, fields, enums, enum values, oneof groups, and methods each have an
anchor. The prefixes keep `KnnQuery.Ivf` (a message) and `KnnQuery.ivf` (a field)
distinct even though GitHub lowercases HTML anchors.
Descriptions are rendered as plain text; application defaults belong in the
source comments, since protobuf scalar defaults do not express them.

Put multiline descriptions before their declarations. A trailing `//` comment
can document a field on the same line, but continuation lines may attach to
the next declaration or be dropped by `protoc`.

## Website checks

With the website checkout next to Luxir, validate links and anchors against
the local docs with:

```bash
../luxir-site/site.sh check
```

The site importer recognizes Markdown headings and explicit HTML anchors,
including the anchors in generated field tables.
