"""Strict bootstrap code generator from elaborated Verilator XML to native IR.

This is a transitional compiler stage: it consumes resolved XML and emits C++
that constructs QuiesceSim-owned ``ModuleIR``. It does not emit calls into
Verilator's runtime and it intentionally rejects any node whose semantics have
not been implemented in the native IR.
"""

from __future__ import annotations

import html
from pathlib import Path
import re
import xml.etree.ElementTree as ET


class UnsupportedResolvedNode(ValueError):
    """The module cannot yet be lowered without losing semantics."""


def _widths(root: ET.Element) -> dict[str, int]:
    result: dict[str, int] = {}
    for dtype in root.findall(".//typetable/*"):
        dtype_id = dtype.attrib.get("id")
        if dtype_id is None:
            continue
        if "left" in dtype.attrib and "right" in dtype.attrib:
            result[dtype_id] = abs(int(dtype.attrib["left"]) - int(dtype.attrib["right"])) + 1
        elif dtype.tag == "basicdtype":
            result[dtype_id] = 1
    return result


def _constant(node: ET.Element, widths: dict[str, int]) -> str:
    value = html.unescape(node.attrib["name"])
    match = re.fullmatch(r"(\d+)'[sS]?[hH]([0-9a-fA-F_]+)", value)
    if match:
        width, bits = int(match.group(1)), int(match.group(2).replace("_", ""), 16)
    else:
        match = re.fullmatch(r"(\d+)'[sS]?[bB]([01_]+)", value)
        if not match:
            raise UnsupportedResolvedNode(f"unsupported resolved constant: {value}")
        width, bits = int(match.group(1)), int(match.group(2).replace("_", ""), 2)
    if width > 64:
        raise UnsupportedResolvedNode(f"constant wider than native prototype: {width}")
    return f"Expr::constant(LogicWord::known(0x{bits:x}ULL, {width}))"


def _expr(node: ET.Element, widths: dict[str, int]) -> str:
    tag = node.tag
    children = list(node)
    if tag == "varref":
        return f'Expr::variable("{node.attrib["name"]}")'
    if tag == "const":
        return _constant(node, widths)
    binary = {
        "add": "add", "sub": "subtract", "and": "bit_and", "or": "bit_or",
        "xor": "bit_xor", "eq": "equal", "neq": "not_equal",
        "lt": "less_than_unsigned", "gte": "greater_equal_unsigned",
        "shiftr": "shift_right_logical", "shiftl": "shift_left_logical",
    }
    if tag in binary:
        if len(children) != 2:
            raise UnsupportedResolvedNode(f"{tag} expected two operands")
        return f"Expr::binary(ExprKind::{binary[tag]}, {_expr(children[0], widths)}, {_expr(children[1], widths)})"
    if tag == "not":
        if len(children) != 1:
            raise UnsupportedResolvedNode("not expected one operand")
        return f"Expr::unary(ExprKind::logical_not, {_expr(children[0], widths)})"
    if tag == "concat":
        if len(children) != 2:
            raise UnsupportedResolvedNode("concat expected two operands")
        return f"Expr::concat({_expr(children[0], widths)}, {_expr(children[1], widths)})"
    if tag == "sel":
        if len(children) != 2 or "widthConst" not in node.attrib:
            raise UnsupportedResolvedNode("only constant-width resolved selects are supported")
        offset = children[1]
        if offset.tag != "const":
            raise UnsupportedResolvedNode("only constant-offset resolved selects are supported")
        offset_value = html.unescape(offset.attrib["name"])
        offset_match = re.fullmatch(r"\d+'[sS]?[hH]([0-9a-fA-F_]+)", offset_value)
        if not offset_match:
            raise UnsupportedResolvedNode(f"unsupported select offset: {offset_value}")
        lsb = int(offset_match.group(1).replace("_", ""), 16)
        width = int(node.attrib["widthConst"])
        return f"Expr::slice({_expr(children[0], widths)}, {lsb + width - 1}, {lsb})"
    raise UnsupportedResolvedNode(f"unsupported resolved expression node: {tag}")


def emit_cpp_module(xml_path: Path, module_name: str, function_name: str) -> str:
    """Return C++ that constructs a strictly supported resolved module IR.

    Phase 0 deliberately handles continuous assignments only. Sequential and
    procedural lowering will be added after this generated IR is compiled and
    differentially exercised through the native runtime.
    """
    root = ET.parse(xml_path).getroot()
    widths = _widths(root)
    module = next((item for item in root.findall(".//module") if item.attrib.get("name") == module_name), None)
    if module is None:
        raise ValueError(f"resolved XML module not found: {module_name}")
    signals: list[str] = []
    for variable in module.findall("var"):
        if variable.attrib.get("param") == "true" or variable.attrib.get("localparam") == "true":
            continue
        width = widths.get(variable.attrib.get("dtype_id", ""))
        if width is None or width > 64:
            raise UnsupportedResolvedNode(f"unsupported width for signal {variable.attrib['name']}")
        signals.append(f'{{"{variable.attrib["name"]}", {width}, LogicWord::x({width})}}')
    processes: list[str] = []
    for always in module.findall(".//always"):
        if always.find("sentree") is not None:
            raise UnsupportedResolvedNode("sequential/process lowering is not implemented yet")
        contassign = always.find("contassign")
        if contassign is None or len(list(contassign)) != 2:
            raise UnsupportedResolvedNode("only single resolved continuous assignments are supported")
        expression, target = list(contassign)
        if target.tag != "varref":
            raise UnsupportedResolvedNode("continuous assignment target must be a whole signal")
        processes.append(
            '{"generated:cont", ProcessKind::combinational, "", "", '
            f'{{{{"{target.attrib["name"]}", {_expr(expression, widths)}, std::nullopt}}}}, {{}}}}'
        )
    return "\n".join([
        '#include "quiescesim/ir.hpp"',
        '',
        'namespace quiescesim {',
        f'ModuleIR {function_name}() {{',
        f'  return {{"{module_name}", {{{", ".join(signals)}}}, {{{", ".join(processes)}}}}};',
        '}',
        '}  // namespace quiescesim',
        '',
    ])
