#include "quiescesim/ir.hpp"

#include <stdexcept>
#include <unordered_set>

namespace quiescesim {
Expr Expr::constant(LogicWord value) { return {ExprKind::constant, value, "", nullptr, nullptr, nullptr, 0, 0}; }
Expr Expr::variable(std::string name) { return {ExprKind::variable, {}, std::move(name), nullptr, nullptr, nullptr, 0, 0}; }
Expr Expr::memory_read(std::string memory_name, Expr address) { return {ExprKind::memory_read, {}, std::move(memory_name), std::make_shared<Expr>(std::move(address)), nullptr, nullptr, 0, 0}; }
Expr Expr::unary(ExprKind kind, Expr operand) { return {kind, {}, "", std::make_shared<Expr>(std::move(operand)), nullptr, nullptr, 0, 0}; }
Expr Expr::binary(ExprKind kind, Expr left, Expr right) { return {kind, {}, "", std::make_shared<Expr>(std::move(left)), std::make_shared<Expr>(std::move(right)), nullptr, 0, 0}; }
Expr Expr::mux(Expr select, Expr when_true, Expr when_false) { return {ExprKind::mux, {}, "", std::make_shared<Expr>(std::move(select)), std::make_shared<Expr>(std::move(when_true)), std::make_shared<Expr>(std::move(when_false)), 0, 0}; }
Expr Expr::slice(Expr operand, std::uint8_t msb, std::uint8_t lsb) { return {ExprKind::slice, {}, "", std::make_shared<Expr>(std::move(operand)), nullptr, nullptr, msb, lsb}; }
Expr Expr::concat(Expr high, Expr low) { return {ExprKind::concat, {}, "", std::make_shared<Expr>(std::move(high)), std::make_shared<Expr>(std::move(low)), nullptr, 0, 0}; }

namespace {
LogicWord evaluate(const Expr& expression, const std::unordered_map<std::string, SignalId>& signals,
                   const std::unordered_map<std::string, MemoryId>& memories, Engine& engine) {
  switch (expression.kind) {
    case ExprKind::constant: return expression.constant_value;
    case ExprKind::variable: return engine.read(signals.at(expression.variable_name));
    case ExprKind::memory_read: {
      const auto memory = memories.at(expression.variable_name);
      const auto address = evaluate(*expression.left, signals, memories, engine);
      if (!address.is_known() || address.as_u64() >= engine.memory_depth(memory)) return LogicWord::x(engine.memory_element_width(memory));
      return engine.read_memory(memory, address.as_u64());
    }
    case ExprKind::bit_not: return bit_not(evaluate(*expression.left, signals, memories, engine));
    case ExprKind::logical_not: {
      const auto operand = evaluate(*expression.left, signals, memories, engine);
      return operand.is_known() ? LogicWord::known(operand.as_u64() == 0, 1) : LogicWord::x(1);
    }
    case ExprKind::onehot: {
      const auto operand = evaluate(*expression.left, signals, memories, engine);
      if (!operand.is_known()) return LogicWord::x(1);
      const auto bits = operand.as_u64();
      return LogicWord::known(bits != 0 && (bits & (bits - 1)) == 0, 1);
    }
    case ExprKind::bit_and: return bit_and(evaluate(*expression.left, signals, memories, engine), evaluate(*expression.right, signals, memories, engine));
    case ExprKind::bit_or: return bit_or(evaluate(*expression.left, signals, memories, engine), evaluate(*expression.right, signals, memories, engine));
    case ExprKind::bit_xor: return bit_xor(evaluate(*expression.left, signals, memories, engine), evaluate(*expression.right, signals, memories, engine));
    case ExprKind::add:
    case ExprKind::subtract: {
      const auto left = evaluate(*expression.left, signals, memories, engine);
      const auto right = evaluate(*expression.right, signals, memories, engine);
      if (left.width != right.width) throw std::invalid_argument("arithmetic operands must have equal widths");
      if (!left.is_known() || !right.is_known()) return LogicWord::x(left.width);
      return LogicWord::known(expression.kind == ExprKind::add ? left.as_u64() + right.as_u64() : left.as_u64() - right.as_u64(), left.width);
    }
    case ExprKind::shift_left_logical:
    case ExprKind::shift_right_logical: {
      const auto left = evaluate(*expression.left, signals, memories, engine);
      const auto right = evaluate(*expression.right, signals, memories, engine);
      if (!left.is_known() || !right.is_known()) return LogicWord::x(left.width);
      const auto amount = right.as_u64();
      if (amount >= left.width) return LogicWord::known(0, left.width);
      return LogicWord::known(expression.kind == ExprKind::shift_left_logical ? left.as_u64() << amount : left.as_u64() >> amount, left.width);
    }
    case ExprKind::equal:
    case ExprKind::not_equal:
    case ExprKind::case_equal:
    case ExprKind::less_than_unsigned:
    case ExprKind::greater_equal_unsigned: {
      const auto left = evaluate(*expression.left, signals, memories, engine);
      const auto right = evaluate(*expression.right, signals, memories, engine);
      if (left.width != right.width) throw std::invalid_argument("comparison operands must have equal widths");
      if (expression.kind == ExprKind::case_equal) {
        const auto width_mask = left.width == 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << left.width) - 1);
        const auto left_known = ~(left.x_mask | left.z_mask) & width_mask;
        const auto right_known = ~(right.x_mask | right.z_mask) & width_mask;
        const bool matches = left.x_mask == right.x_mask && left.z_mask == right.z_mask &&
                             ((left.bits ^ right.bits) & left_known & right_known) == 0;
        return LogicWord::known(matches, 1);
      }
      if (!left.is_known() || !right.is_known()) return LogicWord::x(1);
      const bool result = expression.kind == ExprKind::equal ? left.as_u64() == right.as_u64()
          : expression.kind == ExprKind::not_equal ? left.as_u64() != right.as_u64()
          : expression.kind == ExprKind::less_than_unsigned ? left.as_u64() < right.as_u64()
          : left.as_u64() >= right.as_u64();
      return LogicWord::known(result, 1);
    }
    case ExprKind::mux: {
      const auto select = evaluate(*expression.left, signals, memories, engine);
      const auto when_true = evaluate(*expression.right, signals, memories, engine);
      const auto when_false = evaluate(*expression.third, signals, memories, engine);
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
      const auto source = evaluate(*expression.left, signals, memories, engine);
      if (expression.msb >= source.width || expression.lsb > expression.msb) throw std::invalid_argument("IR slice range is invalid");
      const auto width = static_cast<std::uint8_t>(expression.msb - expression.lsb + 1);
      const auto mask = width == 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << width) - 1);
      return {(source.bits >> expression.lsb) & mask, (source.x_mask >> expression.lsb) & mask, (source.z_mask >> expression.lsb) & mask, width};
    }
    case ExprKind::concat: {
      const auto high = evaluate(*expression.left, signals, memories, engine);
      const auto low = evaluate(*expression.right, signals, memories, engine);
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

void collect_memory_dependencies(const Expr& expression, const std::unordered_map<std::string, MemoryId>& memories,
                                 std::unordered_set<MemoryId>& result) {
  if (expression.kind == ExprKind::memory_read) result.insert(memories.at(expression.variable_name));
  if (expression.left) collect_memory_dependencies(*expression.left, memories, result);
  if (expression.right) collect_memory_dependencies(*expression.right, memories, result);
  if (expression.third) collect_memory_dependencies(*expression.third, memories, result);
}

bool condition_true(const std::optional<Expr>& condition, const std::unordered_map<std::string, SignalId>& signals,
                    const std::unordered_map<std::string, MemoryId>& memories, Engine& engine) {
  if (!condition) return true;
  const auto value = evaluate(*condition, signals, memories, engine);
  return value.is_known() && value.as_u64() != 0;
}

LogicWord replace_slice(LogicWord original, LogicWord replacement, std::uint8_t msb, std::uint8_t lsb) {
  if (lsb > msb || msb >= original.width) throw std::invalid_argument("IR assignment target slice is invalid");
  const auto width = static_cast<std::uint8_t>(msb - lsb + 1);
  if (replacement.width != width) throw std::invalid_argument("IR assignment target slice width mismatch");
  const auto field_mask = width == 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << width) - 1);
  const auto positioned_mask = field_mask << lsb;
  return {(original.bits & ~positioned_mask) | ((replacement.bits & field_mask) << lsb),
          (original.x_mask & ~positioned_mask) | ((replacement.x_mask & field_mask) << lsb),
          (original.z_mask & ~positioned_mask) | ((replacement.z_mask & field_mask) << lsb), original.width};
}
}

std::unordered_map<std::string, SignalId> compile_ir(const ModuleIR& module, Engine& engine) {
  return compile_ir(module, engine, {}, {});
}

std::unordered_map<std::string, SignalId> compile_ir(
    const ModuleIR& module, Engine& engine,
    const std::unordered_map<std::string, SignalId>& bindings) {
  return compile_ir(module, engine, bindings, {});
}

std::unordered_map<std::string, SignalId> compile_ir(
    const ModuleIR& module, Engine& engine,
    const std::unordered_map<std::string, SignalId>& bindings,
    const std::unordered_map<std::string, MemoryId>& memory_bindings) {
  std::unordered_map<std::string, SignalId> signals;
  for (const auto& signal : module.signals) {
    if (signal.width != signal.initial.width) throw std::invalid_argument("IR signal initial value width mismatch: " + signal.name);
    if (const auto binding = bindings.find(signal.name); binding != bindings.end()) {
      if (engine.read(binding->second).width != signal.width) {
        throw std::invalid_argument("IR hierarchy binding width mismatch: " + signal.name);
      }
      signals.emplace(signal.name, binding->second);
    } else {
      signals.emplace(signal.name, engine.add_signal(module.name + "." + signal.name, signal.initial));
    }
  }
  for (const auto& process : module.processes) {
    if (process.kind == ProcessKind::combinational) {
      std::unordered_set<SignalId> unique_sensitivity;
      std::unordered_set<MemoryId> unique_memory_sensitivity;
      for (const auto& assignment : process.assignments) {
        collect_dependencies(assignment.expression, signals, unique_sensitivity);
        collect_memory_dependencies(assignment.expression, memory_bindings, unique_memory_sensitivity);
        if (assignment.condition) collect_dependencies(*assignment.condition, signals, unique_sensitivity);
        if (assignment.condition) collect_memory_dependencies(*assignment.condition, memory_bindings, unique_memory_sensitivity);
      }
      for (const auto& write : process.memory_writes) {
        collect_dependencies(write.address, signals, unique_sensitivity);
        collect_dependencies(write.expression, signals, unique_sensitivity);
        collect_memory_dependencies(write.address, memory_bindings, unique_memory_sensitivity);
        collect_memory_dependencies(write.expression, memory_bindings, unique_memory_sensitivity);
        if (write.condition) collect_dependencies(*write.condition, signals, unique_sensitivity);
        if (write.condition) collect_memory_dependencies(*write.condition, memory_bindings, unique_memory_sensitivity);
      }
      for (const auto& assertion : process.assertions) {
        collect_dependencies(assertion.failure_condition, signals, unique_sensitivity);
        collect_memory_dependencies(assertion.failure_condition, memory_bindings, unique_memory_sensitivity);
      }
      std::vector<SignalId> sensitivity(unique_sensitivity.begin(), unique_sensitivity.end());
      std::vector<MemoryId> memory_sensitivity(unique_memory_sensitivity.begin(), unique_memory_sensitivity.end());
      const auto id = engine.add_process(process.name, sensitivity, memory_sensitivity, [signals, memory_bindings, process](Engine& runtime) {
        for (const auto& assignment : process.assignments) {
          if (!condition_true(assignment.condition, signals, memory_bindings, runtime)) continue;
          const auto target = signals.at(assignment.target);
          const auto value = evaluate(assignment.expression, signals, memory_bindings, runtime);
          if (assignment.target_slice) {
            const auto [msb, lsb] = *assignment.target_slice;
            runtime.write_now(target, replace_slice(runtime.read(target), value, msb, lsb));
          } else {
            runtime.write_now(target, value);
          }
        }
        for (const auto& write : process.memory_writes) {
          if (!condition_true(write.condition, signals, memory_bindings, runtime)) continue;
          const auto address = evaluate(write.address, signals, memory_bindings, runtime);
          if (!address.is_known()) throw std::invalid_argument("IR memory write address is unknown");
          const auto memory = memory_bindings.at(write.memory);
          if (address.as_u64() >= runtime.memory_depth(memory)) throw std::invalid_argument("IR memory write address is out of range");
          runtime.write_memory_now(memory, address.as_u64(), evaluate(write.expression, signals, memory_bindings, runtime));
        }
        for (const auto& assertion : process.assertions) {
          const auto failure = evaluate(assertion.failure_condition, signals, memory_bindings, runtime);
          if (failure.is_known() && failure.as_u64() != 0) throw std::runtime_error("QuiesceSim assertion failed: " + assertion.message);
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
    engine.add_process(process.name, sensitivity, [signals, memory_bindings, process, clock, reset, has_reset, previous_clock, previous_reset](Engine& runtime) mutable {
      const auto current_clock = runtime.read(clock);
      const auto current_reset = has_reset ? runtime.read(reset) : LogicWord::known(1, 1);
      const bool posedge = current_clock.is_known() && current_clock.as_u64() == 1 && (!previous_clock.is_known() || previous_clock.as_u64() == 0);
      const bool negedge_reset = has_reset && current_reset.is_known() && current_reset.as_u64() == 0 && (!previous_reset.is_known() || previous_reset.as_u64() == 1);
      previous_clock = current_clock;
      previous_reset = current_reset;
      if (!posedge && !negedge_reset) return;
      const auto& assignments = has_reset && current_reset.is_known() && current_reset.as_u64() == 0 ? process.reset_assignments : process.assignments;
      for (const auto& assignment : assignments) {
        if (condition_true(assignment.condition, signals, memory_bindings, runtime)) {
          const auto target = signals.at(assignment.target);
          const auto value = evaluate(assignment.expression, signals, memory_bindings, runtime);
          if (assignment.target_slice) {
            const auto [msb, lsb] = *assignment.target_slice;
            runtime.write_nba_slice(target, value, msb, lsb);
          } else {
            runtime.write_nba(target, value);
          }
        } else if (assignment.condition) {
          // A known-false clock-enable is a proven no-op for this state
          // transition. Preserve normal SystemVerilog `if` behavior for X/Z,
          // but record only the definite optimization opportunity.
          const auto condition = evaluate(*assignment.condition, signals, memory_bindings, runtime);
          if (condition.is_known() && condition.as_u64() == 0) runtime.record_guarded_skip();
        }
      }
      if (!(has_reset && current_reset.is_known() && current_reset.as_u64() == 0)) {
        for (const auto& write : process.memory_writes) {
          if (!condition_true(write.condition, signals, memory_bindings, runtime)) continue;
          const auto address = evaluate(write.address, signals, memory_bindings, runtime);
          if (!address.is_known()) throw std::invalid_argument("IR memory write address is unknown");
          const auto memory = memory_bindings.at(write.memory);
          if (address.as_u64() >= runtime.memory_depth(memory)) throw std::invalid_argument("IR memory write address is out of range");
          runtime.write_memory_nba(memory, address.as_u64(), evaluate(write.expression, signals, memory_bindings, runtime));
        }
      }
      for (const auto& assertion : process.assertions) {
        const auto failure = evaluate(assertion.failure_condition, signals, memory_bindings, runtime);
        if (failure.is_known() && failure.as_u64() != 0) throw std::runtime_error("QuiesceSim assertion failed: " + assertion.message);
      }
    });
  }
  return signals;
}
}  // namespace quiescesim
