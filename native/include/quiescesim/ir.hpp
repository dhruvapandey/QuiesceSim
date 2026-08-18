#pragma once

#include "quiescesim/engine.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace quiescesim {

// QuiesceSim-owned, typed RTL IR. Parsers and bootstrap importers may emit this
// representation, but the native runtime never executes a frontend AST.
enum class ExprKind {
  constant, variable,
  bit_not, logical_not, bit_and, bit_or, bit_xor,
  add, subtract, shift_left_logical, shift_right_logical,
  equal, not_equal, less_than_unsigned, greater_equal_unsigned,
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
};

enum class ProcessKind { combinational, posedge, posedge_or_negedge_reset };

struct ProcessIR {
  std::string name;
  ProcessKind kind;
  std::string clock;
  std::string reset;
  std::vector<AssignmentIR> assignments;
  std::vector<AssignmentIR> reset_assignments;
};

struct SignalIR { std::string name; std::uint8_t width; LogicWord initial; };
struct ModuleIR { std::string name; std::vector<SignalIR> signals; std::vector<ProcessIR> processes; };

// Compile IR into exact active/NBA process callbacks. Returned IDs are the
// stable bridge used by testbench drivers, waves, and later hierarchy lowering.
std::unordered_map<std::string, SignalId> compile_ir(const ModuleIR& module, Engine& engine);

}  // namespace quiescesim
