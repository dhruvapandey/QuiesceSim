#pragma once

#include "quiescesim/engine.hpp"
#include "quiescesim/ir.hpp"

#include <map>
#include <string>
#include <unordered_map>

namespace quiescesim {

// Restricted SystemVerilog compiler for the initial exact-mode bring-up.
// Supported: logic declarations, `always_ff @(posedge clk or negedge reset)`,
// a reset branch, and a single optional enable branch using <= assignments.
// Unsupported syntax is rejected instead of being guessed.
class RestrictedModule {
 public:
  static RestrictedModule parse(const std::string& source);
  [[nodiscard]] ModuleIR to_ir() const;
  std::unordered_map<std::string, SignalId> instantiate(Engine& engine) const;
  [[nodiscard]] const std::string& name() const { return name_; }

 private:
  struct RegisterRule {
    std::string clock;
    std::string reset;
    std::string target;
    std::string reset_expression;
    std::string enable_expression;
    std::string update_expression;
  };
  struct CombRule {
    std::string target;
    std::string expression;
  };
  std::string name_;
  // A sorted declaration map makes generated signal IDs, VCD declarations,
  // and debug output repeatable across runs and hosts.
  std::map<std::string, std::uint8_t> widths_;
  std::vector<RegisterRule> rules_;
  std::vector<CombRule> comb_rules_;
};

}  // namespace quiescesim
