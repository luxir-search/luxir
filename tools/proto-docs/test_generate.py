import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

from generate import INPUTS, ROOT, Reference, compile_descriptors


class ReferenceTest(unittest.TestCase):
    def test_protobuf_structure_and_comments(self):
        with tempfile.TemporaryDirectory(prefix="luxir-proto-docs-test-") as directory:
            root = Path(directory)
            (root / "api.proto").write_text('''syntax = "proto3";
package sample;
import "dependency.proto";
// A request with <markup>, a | pipe, and `backticks`.
message Request {
  // Leading comment.
  optional int32 limit = 1; // Trailing comment.
  map<string, Entry> entries = 2;
  repeated Entry rows = 3;
  // Choose one input.
  oneof input {
    string text = 4;
    bytes binary = 5;
  }
  // This is a real message, not a synthetic map entry.
  message Entry { string key = 1; }
  message Details { string name = 1; }
  Details details = 6;
  enum Mode { DEFAULT = 0; OTHER = 1; }
  Mode mode = 7;
  external.State state = 8;
}
service Indexer {
  // Send requests in both directions.
  rpc Update(stream Request) returns (stream Request);
}
''')
            (root / "dependency.proto").write_text('''syntax = "proto3";
package external;
// State from another file.
enum State { UNKNOWN = 0; READY = 1; }
message Unused {}
''')
            descriptors = compile_descriptors(os.environ.get("PROTOC", "protoc"), [root], ["api.proto"])
        reference = Reference(descriptors, ("api.proto",))
        text = reference.render()
        self.assertIn('id="message-sample.request.details"', text)
        self.assertIn('id="field-sample.request.details"', text)
        self.assertIn('map&lt;`string`, [`sample.Request.Entry`](#message-sample.request.entry)&gt;', text)
        self.assertIn('| 3 | [`sample.Request.Entry`](#message-sample.request.entry) | repeated |', text)
        self.assertNotIn('sample.Request.EntriesEntry', text)
        self.assertNotIn('oneof-sample.request._limit', text)
        self.assertIn('| 1 | `int32` | optional | Leading comment.<br><br>Trailing comment. |', text)
        self.assertIn('oneof [`input`](#oneof-sample.request.input)', text)
        self.assertIn('Choose one input.', text)
        self.assertIn('id="value-sample.request.mode.other"', text)
        self.assertIn('stream [`sample.Request`](#message-sample.request) | stream [`sample.Request`](#message-sample.request)', text)
        self.assertIn('id="value-external.state.ready"', text)
        self.assertNotIn('external.Unused', text)
        self.assertIn(r'A request with &lt;markup&gt;, a \| pipe, and \`backticks\`.', text)
        # Every generated cross-reference resolves, including imported symbols.
        targets = set(re.findall(r'id="([^"]+)"', text))
        self.assertTrue(all(target == target.lower() for target in targets))
        self.assertTrue(set(re.findall(r'\]\(#([^)]+)\)', text)) <= targets)
        self.assertEqual(text, reference.render())

    def test_check_mode_preserves_stale_output(self):
        # Exercise the real CLI in an isolated checkout, including path discovery.
        with tempfile.TemporaryDirectory(prefix="luxir-proto-docs-check-") as directory:
            root = Path(directory)
            script = root / "tools/proto-docs/generate.py"
            script.parent.mkdir(parents=True)
            shutil.copyfile(Path(__file__).with_name("generate.py"), script)
            (root / "protos").mkdir()
            for name in INPUTS:
                shutil.copyfile(ROOT / "protos" / name, root / "protos" / name)
            command = [sys.executable, str(script), "--protoc", os.environ.get("PROTOC", "protoc")]
            subprocess.run(command, cwd=root, check=True, capture_output=True)
            output = root / "docs/reference/protobuf.md"
            generated = output.read_bytes()
            output.write_bytes(generated + b"\nStale output.\n")
            stale = output.read_bytes()
            result = subprocess.run([*command, "--check"], cwd=root, capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn("Protobuf reference is stale", result.stderr)
            self.assertEqual(output.read_bytes(), stale)
            subprocess.run(command, cwd=root, check=True, capture_output=True)
            self.assertEqual(output.read_bytes(), generated)
            subprocess.run([*command, "--check"], cwd=root, check=True, capture_output=True)


if __name__ == "__main__":
    unittest.main()
