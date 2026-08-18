"""Bootstrap elaboration import for QuiesceSim.

This module deliberately delegates parsing/elaboration to Verilator while the
native front end is under construction. It imports the resulting resolved IR
into QuiesceSim's own compact design database. It does *not* use Verilator for
simulation and must never be described as a QuiesceSim compatibility result.
"""

from __future__ import annotations

from collections import Counter
from dataclasses import asdict, dataclass
import hashlib
import json
from pathlib import Path
import subprocess


@dataclass(frozen=True)
class ModuleIR:
    name: str
    resolved_statements: int
    scheduling_processes: int
    child_cells: int
    variables: int


@dataclass(frozen=True)
class DesignIR:
    modules: tuple[ModuleIR, ...]

    @property
    def module_count(self) -> int:
        return len(self.modules)

    def ranked_kernel_candidates(self) -> list[ModuleIR]:
        return sorted(
            self.modules,
            key=lambda module: (module.scheduling_processes, module.resolved_statements, module.variables),
            reverse=True,
        )

    def as_dict(self) -> dict:
        return {
            "module_count": self.module_count,
            "kernel_candidates": [asdict(module) for module in self.ranked_kernel_candidates()],
        }

    def write_manifest(self, path: Path, *, source_json: Path, source_fingerprint: str) -> None:
        """Persist QuiesceSim-owned elaboration metadata for later lowering.

        The manifest is deliberately a compact summary, not Verilator's AST.
        It gives later compiler stages a reproducible input identity and a
        ranked worklist without making the bootstrap frontend part of runtime.
        """
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({
            "schema": "quiescesim-elaboration-manifest-v1",
            "frontend": "verilator-bootstrap",
            "source_json": source_json.name,
            "source_fingerprint_sha256": source_fingerprint,
            **self.as_dict(),
        }, indent=2, sort_keys=True) + "\n")


@dataclass(frozen=True)
class ResolvedModuleIR:
    """A compiler-owned view of one already-elaborated module.

    `processes` deliberately preserves the resolved statement tree for the
    next lowering stage. Runtime code never consumes Verilator's parser tree
    directly; an explicit QuiesceSim lowering pass must translate it first.
    """

    name: str
    variables: tuple[dict, ...]
    processes: tuple[dict, ...]

    def as_dict(self) -> dict:
        return {
            "schema": "quiescesim-resolved-module-v1",
            "module": self.name,
            "variables": list(self.variables),
            "processes": list(self.processes),
        }

    def write(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(self.as_dict(), indent=2, sort_keys=True) + "\n")


def _count_nodes(value: object, counts: Counter[str]) -> None:
    if isinstance(value, dict):
        counts[value.get("type", "UNKNOWN")] += 1
        for child in value.values():
            _count_nodes(child, counts)
    elif isinstance(value, list):
        for child in value:
            _count_nodes(child, counts)


def import_verilator_json(json_path: Path) -> DesignIR:
    """Load a resolved Verilator JSON netlist into QuiesceSim's IR summary."""
    raw = json.loads(json_path.read_text())
    modules: list[ModuleIR] = []
    for module in raw["modulesp"]:
        counts: Counter[str] = Counter()
        _count_nodes(module, counts)
        modules.append(ModuleIR(
            name=module["name"],
            resolved_statements=len(module.get("stmtsp", [])),
            scheduling_processes=counts["ALWAYS"],
            child_cells=counts["CELL"],
            variables=counts["VAR"],
        ))
    return DesignIR(tuple(modules))


def lower_resolved_module(json_path: Path, module_name: str) -> ResolvedModuleIR:
    """Extract a resolved module boundary as input to QuiesceSim lowering.

    This is intentionally not execution. It isolates variable declarations and
    process bodies after parameter/generate resolution so the native compiler
    can add typed-expression and statement lowering incrementally.
    """
    raw = json.loads(json_path.read_text())
    module = next((item for item in raw["modulesp"] if item["name"] == module_name), None)
    if module is None:
        raise ValueError(f"resolved module not found: {module_name}")
    variables = []
    processes = []
    def visit(statement: dict, path: tuple[str, ...]) -> None:
        """Collect executable processes after generate resolution.

        Verilator places selected generate branches beneath GENBLOCK nodes.
        Ignoring those nodes would silently omit real sequential logic (the
        resolved Ibex counter is one example), which is unacceptable for a
        correctness-oriented lowering pipeline.
        """
        statement_type = statement.get("type")
        name = statement.get("name", "")
        current_path = path + ((name or statement_type),)
        if statement_type == "ALWAYS":
            counts: Counter[str] = Counter()
            _count_nodes(statement.get("stmtsp", []), counts)
            processes.append({
                "keyword": statement.get("keyword", ""),
                "source": statement.get("loc", ""),
                "hierarchy_path": list(current_path),
                "statement_kinds": dict(sorted(counts.items())),
                "resolved_body": statement.get("stmtsp", []),
                "sensitivity": statement.get("sentreep", []),
            })
            return
        for key in ("stmtsp", "itemsp"):
            for child in statement.get(key, []):
                visit(child, current_path)

    for statement in module.get("stmtsp", []):
        if statement.get("type") == "VAR":
            variables.append({
                "name": statement["name"],
                "direction": statement.get("direction", "NONE"),
                "dtype_name": statement.get("dtypeName", ""),
                "is_primary_clock": statement.get("isPrimaryClock", False),
                "source": statement.get("loc", ""),
            })
        else:
            visit(statement, ())
    return ResolvedModuleIR(module_name, tuple(variables), tuple(processes))


def elaborate_with_verilator(file_list: Path, output_dir: Path, *, verilator: str = "verilator") -> DesignIR:
    """Elaborate sources using the temporary bootstrap front end and import IR."""
    output_dir.mkdir(parents=True, exist_ok=True)
    command = [
        verilator,
        "-f", str(file_list),
        "--json-only",
        "--Mdir", str(output_dir),
        "--Wno-UNOPTFLAT",
    ]
    subprocess.run(command, check=True)
    json_files = sorted(output_dir.glob("*.tree.json"))
    if len(json_files) != 1:
        raise RuntimeError(f"expected one Verilator JSON netlist, found {len(json_files)}")
    return import_verilator_json(json_files[0])


def fingerprint_file(path: Path) -> str:
    """Return a content hash used to invalidate a saved elaboration manifest."""
    return hashlib.sha256(path.read_bytes()).hexdigest()
