import json
import tempfile
import unittest
from pathlib import Path

from quiescesim.elaborate import fingerprint_file, import_verilator_json, lower_resolved_module, lower_resolved_xml_module
from quiescesim.native_codegen import UnsupportedResolvedNode, emit_cpp_module


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

    def test_xml_lowering_retains_resolved_packed_widths(self):
        fixture = """<verilator_xml><netlist><typetable>
          <basicdtype id=\"1\" name=\"logic\"/>
          <basicdtype id=\"8\" name=\"logic\" left=\"7\" right=\"0\"/>
        </typetable><module name=\"counter\">
          <var name=\"clk\" dtype_id=\"1\" dir=\"input\" vartype=\"logic\"/>
          <var name=\"q\" dtype_id=\"8\" dir=\"output\" vartype=\"logic\"/>
          <always loc=\"fixture,1,1\"><assign><const name=\"8'h0\"/></assign></always>
        </module></netlist></verilator_xml>"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "counter.xml"
            path.write_text(fixture)
            lowered = lower_resolved_xml_module(path, "counter")
        self.assertEqual(lowered.variables[0]["width"], 1)
        self.assertEqual(lowered.variables[1]["width"], 8)
        self.assertEqual(lowered.processes[0]["statement_kinds"]["ASSIGN"], 1)

    def test_xml_lowering_describes_unpacked_arrays_as_memories(self):
        fixture = """<verilator_xml><netlist><typetable>
          <basicdtype id=\"8\" name=\"logic\" left=\"7\" right=\"0\"/>
          <unpackarraydtype id=\"9\" sub_dtype_id=\"8\"><range><const name=\"32'h0\"/><const name=\"32'h3\"/></range></unpackarraydtype>
        </typetable><module name=\"array_port\"><var name=\"words\" dtype_id=\"9\" dir=\"input\" vartype=\"port\"/></module></netlist></verilator_xml>"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "array.xml"
            path.write_text(fixture)
            lowered = lower_resolved_xml_module(path, "array_port")
        self.assertEqual(lowered.variables[0]["storage"], "memory")
        self.assertEqual(lowered.variables[0]["element_width"], 8)
        self.assertEqual(lowered.variables[0]["depth"], 4)

    def test_native_codegen_emits_supported_resolved_continuous_assignment(self):
        fixture = """<verilator_xml><netlist><typetable>
          <basicdtype id=\"8\" name=\"logic\" left=\"7\" right=\"0\"/>
        </typetable><module name=\"adder\">
          <var name=\"a\" dtype_id=\"8\"/><var name=\"b\" dtype_id=\"8\"/><var name=\"y\" dtype_id=\"8\"/>
          <always><contassign><add><varref name=\"a\"/><varref name=\"b\"/></add><varref name=\"y\"/></contassign></always>
        </module></netlist></verilator_xml>"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "adder.xml"
            path.write_text(fixture)
            generated = emit_cpp_module(path, "adder", "generated_adder_ir")
        self.assertIn("ExprKind::add", generated)
        self.assertIn("generated_adder_ir", generated)

    def test_native_codegen_resolves_reference_dtype_width(self):
        fixture = """<verilator_xml><netlist><typetable>
          <basicdtype id=\"3\" name=\"logic\" left=\"2\" right=\"0\"/><refdtype id=\"4\" sub_dtype_id=\"3\"/>
        </typetable><module name=\"aliased\"><var name=\"opcode\" dtype_id=\"4\"/></module></netlist></verilator_xml>"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "aliased.xml"
            path.write_text(fixture)
            generated = emit_cpp_module(path, "aliased", "generated_alias_ir")
        self.assertIn('{"opcode", 3, LogicWord::x(3)}', generated)

    def test_native_codegen_expands_whole_array_copy_to_memory_writes(self):
        fixture = """<verilator_xml><netlist><typetable>
          <basicdtype id=\"8\" name=\"logic\" left=\"7\" right=\"0\"/>
          <unpackarraydtype id=\"9\" sub_dtype_id=\"8\"><range><const name=\"32'h0\"/><const name=\"32'h1\"/></range></unpackarraydtype>
        </typetable><module name=\"copy\"><var name=\"source\" dtype_id=\"9\"/><var name=\"target\" dtype_id=\"9\"/>
          <always><contassign><varref name=\"source\"/><varref name=\"target\"/></contassign></always>
        </module></netlist></verilator_xml>"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "copy.xml"
            path.write_text(fixture)
            generated = emit_cpp_module(path, "copy", "generated_copy_ir")
        self.assertEqual(generated.count('Expr::memory_read("source"'), 2)
        self.assertIn('"generated:array-copy:0"', generated)

    def test_native_codegen_lowers_case_with_case_equality_and_default(self):
        fixture = """<verilator_xml><netlist><typetable><basicdtype id=\"2\" name=\"logic\" left=\"1\" right=\"0\"/></typetable>
        <module name=\"decode\"><var name=\"op\" dtype_id=\"2\"/><var name=\"y\" dtype_id=\"2\"/>
          <always><begin><case><varref name=\"op\"/><caseitem><const name=\"2'h0\"/><assign><const name=\"2'h1\"/><varref name=\"y\"/></assign></caseitem><caseitem><begin><assign><const name=\"2'h2\"/><varref name=\"y\"/></assign></begin></caseitem></case></begin></always>
        </module></netlist></verilator_xml>"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "decode.xml"
            path.write_text(fixture)
            generated = emit_cpp_module(path, "decode", "generated_decode_ir")
        self.assertIn("ExprKind::case_equal", generated)
        self.assertIn("ExprKind::logical_not", generated)

    def test_native_codegen_retains_resolved_unique_case_multiple_match_guard(self):
        fixture = """<verilator_xml><netlist><typetable><basicdtype id="1" name="logic"/><basicdtype id="2" name="logic" left="1" right="0"/></typetable>
        <module name="decode"><var name="op" dtype_id="2"/><var name="y" dtype_id="1"/>
          <always><begin><case><varref name="op"/>
            <caseitem><const name="2'h0"/><assign><const name="1'h1"/><varref name="y"/></assign></caseitem>
            <caseitem><assign><const name="1'h0"/><varref name="y"/></assign></caseitem>
            <if><not><onehot><concat><eq><const name="2'h0"/><varref name="op"/></eq><eq><const name="2'h1"/><varref name="op"/></eq></concat></onehot></not>
              <begin><if><neq><const name="2'h0"/><concat><eq><const name="2'h0"/><varref name="op"/></eq><eq><const name="2'h1"/><varref name="op"/></eq></concat></neq>
                <begin><display/><stop/></begin></if></begin></if>
          </case></begin></always>
        </module></netlist></verilator_xml>"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "decode.xml"
            path.write_text(fixture)
            generated = emit_cpp_module(path, "decode", "generated_decode_ir")
        self.assertIn("ExprKind::onehot", generated)
        self.assertIn("resolved unique case has multiple matching items", generated)

    def test_native_codegen_lowers_procedural_constant_select_target(self):
        fixture = """<verilator_xml><netlist><typetable><basicdtype id="2" name="logic" left="7" right="0"/><basicdtype id="1" name="logic" left="1" right="0"/></typetable>
        <module name="slice_write"><var name="input" dtype_id="1"/><var name="output" dtype_id="2"/>
          <always><begin><assign><varref name="input"/><sel widthConst="2"><varref name="output"/><const name="32'h2"/></sel></assign></begin></always>
        </module></netlist></verilator_xml>"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "slice_write.xml"
            path.write_text(fixture)
            generated = emit_cpp_module(path, "slice_write", "generated_slice_write_ir")
        self.assertIn("{3, 2}", generated)

    def test_native_codegen_lowers_comb_and_canonical_async_reset(self):
        fixture = """<verilator_xml><netlist><typetable><basicdtype id=\"1\" name=\"logic\"/><basicdtype id=\"8\" name=\"logic\" left=\"7\" right=\"0\"/></typetable>
        <module name=\"flop\"><var name=\"clk\" dtype_id=\"1\"/><var name=\"rst_n\" dtype_id=\"1\"/><var name=\"d\" dtype_id=\"8\"/><var name=\"q\" dtype_id=\"8\"/>
        <always><begin><assign><varref name=\"d\"/><varref name=\"q\"/></assign></begin></always>
        <always><sentree><senitem edgeType=\"POS\"><varref name=\"clk\"/></senitem><senitem edgeType=\"NEG\"><varref name=\"rst_n\"/></senitem></sentree><begin><if><varref name=\"rst_n\"/><begin><assigndly><varref name=\"d\"/><varref name=\"q\"/></assigndly></begin><begin><assigndly><const name=\"8'h0\"/><varref name=\"q\"/></assigndly></begin></if></begin></always>
        </module></netlist></verilator_xml>"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "flop.xml"
            path.write_text(fixture)
            generated = emit_cpp_module(path, "flop", "generated_flop_ir")
        self.assertIn("ProcessKind::combinational", generated)
        self.assertIn("ProcessKind::posedge_or_negedge_reset", generated)
