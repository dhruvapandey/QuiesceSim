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
    types = {node.attrib["id"]: node for node in root.findall(".//typetable/*") if "id" in node.attrib}
    result: dict[str, int] = {}

    def resolve(dtype_id: str) -> int | None:
        if dtype_id in result:
            return result[dtype_id]
        node = types.get(dtype_id)
        if node is None or node.tag == "unpackarraydtype":
            return None
        if "left" in node.attrib and "right" in node.attrib:
            result[dtype_id] = abs(int(node.attrib["left"]) - int(node.attrib["right"])) + 1
            return result[dtype_id]
        if "sub_dtype_id" in node.attrib:
            width = resolve(node.attrib["sub_dtype_id"])
            if width is not None:
                result[dtype_id] = width
            return width
        if node.tag == "basicdtype":
            result[dtype_id] = 1
            return 1
        return None

    for dtype_id in types:
        resolve(dtype_id)
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


def _array_shapes(root: ET.Element, widths: dict[str, int]) -> dict[str, tuple[int, int]]:
    result: dict[str, tuple[int, int]] = {}
    for dtype in root.findall(".//typetable/unpackarraydtype"):
        element_width = widths.get(dtype.attrib.get("sub_dtype_id", ""))
        bounds = dtype.findall("range/const")
        if element_width is None or len(bounds) != 2:
            continue
        try:
            values = [int(html.unescape(item.attrib["name"]).split("'h", 1)[1], 16) for item in bounds]
        except (KeyError, IndexError, ValueError):
            continue
        result[dtype.attrib["id"]] = (element_width, abs(values[0] - values[1]) + 1)
    return result


def _expr(node: ET.Element, widths: dict[str, int], arrays: dict[str, tuple[int, int]] | None = None) -> str:
    arrays = arrays or {}
    tag = node.tag
    children = list(node)
    if tag == "varref":
        if node.attrib["name"] in arrays:
            raise UnsupportedResolvedNode(f"whole-array expression requires explicit copy lowering: {node.attrib['name']}")
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
        return f"Expr::binary(ExprKind::{binary[tag]}, {_expr(children[0], widths, arrays)}, {_expr(children[1], widths, arrays)})"
    if tag == "not":
        if len(children) != 1:
            raise UnsupportedResolvedNode("not expected one operand")
        return f"Expr::unary(ExprKind::logical_not, {_expr(children[0], widths, arrays)})"
    if tag == "onehot":
        if len(children) != 1:
            raise UnsupportedResolvedNode("onehot expected one operand")
        return f"Expr::unary(ExprKind::onehot, {_expr(children[0], widths, arrays)})"
    if tag == "cond":
        if len(children) != 3:
            raise UnsupportedResolvedNode("cond expected selector and two branches")
        return f"Expr::mux({_expr(children[0], widths, arrays)}, {_expr(children[1], widths, arrays)}, {_expr(children[2], widths, arrays)})"
    if tag == "concat":
        if len(children) != 2:
            raise UnsupportedResolvedNode("concat expected two operands")
        return f"Expr::concat({_expr(children[0], widths, arrays)}, {_expr(children[1], widths, arrays)})"
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
        return f"Expr::slice({_expr(children[0], widths, arrays)}, {lsb + width - 1}, {lsb})"
    if tag == "arraysel":
        if len(children) != 2 or children[0].tag != "varref" or children[0].attrib.get("name") not in arrays:
            raise UnsupportedResolvedNode("array select must read a known unpacked-array binding")
        return f'Expr::memory_read("{children[0].attrib["name"]}", {_expr(children[1], widths, arrays)})'
    raise UnsupportedResolvedNode(f"unsupported resolved expression node: {tag}")


def _and_guard(existing: str | None, condition: str) -> str:
    return condition if existing is None else f"Expr::binary(ExprKind::bit_and, {existing}, {condition})"


def _not_guard(condition: str) -> str:
    return f"Expr::unary(ExprKind::logical_not, {condition})"


def _or_guard(conditions: list[str]) -> str:
    if not conditions:
        return "Expr::constant(LogicWord::known(0, 1))"
    result = conditions[0]
    for condition in conditions[1:]:
        result = f"Expr::binary(ExprKind::bit_or, {result}, {condition})"
    return result


def _target(node: ET.Element) -> tuple[str, str]:
    """Return the whole-signal target name and optional IR slice initializer."""
    if node.tag == "varref":
        return node.attrib["name"], ""
    if node.tag == "sel":
        children = list(node)
        if len(children) != 2 or children[0].tag != "varref" or children[1].tag != "const" or "widthConst" not in node.attrib:
            raise UnsupportedResolvedNode("assignment target must be a whole signal or constant select")
        text = html.unescape(children[1].attrib.get("name", ""))
        match = re.fullmatch(r"\d+'[sS]?[hH]([0-9a-fA-F_]+)", text)
        if not match:
            raise UnsupportedResolvedNode(f"unsupported assignment slice offset: {text}")
        lsb = int(match.group(1).replace("_", ""), 16)
        width = int(node.attrib["widthConst"])
        return children[0].attrib["name"], f", {{{{{lsb + width - 1}, {lsb}}}}}"
    raise UnsupportedResolvedNode("assignment target must be a whole signal or constant select")


def _unique_case_failure(node: ET.Element, widths: dict[str, int], arrays: dict[str, tuple[int, int]]) -> str | None:
    """Lower Verilator's resolved unique-case multiple-match stop into an IR guard.

    The resolved AST retains a synthesis-independent runtime check after the
    case items: ``!onehot(matches)`` followed by ``matches != 0``.  Together
    those predicates mean *more than one item matched*.  We preserve that
    predicate as an assertion instead of silently dropping compiler-inserted
    safety code.
    """
    if node.tag != "if" or len(list(node)) != 2:
        return None
    outer_condition, outer_body = list(node)
    if outer_body.tag != "begin" or len(list(outer_body)) != 1:
        return None
    inner = list(outer_body)[0]
    if inner.tag != "if" or len(list(inner)) != 2:
        return None
    inner_condition, inner_body = list(inner)
    if inner_body.tag != "begin" or not any(child.tag == "stop" for child in inner_body.iter()):
        return None
    return _and_guard(_expr(outer_condition, widths, arrays), _expr(inner_condition, widths, arrays))


def _flatten_assignments(node: ET.Element, widths: dict[str, int], guard: str | None = None,
                         arrays: dict[str, tuple[int, int]] | None = None,
                         assertions: list[tuple[str, str]] | None = None) -> list[tuple[str, str, str | None, str]]:
    """Flatten resolved begin/if trees while preserving statement order."""
    if node.tag == "begin":
        result: list[tuple[str, str, str | None, str]] = []
        for child in node:
            result.extend(_flatten_assignments(child, widths, guard, arrays, assertions))
        return result
    if node.tag == "case":
        children = list(node)
        if len(children) < 2:
            raise UnsupportedResolvedNode("resolved case statement has no items")
        selector = _expr(children[0], widths, arrays)
        explicit_matches: list[str] = []
        default_item: list[ET.Element] | None = None
        result: list[tuple[str, str, str | None, str]] = []
        for item in children[1:]:
            if item.tag != "caseitem":
                failure = _unique_case_failure(item, widths, arrays or {})
                if failure is not None and assertions is not None:
                    assertions.append((failure, "resolved unique case has multiple matching items"))
                    continue
                raise UnsupportedResolvedNode("resolved case has non-caseitem child")
            item_children = list(item)
            label_count = 0
            # Resolved symbolic enum values may remain as varrefs (rather
            # than being folded constants), so both are legal case labels.
            while label_count < len(item_children) and item_children[label_count].tag in ("const", "varref"):
                label_count += 1
            if label_count == 0:
                if default_item is not None:
                    raise UnsupportedResolvedNode("resolved case has multiple defaults")
                default_item = item_children
                continue
            labels = [f"Expr::binary(ExprKind::case_equal, {selector}, {_expr(label, widths, arrays)})"
                      for label in item_children[:label_count]]
            item_match = _or_guard(labels)
            explicit_matches.append(item_match)
            for statement in item_children[label_count:]:
                result.extend(_flatten_assignments(statement, widths, _and_guard(guard, item_match), arrays, assertions))
        if default_item is not None:
            default_guard = _and_guard(guard, _not_guard(_or_guard(explicit_matches)))
            for statement in default_item:
                result.extend(_flatten_assignments(statement, widths, default_guard, arrays, assertions))
        return result
    if node.tag == "if":
        children = list(node)
        if len(children) not in (2, 3):
            raise UnsupportedResolvedNode("resolved if statement has unsupported shape")
        condition = _expr(children[0], widths, arrays)
        result = _flatten_assignments(children[1], widths, _and_guard(guard, condition), arrays, assertions)
        if len(children) == 3:
            result.extend(_flatten_assignments(children[2], widths, _and_guard(guard, _not_guard(condition)), arrays, assertions))
        return result
    if node.tag in ("assign", "assigndly"):
        children = list(node)
        if len(children) != 2:
            raise UnsupportedResolvedNode("procedural assignment has unsupported shape")
        target, slice_initializer = _target(children[1])
        return [(target, _expr(children[0], widths, arrays), guard, slice_initializer)]
    raise UnsupportedResolvedNode(f"unsupported resolved statement node: {node.tag}")


def _assignment_cpp(assignment: tuple[str, str, str | None, str]) -> str:
    target, expression, guard, target_slice = assignment
    return f'{{"{target}", {expression}, {guard or "std::nullopt"}{target_slice}}}'


def _assertion_cpp(assertion: tuple[str, str]) -> str:
    failure, message = assertion
    return f'{{{failure}, "{message}"}}'


def _canonical_async_reset(always: ET.Element, widths: dict[str, int], arrays: dict[str, tuple[int, int]]) -> tuple[str, str, list[tuple[str, str, str | None, str]], list[tuple[str, str, str | None, str]]] | None:
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
            _flatten_assignments(branches[1], widths, arrays=arrays), _flatten_assignments(branches[2], widths, arrays=arrays))


def emit_cpp_module(xml_path: Path, module_name: str, function_name: str) -> str:
    """Return C++ that constructs a strictly supported resolved module IR.

    The supported procedural subset is deliberately narrow: ordered blocking
    assignments in ``always_comb`` and canonical active-low asynchronous-reset
    flip-flop blocks. All other structures fail loudly.
    """
    root = ET.parse(xml_path).getroot()
    widths = _widths(root)
    array_types = _array_shapes(root, widths)
    module = next((item for item in root.findall(".//module") if item.attrib.get("name") == module_name), None)
    if module is None:
        raise ValueError(f"resolved XML module not found: {module_name}")
    signals: list[str] = []
    arrays: dict[str, tuple[int, int]] = {}
    for variable in module.findall("var"):
        if variable.attrib.get("param") == "true" or variable.attrib.get("localparam") == "true":
            continue
        dtype_id = variable.attrib.get("dtype_id", "")
        if dtype_id in array_types:
            arrays[variable.attrib["name"]] = array_types[dtype_id]
            continue
        width = widths.get(dtype_id)
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
            if target.tag == "varref" and target.attrib["name"] in arrays:
                if expression.tag != "varref" or expression.attrib.get("name") not in arrays:
                    raise UnsupportedResolvedNode("whole-array assignment requires a matching array source")
                target_name, source_name = target.attrib["name"], expression.attrib["name"]
                if arrays[target_name] != arrays[source_name]:
                    raise UnsupportedResolvedNode("whole-array assignment has mismatched array shapes")
                width, depth = arrays[target_name]
                writes = ", ".join(
                    f'{{"{target_name}", Expr::constant(LogicWord::known({element}ULL, 32)), '
                    f'Expr::memory_read("{source_name}", Expr::constant(LogicWord::known({element}ULL, 32))), std::nullopt}}'
                    for element in range(depth)
                )
                processes.append(f'{{"generated:array-copy:{index}", ProcessKind::combinational, "", "", {{}}, {{}}, {{{writes}}}}}')
                continue
            target_name, target_slice = _target(target)
            processes.append(
                f'{{"generated:cont:{index}", ProcessKind::combinational, "", "", '
                f'{{{{"{target_name}", {_expr(expression, widths, arrays)}, std::nullopt{target_slice}}}}}, {{}}}}'
            )
            continue
        async_reset = _canonical_async_reset(always, widths, arrays)
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
        assertions: list[tuple[str, str]] = []
        assignments = _flatten_assignments(body, widths, arrays=arrays, assertions=assertions)
        processes.append(
            f'{{"generated:comb:{index}", ProcessKind::combinational, "", "", '
            f'{{{", ".join(_assignment_cpp(item) for item in assignments)}}}, {{}}, {{}}, '
            f'{{{", ".join(_assertion_cpp(item) for item in assertions)}}}}}'
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
