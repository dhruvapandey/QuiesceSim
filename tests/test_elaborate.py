import json
import tempfile
import unittest
from pathlib import Path

from quiescesim.elaborate import fingerprint_file, import_verilator_json, lower_resolved_module


class ElaborateImportTests(unittest.TestCase):
    def test_imports_and_ranks_resolved_modules(self):
        fixture = {
            "modulesp": [
                {"name": "small", "stmtsp": [], "type": "MODULE"},
                {"name": "hot", "stmtsp": [
                    {"type": "VAR", "name": "clk", "direction": "INPUT", "dtypeName": "logic", "isPrimaryClock": True},
                    {"type": "ALWAYS", "keyword": "always_ff", "loc": "fixture,1:1", "stmtsp": [{"type": "ASSIGN"}]},
                ], "type": "MODULE"},
            ]
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "design.json"
            path.write_text(json.dumps(fixture))
            design = import_verilator_json(path)
            manifest = Path(directory) / "manifest.json"
            design.write_manifest(manifest, source_json=path, source_fingerprint=fingerprint_file(path))
            saved = json.loads(manifest.read_text())
            lowered = lower_resolved_module(path, "hot")
        self.assertEqual(design.module_count, 2)
        self.assertEqual(design.ranked_kernel_candidates()[0].name, "hot")
        self.assertEqual(saved["schema"], "quiescesim-elaboration-manifest-v1")
        self.assertEqual(saved["module_count"], 2)
        self.assertEqual(saved["kernel_candidates"][0]["name"], "hot")
        self.assertEqual(lowered.variables[0]["name"], "clk")
        self.assertEqual(lowered.processes[0]["statement_kinds"]["ASSIGN"], 1)

    def test_collects_processes_inside_selected_generate_blocks(self):
        fixture = {"modulesp": [{"name": "generated", "type": "MODULE", "stmtsp": [
            {"type": "GENBLOCK", "name": "enabled_branch", "itemsp": [
                {"type": "ALWAYS", "keyword": "always_ff", "loc": "fixture,2:1",
                 "sentreep": [{"type": "SENTREE"}], "stmtsp": [{"type": "ASSIGNDLY"}]}
            ]}
        ]}]}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "generated.json"
            path.write_text(json.dumps(fixture))
            lowered = lower_resolved_module(path, "generated")
        self.assertEqual(len(lowered.processes), 1)
        self.assertEqual(lowered.processes[0]["keyword"], "always_ff")
        self.assertEqual(lowered.processes[0]["hierarchy_path"], ["enabled_branch", "ALWAYS"])
        self.assertEqual(lowered.processes[0]["statement_kinds"]["ASSIGNDLY"], 1)
