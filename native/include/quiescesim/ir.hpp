#pragma once

#include "quiescesim/engine.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace quiescesim {

// QuiesceSim-owned, typed RTL IR. Parsers and bootstrap importers may emit this
// representation, but the native runtime never executes a frontend AST.
enum class ExprKind {
  constant, variable, memory_read,
  bit_not, logical_not, bit_and, bit_or, bit_xor,
  add, subtract, shift_left_logical, shift_right_logical,
  equal, not_equal, case_equal, less_than_unsigned, greater_equal_unsigned,
  mux, slice, concat
};

struct Expr {
  ExprKind kind;
  LogicWord constant_value{};
  std::string variable_name;
  std::shared_ptr<Expr> left;
  std::shared_ptr<Expr> right;
  std::shared_ptr<Expr> third;
  std::uint8_t msb{0};
  std::uint8_t lsb{0};

  static Expr constant(LogicWord value);
  static Expr variable(std::string name);
  static Expr memory_read(std::string memory_name, Expr address);
  static Expr unary(ExprKind kind, Expr operand);
  static Expr binary(ExprKind kind, Expr left, Expr right);
  // SystemVerilog conditional-expression semantics, including per-bit merge
  // when the selector is X/Z.
  static Expr mux(Expr select, Expr when_true, Expr when_false);
  static Expr slice(Expr operand, std::uint8_t msb, std::uint8_t lsb);
  static Expr concat(Expr high, Expr low);
};

struct AssignmentIR {
  std::string target;
  Expr expression;
  std::optional<Expr> condition;
  // Optional constant part-select on the LHS. Initial support is deliberately
  // combinational only; clocked partial writes need masked NBA commit records.
  std::optional<std::pair<std::uint8_t, std::uint8_t>> target_slice;

  AssignmentIR(std::string target_in, Expr expression_in,
               std::optional<Expr> condition_in = std::nullopt,
               std::optional<std::pair<std::uint8_t, std::uint8_t>> target_slice_in = std::nullopt)
      : target(std::move(target_in)), expression(std::move(expression_in)),
        condition(std::move(condition_in)), target_slice(std::move(target_slice_in)) {}
};

struct MemoryWriteIR {
  std::string memory;
  Expr address;
  Expr expression;
  std::optional<Expr> condition;

  MemoryWriteIR(std::string memory_in, Expr address_in, Expr expression_in,
                std::optional<Expr> condition_in = std::nullopt)
      : memory(std::move(memory_in)), address(std::move(address_in)),
        expression(std::move(expression_in)), condition(std::move(condition_in)) {}
};

enum class ProcessKind { combinational, posedge, posedge_or_negedge_reset };

struct ProcessIR {
  std::string name;
  ProcessKind kind;
  std::string clock;
  std::string reset;
  std::vector<AssignmentIR> assignments;
  std::vector<AssignmentIR> reset_assignments;
  std::vector<MemoryWriteIR> memory_writes;

  ProcessIR(std::string name_in, ProcessKind kind_in, std::string clock_in,
            std::string reset_in, std::vector<AssignmentIR> assignments_in,
            std::vector<AssignmentIR> reset_assignments_in,
            std::vector<MemoryWriteIR> memory_writes_in = {})
      : name(std::move(name_in)), kind(kind_in), clock(std::move(clock_in)),
        reset(std::move(reset_in)), assignments(std::move(assignments_in)),
        reset_assignments(std::move(reset_assignments_in)), memory_writes(std::move(memory_writes_in)) {}
};

struct SignalIR { std::string name; std::uint8_t width; LogicWord initial; };
struct ModuleIR { std::string name; std::vector<SignalIR> signals; std::vector<ProcessIR> processes; };

// Compile IR into exact active/NBA process callbacks. Returned IDs are the
// stable bridge used by testbench drivers, waves, and later hierarchy lowering.
std::unordered_map<std::string, SignalId> compile_ir(const ModuleIR& module, Engine& engine);
// Compile one hierarchy instance. A bound local signal reuses an existing
// parent signal ID; every unbound local signal is allocated by this instance.
// This is the first hierarchy primitive: instances share event scheduling and
// four-state net values rather than exchanging copied snapshots.
std::unordered_map<std::string, SignalId> compile_ir(
    const ModuleIR& module, Engine& engine,
    const std::unordered_map<std::string, SignalId>& bindings);
// Variant used by hierarchy lowering when a module reads a parent-owned
// memory. Memories are never copied: the compiled process observes the same
// event-aware storage as its parent and sibling instances.
std::unordered_map<std::string, SignalId> compile_ir(
    const ModuleIR& module, Engine& engine,
    const std::unordered_map<std::string, SignalId>& signal_bindings,
    const std::unordered_map<std::string, MemoryId>& memory_bindings);

}  // namespace quiescesim
