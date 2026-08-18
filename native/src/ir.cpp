#include "quiescesim/ir.hpp"

#include <stdexcept>
#include <unordered_set>

namespace quiescesim {
Expr Expr::constant(LogicWord value) { return {ExprKind::constant, value, "", nullptr, nullptr, nullptr, 0, 0}; }
Expr Expr::variable(std::string name) { return {ExprKind::variable, {}, std::move(name), nullptr, nullptr, nullptr, 0, 0}; }
Expr Expr::unary(ExprKind kind, Expr operand) { return {kind, {}, "", std::make_shared<Expr>(std::move(operand)), nullptr, nullptr, 0, 0}; }
Expr Expr::binary(ExprKind kind, Expr left, Expr right) { return {kind, {}, "", std::make_shared<Expr>(std::move(left)), std::make_shared<Expr>(std::move(right)), nullptr, 0, 0}; }
Expr Expr::mux(Expr select, Expr when_true, Expr when_false) { return {ExprKind::mux, {}, "", std::make_shared<Expr>(std::move(select)), std::make_shared<Expr>(std::move(when_true)), std::make_shared<Expr>(std::move(when_false)), 0, 0}; }
Expr Expr::slice(Expr operand, std::uint8_t msb, std::uint8_t lsb) { return {ExprKind::slice, {}, "", std::make_shared<Expr>(std::move(operand)), nullptr, nullptr, msb, lsb}; }
Expr Expr::concat(Expr high, Expr low) { return {ExprKind::concat, {}, "", std::make_shared<Expr>(std::move(high)), std::make_shared<Expr>(std::move(low)), nullptr, 0, 0}; }

namespace {
LogicWord evaluate(const Expr& expression, const std::unordered_map<std::string, SignalId>& signals, Engine& engine) {
  switch (expression.kind) {
    case ExprKind::constant: return expression.constant_value;
    case ExprKind::variable: return engine.read(signals.at(expression.variable_name));
    case ExprKind::bit_not: return bit_not(evaluate(*expression.left, signals, engine));
    case ExprKind::bit_and: return bit_and(evaluate(*expression.left, signals, engine), evaluate(*expression.right, signals, engine));
    case ExprKind::bit_or: return bit_or(evaluate(*expression.left, signals, engine), evaluate(*expression.right, signals, engine));
    case ExprKind::bit_xor: return bit_xor(evaluate(*expression.left, signals, engine), evaluate(*expression.right, signals, engine));
    case ExprKind::add: {
      const auto left = evaluate(*expression.left, signals, engine);
      const auto right = evaluate(*expression.right, signals, engine);
      if (left.width != right.width) throw std::invalid_argument("addition operands must have equal widths");
      return left.is_known() && right.is_known() ? LogicWord::known(left.as_u64() + right.as_u64(), left.width) : LogicWord::x(left.width);
    }
    case ExprKind::equal:
    case ExprKind::not_equal: {
      const auto left = evaluate(*expression.left, signals, engine);
      const auto right = evaluate(*expression.right, signals, engine);
      if (left.width != right.width) throw std::invalid_argument("comparison operands must have equal widths");
      if (!left.is_known() || !right.is_known()) return LogicWord::x(1);
      const bool equals = left.as_u64() == right.as_u64();
      return LogicWord::known(expression.kind == ExprKind::equal ? equals : !equals, 1);
    }
    case ExprKind::mux: {
      const auto select = evaluate(*expression.left, signals, engine);
      const auto when_true = evaluate(*expression.right, signals, engine);
      const auto when_false = evaluate(*expression.third, signals, engine);
      if (select.width != 1) throw std::invalid_argument("initial IR mux selector must be one bit");
      if (when_true.width != when_false.width) throw std::invalid_argument("mux branches must have equal widths");
      if (select.is_known()) return select.as_u64() != 0 ? when_true : when_false;
      // IEEE conditional operator merges only branch bits that are exactly
      // equal. Every disagreement, including known-vs-unknown, becomes X.
      const auto width_mask = when_true.width == 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << when_true.width) - 1);
      const auto true_unknown = when_true.x_mask | when_true.z_mask;
      const auto false_unknown = when_false.x_mask | when_false.z_mask;
      const auto same_known = ~(true_unknown | false_unknown) & ~(when_true.bits ^ when_false.bits) & width_mask;
      const auto same_x = when_true.x_mask & when_false.x_mask;
      const auto same_z = when_true.z_mask & when_false.z_mask;
      return {when_true.bits & same_known, (~same_known & ~same_x & ~same_z) & width_mask, same_z, when_true.width};
    }
    case ExprKind::slice: {
      const auto source = evaluate(*expression.left, signals, engine);
      if (expression.msb >= source.width || expression.lsb > expression.msb) throw std::invalid_argument("IR slice range is invalid");
      const auto width = static_cast<std::uint8_t>(expression.msb - expression.lsb + 1);
      const auto mask = width == 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << width) - 1);
      return {(source.bits >> expression.lsb) & mask, (source.x_mask >> expression.lsb) & mask, (source.z_mask >> expression.lsb) & mask, width};
    }
    case ExprKind::concat: {
      const auto high = evaluate(*expression.left, signals, engine);
      const auto low = evaluate(*expression.right, signals, engine);
      if (high.width + low.width > 64) throw std::invalid_argument("IR concat wider than 64 bits is unsupported");
      const auto width = static_cast<std::uint8_t>(high.width + low.width);
      return {(high.bits << low.width) | low.bits, (high.x_mask << low.width) | low.x_mask, (high.z_mask << low.width) | low.z_mask, width};
    }
  }
  throw std::logic_error("unknown QuiesceSim IR expression kind");
}

void collect_dependencies(const Expr& expression, const std::unordered_map<std::string, SignalId>& signals, std::unordered_set<SignalId>& result) {
  if (expression.kind == ExprKind::variable) { result.insert(signals.at(expression.variable_name)); return; }
  if (expression.left) collect_dependencies(*expression.left, signals, result);
  if (expression.right) collect_dependencies(*expression.right, signals, result);
  if (expression.third) collect_dependencies(*expression.third, signals, result);
}

bool condition_true(const std::optional<Expr>& condition, const std::unordered_map<std::string, SignalId>& signals, Engine& engine) {
  if (!condition) return true;
  const auto value = evaluate(*condition, signals, engine);
  return value.is_known() && value.as_u64() != 0;
}
}

std::unordered_map<std::string, SignalId> compile_ir(const ModuleIR& module, Engine& engine) {
  std::unordered_map<std::string, SignalId> signals;
  for (const auto& signal : module.signals) {
    if (signal.width != signal.initial.width) throw std::invalid_argument("IR signal initial value width mismatch: " + signal.name);
    signals.emplace(signal.name, engine.add_signal(signal.name, signal.initial));
  }
  for (const auto& process : module.processes) {
    if (process.kind == ProcessKind::combinational) {
      std::unordered_set<SignalId> unique_sensitivity;
      for (const auto& assignment : process.assignments) {
        collect_dependencies(assignment.expression, signals, unique_sensitivity);
        if (assignment.condition) collect_dependencies(*assignment.condition, signals, unique_sensitivity);
      }
      std::vector<SignalId> sensitivity(unique_sensitivity.begin(), unique_sensitivity.end());
      const auto id = engine.add_process(process.name, sensitivity, [signals, process](Engine& runtime) {
        for (const auto& assignment : process.assignments) {
          if (condition_true(assignment.condition, signals, runtime)) runtime.write_now(signals.at(assignment.target), evaluate(assignment.expression, signals, runtime));
        }
      });
      engine.schedule_active(id);
      continue;
    }
    const auto clock = signals.at(process.clock);
    const bool has_reset = process.kind == ProcessKind::posedge_or_negedge_reset;
    const auto reset = has_reset ? signals.at(process.reset) : SignalId{0};
    auto previous_clock = engine.read(clock);
    auto previous_reset = has_reset ? engine.read(reset) : LogicWord::known(1, 1);
    std::vector<SignalId> sensitivity{clock};
    if (has_reset) sensitivity.push_back(reset);
    engine.add_process(process.name, sensitivity, [signals, process, clock, reset, has_reset, previous_clock, previous_reset](Engine& runtime) mutable {
      const auto current_clock = runtime.read(clock);
      const auto current_reset = has_reset ? runtime.read(reset) : LogicWord::known(1, 1);
      const bool posedge = current_clock.is_known() && current_clock.as_u64() == 1 && (!previous_clock.is_known() || previous_clock.as_u64() == 0);
      const bool negedge_reset = has_reset && current_reset.is_known() && current_reset.as_u64() == 0 && (!previous_reset.is_known() || previous_reset.as_u64() == 1);
      previous_clock = current_clock;
      previous_reset = current_reset;
      if (!posedge && !negedge_reset) return;
      const auto& assignments = has_reset && current_reset.is_known() && current_reset.as_u64() == 0 ? process.reset_assignments : process.assignments;
      for (const auto& assignment : assignments) {
        if (condition_true(assignment.condition, signals, runtime)) {
          runtime.write_nba(signals.at(assignment.target), evaluate(assignment.expression, signals, runtime));
        } else if (assignment.condition) {
          // A known-false clock-enable is a proven no-op for this state
          // transition. Preserve normal SystemVerilog `if` behavior for X/Z,
          // but record only the definite optimization opportunity.
          const auto condition = evaluate(*assignment.condition, signals, runtime);
          if (condition.is_known() && condition.as_u64() == 0) runtime.record_guarded_skip();
        }
      }
    });
  }
  return signals;
}
}  // namespace quiescesim
