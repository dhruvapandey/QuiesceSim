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


def _and_guard(existing: str | None, condition: str) -> str:
    return condition if existing is None else f"Expr::binary(ExprKind::bit_and, {existing}, {condition})"


def _not_guard(condition: str) -> str:
    return f"Expr::unary(ExprKind::logical_not, {condition})"


def _flatten_assignments(node: ET.Element, widths: dict[str, int], guard: str | None = None) -> list[tuple[str, str, str | None]]:
    """Flatten resolved begin/if trees while preserving statement order."""
    if node.tag == "begin":
        result: list[tuple[str, str, str | None]] = []
        for child in node:
            result.extend(_flatten_assignments(child, widths, guard))
        return result
    if node.tag == "if":
        children = list(node)
        if len(children) not in (2, 3):
            raise UnsupportedResolvedNode("resolved if statement has unsupported shape")
        condition = _expr(children[0], widths)
        result = _flatten_assignments(children[1], widths, _and_guard(guard, condition))
        if len(children) == 3:
            result.extend(_flatten_assignments(children[2], widths, _and_guard(guard, _not_guard(condition))))
        return result
    if node.tag in ("assign", "assigndly"):
        children = list(node)
        if len(children) != 2 or children[1].tag != "varref":
            raise UnsupportedResolvedNode("procedural assignment target must be a whole resolved signal")
        return [(children[1].attrib["name"], _expr(children[0], widths), guard)]
    raise UnsupportedResolvedNode(f"unsupported resolved statement node: {node.tag}")


def _assignment_cpp(assignment: tuple[str, str, str | None]) -> str:
    target, expression, guard = assignment
    return f'{{"{target}", {expression}, {guard or "std::nullopt"}}}'


def _canonical_async_reset(always: ET.Element, widths: dict[str, int]) -> tuple[str, str, list[tuple[str, str, str | None]], list[tuple[str, str, str | None]]] | None:
    """Recognize ``if (rst_n) normal; else reset;`` after XML resolution."""
    sentree = always.find("sentree")
    if sentree is None:
        return None
    items = sentree.findall("senitem")
    if len(items) != 2 or items[0].attrib.get("edgeType") != "POS" or items[1].attrib.get("edgeType") != "NEG":
        raise UnsupportedResolvedNode("only posedge/negedge asynchronous-reset sensitivity is supported")
    clock_ref = items[0].find("varref")
    reset_ref = items[1].find("varref")
    if clock_ref is None or reset_ref is None:
        raise UnsupportedResolvedNode("clock/reset sensitivity must name signals")
    body = next((child for child in always if child.tag != "sentree"), None)
    if body is None or body.tag != "begin" or len(list(body)) != 1 or list(body)[0].tag != "if":
        raise UnsupportedResolvedNode("asynchronous-reset process must have one top-level if")
    reset_if = list(body)[0]
    branches = list(reset_if)
    if len(branches) != 3 or branches[0].tag != "varref" or branches[0].attrib.get("name") != reset_ref.attrib["name"]:
        raise UnsupportedResolvedNode("asynchronous-reset process must test its reset signal directly")
    return (clock_ref.attrib["name"], reset_ref.attrib["name"],
            _flatten_assignments(branches[1], widths), _flatten_assignments(branches[2], widths))


def emit_cpp_module(xml_path: Path, module_name: str, function_name: str) -> str:
    """Return C++ that constructs a strictly supported resolved module IR.

    The supported procedural subset is deliberately narrow: ordered blocking
    assignments in ``always_comb`` and canonical active-low asynchronous-reset
    flip-flop blocks. All other structures fail loudly.
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
    for index, always in enumerate(module.findall(".//always")):
        contassign = always.find("contassign")
        if contassign is not None:
            if len(list(contassign)) != 2:
                raise UnsupportedResolvedNode("continuous assignment has unsupported shape")
            expression, target = list(contassign)
            if target.tag != "varref":
                raise UnsupportedResolvedNode("continuous assignment target must be a whole signal")
            processes.append(
                f'{{"generated:cont:{index}", ProcessKind::combinational, "", "", '
                f'{{{{"{target.attrib["name"]}", {_expr(expression, widths)}, std::nullopt}}}}, {{}}}}'
            )
            continue
        async_reset = _canonical_async_reset(always, widths)
        if async_reset is not None:
            clock, reset, normal, reset_assignments = async_reset
            processes.append(
                f'{{"generated:ff:{index}", ProcessKind::posedge_or_negedge_reset, "{clock}", "{reset}", '
                f'{{{", ".join(_assignment_cpp(item) for item in normal)}}}, '
                f'{{{", ".join(_assignment_cpp(item) for item in reset_assignments)}}}}}'
            )
            continue
        if always.find("sentree") is not None:
            raise UnsupportedResolvedNode("unsupported sequential sensitivity/process shape")
        body = next(iter(always), None)
        if body is None:
            raise UnsupportedResolvedNode("empty resolved procedural process")
        assignments = _flatten_assignments(body, widths)
        processes.append(
            f'{{"generated:comb:{index}", ProcessKind::combinational, "", "", '
            f'{{{", ".join(_assignment_cpp(item) for item in assignments)}}}, {{}}}}'
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
