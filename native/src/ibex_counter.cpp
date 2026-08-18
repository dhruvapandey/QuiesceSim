#include "quiescesim/ibex_counter.hpp"

namespace quiescesim {

ModuleIR ibex_counter64_ir() {
  const auto q = Expr::variable("counter_q");
  const auto input = Expr::variable("counter_val_i");
  const auto incremented = Expr::binary(ExprKind::add, q, Expr::constant(LogicWord::known(1, 64)));
  const auto low_write = Expr::concat(Expr::slice(q, 63, 32), input);
  const auto high_write = Expr::concat(input, Expr::slice(q, 31, 0));
  return {
      "ibex_counter__C40_P1",
      {
          {"clk_i", 1, LogicWord::x(1)}, {"rst_ni", 1, LogicWord::x(1)},
          {"counter_inc_i", 1, LogicWord::x(1)}, {"counterh_we_i", 1, LogicWord::x(1)}, {"counter_we_i", 1, LogicWord::x(1)},
          {"counter_val_i", 32, LogicWord::x(32)}, {"counter_val_o", 64, LogicWord::x(64)}, {"counter_val_upd_o", 64, LogicWord::x(64)},
          {"counter_q", 64, LogicWord::x(64)}, {"counter_d", 64, LogicWord::x(64)},
      },
      {
          {"ibex_counter64:comb", ProcessKind::combinational, "", "", {
              {"counter_val_o", q, std::nullopt},
              {"counter_val_upd_o", incremented, std::nullopt},
              {"counter_d", q, std::nullopt},
              {"counter_d", incremented, Expr::variable("counter_inc_i")},
              {"counter_d", low_write, Expr::variable("counter_we_i")},
              {"counter_d", high_write, Expr::variable("counterh_we_i")},
          }, {}},
          {"ibex_counter64:ff", ProcessKind::posedge_or_negedge_reset, "clk_i", "rst_ni",
              {{"counter_q", Expr::variable("counter_d"), std::nullopt}},
              {{"counter_q", Expr::constant(LogicWord::known(0, 64)), std::nullopt}}},
      }};
}

IbexCounter64Ports instantiate_ibex_counter64(Engine& engine) {
  const auto signals = compile_ir(ibex_counter64_ir(), engine);
  return {signals.at("clk_i"), signals.at("rst_ni"), signals.at("counter_inc_i"), signals.at("counterh_we_i"), signals.at("counter_we_i"),
          signals.at("counter_val_i"), signals.at("counter_val_o"), signals.at("counter_val_upd_o")};
}

}  // namespace quiescesim
