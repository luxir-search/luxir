#!/usr/bin/env python3
"""Generate the public API reference from protoc descriptors and source comments."""

import argparse
import difflib
import html
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap

from google.protobuf import descriptor_pb2 as pb


ROOT = Path(__file__).resolve().parents[2]
INPUTS = ("luxir.proto", "luxir_types.proto")
OUTPUT = ROOT / "docs/reference/protobuf.md"
SYMBOL_KINDS = {pb.DescriptorProto: "message", pb.EnumDescriptorProto: "enum", pb.ServiceDescriptorProto: "service"}


def prose(text):
    """Comments are plain text; preserve paragraphs without admitting HTML/Markdown."""
    paragraphs = re.split(r"\n\s*\n", text.strip())
    return "<br><br>".join(
        re.sub(r"([\\`*_\[\]|])", r"\\\1", html.escape(" ".join(p.split()), quote=False))
        for p in paragraphs
    )


class Reference:
    def __init__(self, descriptors, inputs=INPUTS):
        self.inputs = inputs
        self.messages = {}
        self.symbols = {}
        self.locations = {}
        self.anchors = set()
        for file in descriptors.file:
            self.locations[file.name] = {tuple(loc.path): loc for loc in file.source_code_info.location}
            self.collect(file, file.package, (), file.message_type, 4)
            for i, enum in enumerate(file.enum_type):
                self.symbols[f"{file.package}.{enum.name}"] = (file, enum, (5, i))
            for i, service in enumerate(file.service):
                self.symbols[f"{file.package}.{service.name}"] = (file, service, (6, i))

    def collect(self, file, parent, path, messages, field_number):
        for i, message in enumerate(messages):
            name = f"{parent}.{message.name}"
            location = (*path, field_number, i)
            self.messages[name] = message
            if not message.options.map_entry:
                self.symbols[name] = (file, message, location)
            self.collect(file, name, location, message.nested_type, 3)
            for j, enum in enumerate(message.enum_type):
                self.symbols[f"{name}.{enum.name}"] = (file, enum, (*location, 4, j))

    def description(self, file, path):
        loc = self.locations[file.name].get(path)
        if loc is None:
            return ""
        return prose("\n\n".join(
            textwrap.dedent(comment).strip()
            for comment in (loc.leading_comments, loc.trailing_comments) if comment.strip()
        ))

    def anchor(self, name, kind=None):
        if kind is None:
            kind = SYMBOL_KINDS[type(self.symbols[name][1])]
        # GitHub lowercases HTML IDs. The kind distinguishes a nested message
        # such as KnnQuery.Ivf from the containing message's ivf field.
        return f"{kind}-{name}".lower()

    def target(self, name, kind=None):
        identifier = self.anchor(name, kind)
        if identifier in self.anchors:
            raise ValueError(f"Duplicate documentation anchor: {identifier}")
        self.anchors.add(identifier)
        return f'<a id="{identifier}"></a>'

    def link(self, name, label=None):
        name = name.lstrip(".")
        label = label or name.removeprefix("luxir.")
        file, _, _ = self.symbols[name]
        if file.name not in self.inputs:
            # Document the imported type alongside our enums/messages so its
            # definition is available without publishing the rest of its file.
            self.external.add(name)
        return f"[`{label}`](#{self.anchor(name)})"

    def field_type(self, field):
        if field.type_name:
            message = self.messages.get(field.type_name.lstrip("."))
            if message is not None and message.options.map_entry:
                key, value = message.field
                return f"map&lt;{self.field_type(key)}, {self.field_type(value)}&gt;"
            return self.link(field.type_name)
        return f"`{pb.FieldDescriptorProto.Type.Name(field.type).removeprefix('TYPE_').lower()}`"

    def render_symbol(self, name):
        file, symbol, path = self.symbols[name]
        out = [self.target(name), "", f"### {name}", ""]
        comment = self.description(file, path)
        if comment:
            out += [comment, ""]
        if file.name in self.inputs:
            out += [f"[Source](../../protos/{file.name})", ""]

        if isinstance(symbol, pb.DescriptorProto):
            groups = {f.oneof_index for f in symbol.field if f.HasField("oneof_index") and not f.proto3_optional}
            for i in sorted(groups):
                group = symbol.oneof_decl[i]
                out += [self.target(f"{name}.{group.name}", "oneof"), "",
                        f"**oneof `{group.name}`:** at most one member can be set.", ""]
                comment = self.description(file, (*path, 8, i))
                if comment:
                    out += [comment, ""]
            if symbol.field:
                out += ["| Field | Number | Type | Cardinality / group | Description |",
                        "|---|---|---|---|---|"]
            for i, field in enumerate(symbol.field):
                full_name = f"{name}.{field.name}"
                label = "singular"
                message = self.messages.get(field.type_name.lstrip("."))
                if message is not None and message.options.map_entry:
                    label = "map"
                elif field.proto3_optional:
                    label = "optional"
                elif field.HasField("oneof_index"):
                    group = symbol.oneof_decl[field.oneof_index].name
                    label = f"oneof [`{group}`](#{self.anchor(f'{name}.{group}', 'oneof')})"
                elif field.label == pb.FieldDescriptorProto.LABEL_REPEATED:
                    label = "repeated"
                description = self.description(file, (*path, 2, i))
                if field.options.deprecated:
                    description = "**Deprecated.** " + description
                out.append(f"| {self.target(full_name, 'field')}[`{field.name}`](#{self.anchor(full_name, 'field')}) | "
                           f"{field.number} | {self.field_type(field)} | {label} | {description} |")
        elif isinstance(symbol, pb.EnumDescriptorProto):
            out += ["| Value | Number | Description |", "|---|---|---|"]
            for i, value in enumerate(symbol.value):
                full_name = f"{name}.{value.name}"
                out.append(f"| {self.target(full_name, 'value')}[`{value.name}`](#{self.anchor(full_name, 'value')}) | "
                           f"{value.number} | {self.description(file, (*path, 2, i))} |")
        elif isinstance(symbol, pb.ServiceDescriptorProto):
            out += ["| Method | Request | Response | Description |", "|---|---|---|---|"]
            for i, method in enumerate(symbol.method):
                full_name = f"{name}.{method.name}"
                request = ("stream " if method.client_streaming else "") + self.link(method.input_type)
                response = ("stream " if method.server_streaming else "") + self.link(method.output_type)
                out.append(f"| {self.target(full_name, 'method')}[`{method.name}`](#{self.anchor(full_name, 'method')}) | "
                           f"{request} | {response} | {self.description(file, (*path, 2, i))} |")
        return "\n".join(out).rstrip() + "\n"

    def render(self):
        self.anchors.clear()
        self.external = set()
        out = ["""# Protobuf API reference

Messages, fields, enums, and services shared by Luxir's HTTP and gRPC APIs.
See the [gRPC guide](../guide/grpc.md) for client generation and streaming,
and [HTTP JSON mappings](../guide/grpc.md#http-json-versus-protobuf-values)
for the HTTP shorthands.

<!-- Generated by tools/proto-docs/generate.py. Edit the .proto comments, then regenerate. -->

Field descriptions come from the protobuf source comments. `optional` marks
explicit field presence; `oneof` members share a single choice. Application
defaults and constraints are described in the comments and guides.
"""]
        for title, kind in (("Services", pb.ServiceDescriptorProto),
                            ("Messages", pb.DescriptorProto), ("Enums", pb.EnumDescriptorProto)):
            names = sorted(name for name, (file, symbol, _) in self.symbols.items()
                           if file.name in self.inputs and isinstance(symbol, kind))
            out += [f"## {title}\n", "\n".join(f"- {self.link(name)}" for name in names) + "\n"]
            out += [self.render_symbol(name) for name in names]
        if self.external:
            out += ["## Imported types\n"]
            rendered = set()
            while self.external - rendered:
                name = min(self.external - rendered)
                out.append(self.render_symbol(name))
                rendered.add(name)
        return "\n".join(out)


def compile_descriptors(protoc, proto_paths, inputs):
    with tempfile.TemporaryDirectory(prefix="luxir-proto-docs-") as directory:
        output = Path(directory) / "api.pb"
        subprocess.run([protoc, *(f"-I{p}" for p in proto_paths),
                        "--include_imports", "--include_source_info",
                        f"--descriptor_set_out={output}", *inputs], check=True)
        return pb.FileDescriptorSet.FromString(output.read_bytes())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="check for stale output without writing files")
    parser.add_argument("--protoc", default=os.environ.get("PROTOC", "protoc"), help="protoc executable (or set PROTOC)")
    parser.add_argument("-I", "--proto-path", action="append", default=[], help="additional protobuf import directory")
    args = parser.parse_args()
    executable = shutil.which(args.protoc)
    if executable is None:
        parser.error("protoc not found; use --protoc or set PROTOC to your toolchain's executable")
    paths = [ROOT / "protos", *map(Path, args.proto_path)]
    # Both a standard protoc installation and vcpkg bundle the well-known types.
    for parent in Path(executable).resolve().parents:
        include = parent / "include"
        if (include / "google/protobuf/struct.proto").is_file():
            paths.append(include)
            break
    generated = Reference(compile_descriptors(executable, paths, INPUTS)).render()
    current = OUTPUT.read_text() if OUTPUT.exists() else ""
    if args.check:
        if current != generated:
            sys.stdout.writelines(difflib.unified_diff(current.splitlines(True), generated.splitlines(True),
                                  fromfile=str(OUTPUT.relative_to(ROOT)), tofile="regenerated reference"))
            print("Protobuf reference is stale. Run tools/proto-docs/generate.py.", file=sys.stderr)
            return 1
        print("Protobuf reference is up to date.")
    else:
        OUTPUT.parent.mkdir(parents=True, exist_ok=True)
        OUTPUT.write_text(generated)
        print(f"Generated {OUTPUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
