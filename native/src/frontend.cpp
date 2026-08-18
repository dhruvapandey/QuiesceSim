#include "quiescesim/frontend.hpp"

#include <cctype>
#include <regex>
#include <stdexcept>

namespace quiescesim {
namespace {
std::string trim(std::string value) {
  const auto begin = value.find_first_not_of(" \t\n\r");
  const auto end = value.find_last_not_of(" \t\n\r");
  return begin == std::string::npos ? "" : value.substr(begin, end - begin + 1);
}

bool contains_parameterized_logic_width(const std::string& source) {
  std::size_t cursor = 0;
  while ((cursor = source.find("logic", cursor)) != std::string::npos) {
    const auto open = source.find('[', cursor + 5);
    const auto boundary = source.find_first_of(";,)", cursor + 5);
    if (open == std::string::npos || (boundary != std::string::npos && open > boundary)) { cursor += 5; continue; }
    const auto close = source.find(']', open + 1);
    if (close == std::string::npos) return true;
    for (std::size_t i = open + 1; i < close; ++i) {
      if (std::isalpha(static_cast<unsigned char>(source[i])) || source[i] == '_') return true;
    }
    cursor = close + 1;
  }
  return false;
}

std::size_t count_matches(const std::string& source, const std::regex& pattern) {
  return static_cast<std::size_t>(std::distance(std::sregex_iterator(source.begin(), source.end(), pattern), std::sregex_iterator()));
}

Expr lower_expression(const std::string& expression, std::uint8_t width) {
  const auto expr = trim(expression);
  if (expr == "'0" || expr == "0" || expr == "1'b0") return Expr::constant(LogicWord::known(0, width));
  if (expr == "1" || expr == "1'b1") return Expr::constant(LogicWord::known(1, width));
  if (expr.starts_with("0x")) return Expr::constant(LogicWord::known(std::stoull(expr, nullptr, 16), width));
  // This deliberately supports one flat conditional expression only. Do not
  // guess at precedence for nested conditionals: unsupported syntax must fail
  // at build time until the full parser owns those grammar rules.
  const auto question = expr.find('?');
  if (question != std::string::npos) {
    const auto colon = expr.find(':', question + 1);
    if (colon == std::string::npos || expr.find('?', question + 1) != std::string::npos || expr.find(':', colon + 1) != std::string::npos) {
      throw std::invalid_argument("unsupported nested or malformed conditional expression: " + expr);
    }
    return Expr::mux(lower_expression(expr.substr(0, question), 1),
                     lower_expression(expr.substr(question + 1, colon - question - 1), width),
                     lower_expression(expr.substr(colon + 1), width));
  }
  if (expr.starts_with("~")) return Expr::unary(ExprKind::bit_not, lower_expression(expr.substr(1), width));
  for (const auto op : {'|', '^', '&'}) {
    const auto index = expr.find(op);
    if (index != std::string::npos) {
      const auto left = lower_expression(expr.substr(0, index), width);
      const auto right = lower_expression(expr.substr(index + 1), width);
      return Expr::binary(op == '|' ? ExprKind::bit_or : op == '^' ? ExprKind::bit_xor : ExprKind::bit_and, left, right);
    }
  }
  const auto plus = expr.find('+');
  if (plus != std::string::npos) return Expr::binary(ExprKind::add, lower_expression(expr.substr(0, plus), width), lower_expression(expr.substr(plus + 1), width));
  bool decimal = !expr.empty();
  for (const auto c : expr) decimal = decimal && std::isdigit(static_cast<unsigned char>(c));
  if (decimal) return Expr::constant(LogicWord::known(std::stoull(expr), width));
  if (std::regex_match(expr, std::regex(R"([A-Za-z_][A-Za-z0-9_]*)"))) return Expr::variable(expr);
  throw std::invalid_argument("unsupported expression: " + expr);
}
}

RestrictedModule RestrictedModule::parse(const std::string& source) {
  RestrictedModule result;
  std::smatch module;
  if (!std::regex_search(source, module, std::regex(R"(module\s+([A-Za-z_][A-Za-z0-9_]*))"))) throw std::invalid_argument("missing module declaration");
  result.name_ = module[1];
  // This bring-up front end deliberately accepts only a narrow, resolved RTL
  // subset. Detect common real-design constructs before partial parsing can
  // turn them into misleading "undeclared signal" messages.
  if (contains_parameterized_logic_width(source)) {
    throw std::invalid_argument("unsupported parameterized packed width; the initial front end accepts literal ranges such as [7:0] only");
  }
  if (source.find("generate") != std::string::npos || source.find("genvar") != std::string::npos) {
    throw std::invalid_argument("unsupported generate construct; elaborate parameterized hierarchy before native compilation");
  }
  if (source.find("for (") != std::string::npos || source.find("for(") != std::string::npos) {
    throw std::invalid_argument("unsupported procedural or generate for-loop; the initial front end does not unroll loops");
  }
  if (source.find("parameter") != std::string::npos) {
    throw std::invalid_argument("unsupported parameter declaration; the initial front end requires an already-resolved module configuration");
  }
  if (source.find("always_latch") != std::string::npos || std::regex_search(source, std::regex(R"(\balways\s*@)"))) {
    throw std::invalid_argument("unsupported procedural block; the initial front end supports only its documented always_ff and always_comb forms");
  }
  // Accept both body declarations (`logic q;`) and the common ANSI port form
  // (`input logic q,`). Multi-name body declarations are handled below.
  const std::regex declaration(R"((?:(?:input|output)\s+)?logic\s*(?:\[\s*(\d+)\s*:\s*(\d+)\s*\])?\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?=[,;\)]))");
  for (std::sregex_iterator it(source.begin(), source.end(), declaration), end; it != end; ++it) {
    const auto width = (*it)[1].matched ? static_cast<std::uint8_t>(std::stoul((*it)[1]) - std::stoul((*it)[2]) + 1) : 1;
    result.widths_[(*it)[3]] = width;
  }
  const std::regex multi_declaration(R"(logic\s*(?:\[\s*(\d+)\s*:\s*(\d+)\s*\])?\s+([A-Za-z_][A-Za-z0-9_]*(?:\s*,\s*[A-Za-z_][A-Za-z0-9_]*)+)\s*;)");
  for (std::sregex_iterator it(source.begin(), source.end(), multi_declaration), end; it != end; ++it) {
    const auto width = (*it)[1].matched ? static_cast<std::uint8_t>(std::stoul((*it)[1]) - std::stoul((*it)[2]) + 1) : 1;
    std::string names = (*it)[3];
    std::size_t cursor = 0;
    while (cursor != std::string::npos) {
      const auto comma = names.find(',', cursor);
      result.widths_[trim(names.substr(cursor, comma == std::string::npos ? comma : comma - cursor))] = width;
      cursor = comma == std::string::npos ? comma : comma + 1;
    }
  }
  const std::regex ff(R"(always_ff\s*@\(\s*posedge\s+([A-Za-z_][A-Za-z0-9_]*)\s+or\s+negedge\s+([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*begin\s*if\s*\(\s*!\s*[A-Za-z_][A-Za-z0-9_]*\s*\)\s*([A-Za-z_][A-Za-z0-9_]*)\s*<=\s*([^;]+);\s*else\s*(?:if\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*)?[A-Za-z_][A-Za-z0-9_]*\s*<=\s*([^;]+);\s*end)");
  for (std::sregex_iterator it(source.begin(), source.end(), ff), end; it != end; ++it) {
    result.rules_.push_back({(*it)[1], (*it)[2], (*it)[3], trim((*it)[4]), trim((*it)[5]), trim((*it)[6])});
  }
  const std::regex ff_no_reset(R"(always_ff\s*@\(\s*posedge\s+([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*begin\s*(?:if\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*)?([A-Za-z_][A-Za-z0-9_]*)\s*<=\s*([^;]+);\s*end)");
  for (std::sregex_iterator it(source.begin(), source.end(), ff_no_reset), end; it != end; ++it) {
    result.rules_.push_back({(*it)[1], "", (*it)[3], "", trim((*it)[2]), trim((*it)[4])});
  }
  if (count_matches(source, std::regex(R"(\balways_ff\b)")) != result.rules_.size()) {
    throw std::invalid_argument("unsupported always_ff block; expected posedge clock or negedge reset with one reset assignment and one optional enable assignment");
  }
  for (const auto& rule : result.rules_) {
    if (!result.widths_.contains(rule.target)) throw std::invalid_argument("undeclared register: " + rule.target);
    if (!result.widths_.contains(rule.clock) || (!rule.reset.empty() && !result.widths_.contains(rule.reset))) throw std::invalid_argument("clock/reset must be declared logic");
  }
  const std::regex assign(R"(assign\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^;]+);)");
  for (std::sregex_iterator it(source.begin(), source.end(), assign), end; it != end; ++it) {
    result.comb_rules_.push_back({(*it)[1], trim((*it)[2])});
  }
  // The first always_comb increment intentionally permits one blocking
  // assignment only. A full block needs statement ordering, temporaries, and
  // complete implicit sensitivity rules, so it must not be approximated here.
  const std::regex always_comb(R"(always_comb\s+begin\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^;]+);\s*end)");
  for (std::sregex_iterator it(source.begin(), source.end(), always_comb), end; it != end; ++it) {
    result.comb_rules_.push_back({(*it)[1], trim((*it)[2])});
  }
  if (count_matches(source, std::regex(R"(\balways_comb\b)")) != count_matches(source, always_comb)) {
    throw std::invalid_argument("unsupported always_comb block; expected exactly one blocking assignment");
  }
  if (result.rules_.empty() && result.comb_rules_.empty()) throw std::invalid_argument("no supported assign or always_ff block found");
  for (const auto& rule : result.comb_rules_) {
    if (!result.widths_.contains(rule.target)) throw std::invalid_argument("undeclared assign target: " + rule.target);
  }
  return result;
}

ModuleIR RestrictedModule::to_ir() const {
  ModuleIR ir{.name = name_};
  for (const auto& [name, width] : widths_) ir.signals.push_back({name, width, LogicWord::x(width)});
  for (const auto& rule : rules_) {
    ProcessIR process{"always_ff:" + rule.target,
                      rule.reset.empty() ? ProcessKind::posedge : ProcessKind::posedge_or_negedge_reset,
                      rule.clock, rule.reset, {}, {}};
    process.assignments.push_back({rule.target, lower_expression(rule.update_expression, widths_.at(rule.target)), rule.enable_expression.empty() ? std::nullopt : std::optional<Expr>{lower_expression(rule.enable_expression, 1)}});
    if (!rule.reset.empty()) process.reset_assignments.push_back({rule.target, lower_expression(rule.reset_expression, widths_.at(rule.target)), std::nullopt});
    ir.processes.push_back(std::move(process));
  }
  for (const auto& rule : comb_rules_) {
    ir.processes.push_back({"comb:" + rule.target, ProcessKind::combinational, "", "", {{rule.target, lower_expression(rule.expression, widths_.at(rule.target)), std::nullopt}}, {}});
  }
  return ir;
}

std::unordered_map<std::string, SignalId> RestrictedModule::instantiate(Engine& engine) const {
  // All supported source now follows the same owned IR-to-native execution
  // path as hand-lowered Ibex kernels. Keeping parsing separate from runtime
  // compilation prevents the bootstrap frontend from becoming a second,
  // semantically divergent simulator.
  return compile_ir(to_ir(), engine);
}

}  // namespace quiescesim
