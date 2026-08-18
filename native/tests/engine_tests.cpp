#include "quiescesim/engine.hpp"
#include "quiescesim/frontend.hpp"
#include "quiescesim/ibex_counter.hpp"
#include "quiescesim/ir.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>

using namespace quiescesim;

void require(bool condition, const char* message) { if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); } }

int main() {
  Engine sim;
  const auto clk = sim.add_signal("clk", LogicWord::known(0, 1));
  const auto rst_n = sim.add_signal("rst_n", LogicWord::known(0, 1));
  const auto d = sim.add_signal("d", LogicWord::known(0, 8));
  const auto q = sim.add_signal("q", LogicWord::x(8));
  const auto seen = sim.add_signal("seen", LogicWord::x(8));

  const auto ff = sim.add_process("ff", {clk, rst_n}, [=](Engine& engine) {
    if (engine.read(rst_n).as_u64() == 0) engine.write_nba(q, LogicWord::known(0, 8));
    else if (engine.read(clk).as_u64() == 1) engine.write_nba(q, engine.read(d));
  });
  const auto observer = sim.add_process("observer", {clk}, [=](Engine& engine) {
    if (engine.read(clk).as_u64() == 1) engine.write_nba(seen, engine.read(q));
  });
  (void)ff; (void)observer;

  sim.schedule_at(1, [=](Engine& engine) { engine.write_now(clk, LogicWord::known(1, 1)); });
  sim.schedule_at(2, [=](Engine& engine) { engine.write_now(clk, LogicWord::known(0, 1)); });
  sim.schedule_at(3, [=](Engine& engine) { engine.write_now(rst_n, LogicWord::known(1, 1)); engine.write_now(d, LogicWord::known(0x2a, 8)); });
  sim.schedule_at(4, [=](Engine& engine) { engine.write_now(clk, LogicWord::known(1, 1)); });
  sim.run();

  require(sim.read(q) == LogicWord::known(0x2a, 8), "nonblocking register update did not commit");
  require(sim.read(seen) == LogicWord::known(0, 8), "observer did not see pre-NBA q value");
  require(sim.waves().size() >= 7, "wave changes were not captured");
  Engine nba_conflict;
  const auto conflict_q = nba_conflict.add_signal("q", LogicWord::known(0, 1));
  const auto first_writer = nba_conflict.add_process("first_writer", {}, [=](Engine& engine) {
    engine.write_nba(conflict_q, LogicWord::known(0, 1));
    engine.write_nba(conflict_q, LogicWord::known(1, 1));
  });
  nba_conflict.schedule_active(first_writer);
  nba_conflict.run();
  require(nba_conflict.read(conflict_q) == LogicWord::known(1, 1), "last NBA assignment in a process did not win");
  Engine two_writers;
  const auto shared_q = two_writers.add_signal("shared_q", LogicWord::known(0, 1));
  const auto writer_a = two_writers.add_process("writer_a", {}, [=](Engine& engine) { engine.write_nba(shared_q, LogicWord::known(0, 1)); });
  const auto writer_b = two_writers.add_process("writer_b", {}, [=](Engine& engine) { engine.write_nba(shared_q, LogicWord::known(1, 1)); });
  two_writers.schedule_active(writer_a);
  two_writers.schedule_active(writer_b);
  two_writers.run();
  require(two_writers.read(shared_q) == LogicWord::known(1, 1), "NBA ordering between scheduled processes was not deterministic");
  std::ostringstream vcd;
  sim.write_vcd(vcd);
  require(vcd.str().find("$enddefinitions $end") != std::string::npos, "VCD header was not emitted");
  require(vcd.str().find("#4") != std::string::npos, "VCD did not include timed value changes");
  std::ostringstream repeat_vcd;
  sim.write_vcd(repeat_vcd);
  require(vcd.str() == repeat_vcd.str(), "VCD export was not deterministic");
  bool x_read_rejected = false;
  try { (void)LogicWord::x(1).as_u64(); } catch (const std::logic_error&) { x_read_rejected = true; }
  require(x_read_rejected, "four-state unknown was coerced into an integer");
  require(bit_and(LogicWord::known(0, 1), LogicWord::x(1)) == LogicWord::known(0, 1), "0 & X must be 0");
  require(bit_and(LogicWord::known(1, 1), LogicWord::x(1)) == LogicWord::x(1), "1 & X must be X");
  require(bit_or(LogicWord::known(1, 1), LogicWord::z(1)) == LogicWord::known(1, 1), "1 | Z must be 1");
  require(bit_not(LogicWord::z(1)) == LogicWord::x(1), "bitwise inversion of Z must produce X");
  require(bit_xor(LogicWord::x(1), LogicWord::known(0, 1)) == LogicWord::x(1), "X ^ 0 must be X");
  const std::string rtl = R"(
    module counter;
      logic clk; logic rst_n; logic en; logic [7:0] q;
      always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) q <= '0;
        else if (en) q <= q + 1;
      end
    endmodule
  )";
  Engine compiled;
  const auto model = RestrictedModule::parse(rtl);
  const auto ports = model.instantiate(compiled);
  compiled.write_now(ports.at("clk"), LogicWord::known(0, 1));
  compiled.write_now(ports.at("rst_n"), LogicWord::known(0, 1));
  compiled.write_now(ports.at("en"), LogicWord::known(1, 1));
  compiled.write_now(ports.at("clk"), LogicWord::known(1, 1));
  compiled.run();
  require(compiled.read(ports.at("q")) == LogicWord::known(0, 8), "compiled reset branch failed");
  compiled.schedule_at(1, [=](Engine& runtime) { runtime.write_now(ports.at("clk"), LogicWord::known(0, 1)); });
  compiled.schedule_at(2, [=](Engine& runtime) { runtime.write_now(ports.at("rst_n"), LogicWord::known(1, 1)); });
  compiled.schedule_at(3, [=](Engine& runtime) { runtime.write_now(ports.at("clk"), LogicWord::known(1, 1)); });
  compiled.run();
  require(compiled.read(ports.at("q")) == LogicWord::known(1, 8), "compiled enabled counter failed");
  compiled.write_now(ports.at("en"), LogicWord::known(0, 1));
  compiled.schedule_at(4, [=](Engine& runtime) { runtime.write_now(ports.at("clk"), LogicWord::known(0, 1)); });
  compiled.schedule_at(5, [=](Engine& runtime) { runtime.write_now(ports.at("clk"), LogicWord::known(1, 1)); });
  compiled.run();
  require(compiled.read(ports.at("q")) == LogicWord::known(1, 8), "compiled disabled clock-enable changed register state");
  Engine ir_from_frontend;
  const auto ir_ports = compile_ir(model.to_ir(), ir_from_frontend);
  ir_from_frontend.write_now(ir_ports.at("clk"), LogicWord::known(0, 1));
  ir_from_frontend.write_now(ir_ports.at("rst_n"), LogicWord::known(0, 1));
  ir_from_frontend.write_now(ir_ports.at("en"), LogicWord::known(1, 1));
  ir_from_frontend.write_now(ir_ports.at("clk"), LogicWord::known(1, 1));
  ir_from_frontend.run();
  ir_from_frontend.schedule_at(1, [=](Engine& runtime) { runtime.write_now(ir_ports.at("clk"), LogicWord::known(0, 1)); });
  ir_from_frontend.schedule_at(2, [=](Engine& runtime) { runtime.write_now(ir_ports.at("rst_n"), LogicWord::known(1, 1)); });
  ir_from_frontend.schedule_at(3, [=](Engine& runtime) { runtime.write_now(ir_ports.at("clk"), LogicWord::known(1, 1)); });
  ir_from_frontend.run();
  require(ir_from_frontend.read(ir_ports.at("q")) == LogicWord::known(1, 8), "restricted frontend IR did not preserve counter behavior");
  const std::string ff_no_reset_rtl = R"(
    module register_no_reset;
      logic clk; logic d; logic q;
      always_ff @(posedge clk) begin
        q <= d;
      end
    endmodule
  )";
  Engine ff_no_reset_engine;
  const auto ff_no_reset_model = RestrictedModule::parse(ff_no_reset_rtl);
  const auto ff_no_reset_signals = ff_no_reset_model.instantiate(ff_no_reset_engine);
  ff_no_reset_engine.write_now(ff_no_reset_signals.at("clk"), LogicWord::known(0, 1));
  ff_no_reset_engine.write_now(ff_no_reset_signals.at("d"), LogicWord::known(1, 1));
  ff_no_reset_engine.write_now(ff_no_reset_signals.at("clk"), LogicWord::known(1, 1));
  ff_no_reset_engine.run();
  require(ff_no_reset_engine.read(ff_no_reset_signals.at("q")) == LogicWord::known(1, 1), "posedge-only always_ff did not update register");
  const std::string ff_enable_rtl = R"(
    module register_enable;
      logic clk; logic en; logic d; logic q;
      always_ff @(posedge clk) begin
        if (en) q <= d;
      end
    endmodule
  )";
  Engine ff_enable_engine;
  const auto ff_enable_model = RestrictedModule::parse(ff_enable_rtl);
  const auto ff_enable_signals = ff_enable_model.instantiate(ff_enable_engine);
  ff_enable_engine.write_now(ff_enable_signals.at("clk"), LogicWord::known(0, 1));
  ff_enable_engine.write_now(ff_enable_signals.at("en"), LogicWord::known(0, 1));
  ff_enable_engine.write_now(ff_enable_signals.at("d"), LogicWord::known(1, 1));
  ff_enable_engine.write_now(ff_enable_signals.at("clk"), LogicWord::known(1, 1));
  ff_enable_engine.run();
  require(ff_enable_engine.read(ff_enable_signals.at("q")) == LogicWord::x(1), "disabled posedge-only always_ff should preserve X state");
  require(ff_enable_engine.skipped_guarded_evaluations() == 1, "definitely disabled register did not use guarded skip");
  Engine ff_unknown_enable_engine;
  const auto ff_unknown_enable_model = RestrictedModule::parse(ff_enable_rtl);
  const auto ff_unknown_enable_signals = ff_unknown_enable_model.instantiate(ff_unknown_enable_engine);
  ff_unknown_enable_engine.write_now(ff_unknown_enable_signals.at("clk"), LogicWord::known(0, 1));
  ff_unknown_enable_engine.write_now(ff_unknown_enable_signals.at("en"), LogicWord::x(1));
  ff_unknown_enable_engine.write_now(ff_unknown_enable_signals.at("d"), LogicWord::known(1, 1));
  ff_unknown_enable_engine.write_now(ff_unknown_enable_signals.at("clk"), LogicWord::known(1, 1));
  ff_unknown_enable_engine.run();
  require(ff_unknown_enable_engine.read(ff_unknown_enable_signals.at("q")) == LogicWord::x(1), "X clock-enable must not take the then branch");
  require(ff_unknown_enable_engine.skipped_guarded_evaluations() == 0, "X clock-enable must not be reported as a guarded skip");
  const std::string comb_rtl = R"(
    module logic_cone;
      logic a; logic b; logic y;
      assign y = a & ~b;
    endmodule
  )";
  Engine combinational;
  const auto cone = RestrictedModule::parse(comb_rtl);
  const auto cone_signals = cone.instantiate(combinational);
  combinational.write_now(cone_signals.at("a"), LogicWord::known(1, 1));
  combinational.write_now(cone_signals.at("b"), LogicWord::known(0, 1));
  combinational.run();
  require(combinational.read(cone_signals.at("y")) == LogicWord::known(1, 1), "compiled continuous assignment failed");
  Engine combinational_unknown;
  const auto unknown_cone = RestrictedModule::parse(comb_rtl);
  const auto unknown_signals = unknown_cone.instantiate(combinational_unknown);
  combinational_unknown.write_now(unknown_signals.at("a"), LogicWord::x(1));
  combinational_unknown.write_now(unknown_signals.at("b"), LogicWord::known(0, 1));
  combinational_unknown.run();
  require(combinational_unknown.read(unknown_signals.at("y")) == LogicWord::x(1), "frontend did not propagate X through compiled assignment");
  const std::string always_comb_rtl = R"(
    module mux2;
      logic a; logic b; logic y;
      always_comb begin
        y = a | b;
      end
    endmodule
  )";
  Engine always_comb_engine;
  const auto always_comb_model = RestrictedModule::parse(always_comb_rtl);
  const auto always_comb_signals = always_comb_model.instantiate(always_comb_engine);
  always_comb_engine.write_now(always_comb_signals.at("a"), LogicWord::known(0, 1));
  always_comb_engine.write_now(always_comb_signals.at("b"), LogicWord::known(1, 1));
  always_comb_engine.run();
  require(always_comb_engine.read(always_comb_signals.at("y")) == LogicWord::known(1, 1), "always_comb did not evaluate initial inputs");
  always_comb_engine.write_now(always_comb_signals.at("b"), LogicWord::known(0, 1));
  always_comb_engine.run();
  require(always_comb_engine.read(always_comb_signals.at("y")) == LogicWord::known(0, 1), "always_comb did not wake on a dependency change");
  const std::string unsupported_always_comb_rtl = R"(
    module unsafe_partial_compile;
      logic a; logic b; logic y;
      assign y = a;
      always_comb begin
        y = a;
        y = b;
      end
    endmodule
  )";
  bool unsupported_always_comb_rejected = false;
  try { (void)RestrictedModule::parse(unsupported_always_comb_rtl); }
  catch (const std::invalid_argument& error) {
    unsupported_always_comb_rejected = std::string(error.what()).find("unsupported always_comb") != std::string::npos;
  }
  require(unsupported_always_comb_rejected, "unsupported always_comb block was silently partially compiled");
  Engine ibex_counter;
  const auto counter_ports = instantiate_ibex_counter64(ibex_counter);
  ibex_counter.write_now(counter_ports.clk_i, LogicWord::known(0, 1));
  ibex_counter.write_now(counter_ports.rst_ni, LogicWord::known(0, 1));
  ibex_counter.write_now(counter_ports.counter_inc_i, LogicWord::known(0, 1));
  ibex_counter.write_now(counter_ports.counterh_we_i, LogicWord::known(0, 1));
  ibex_counter.write_now(counter_ports.counter_we_i, LogicWord::known(0, 1));
  ibex_counter.write_now(counter_ports.counter_val_i, LogicWord::known(0, 32));
  ibex_counter.write_now(counter_ports.clk_i, LogicWord::known(1, 1));
  ibex_counter.run();
  require(ibex_counter.read(counter_ports.counter_val_o) == LogicWord::known(0, 64), "Ibex counter reset did not clear state");
  ibex_counter.schedule_at(1, [=](Engine& runtime) { runtime.write_now(counter_ports.clk_i, LogicWord::known(0, 1)); });
  ibex_counter.schedule_at(2, [=](Engine& runtime) { runtime.write_now(counter_ports.rst_ni, LogicWord::known(1, 1)); });
  ibex_counter.schedule_at(3, [=](Engine& runtime) { runtime.write_now(counter_ports.counter_inc_i, LogicWord::known(1, 1)); });
  ibex_counter.schedule_at(4, [=](Engine& runtime) { runtime.write_now(counter_ports.clk_i, LogicWord::known(1, 1)); });
  ibex_counter.run();
  require(ibex_counter.read(counter_ports.counter_val_o) == LogicWord::known(1, 64), "Ibex counter increment did not match resolved RTL behavior");
  require(ibex_counter.read(counter_ports.counter_val_upd_o) == LogicWord::known(2, 64), "Ibex counter incremented output did not match resolved RTL behavior");
  ibex_counter.schedule_at(5, [=](Engine& runtime) { runtime.write_now(counter_ports.clk_i, LogicWord::known(0, 1)); });
  ibex_counter.schedule_at(6, [=](Engine& runtime) { runtime.write_now(counter_ports.counter_inc_i, LogicWord::known(0, 1)); });
  ibex_counter.schedule_at(7, [=](Engine& runtime) { runtime.write_now(counter_ports.counter_we_i, LogicWord::known(1, 1)); });
  ibex_counter.schedule_at(8, [=](Engine& runtime) { runtime.write_now(counter_ports.counter_val_i, LogicWord::known(0xdeadbeef, 32)); });
  ibex_counter.schedule_at(9, [=](Engine& runtime) { runtime.write_now(counter_ports.clk_i, LogicWord::known(1, 1)); });
  ibex_counter.run();
  require(ibex_counter.read(counter_ports.counter_val_o) == LogicWord::known(0x00000000deadbeefULL, 64), "Ibex counter low-word write did not preserve high word");
  ibex_counter.schedule_at(10, [=](Engine& runtime) { runtime.write_now(counter_ports.clk_i, LogicWord::known(0, 1)); });
  ibex_counter.schedule_at(11, [=](Engine& runtime) { runtime.write_now(counter_ports.counter_we_i, LogicWord::known(0, 1)); });
  ibex_counter.schedule_at(12, [=](Engine& runtime) { runtime.write_now(counter_ports.counterh_we_i, LogicWord::known(1, 1)); });
  ibex_counter.schedule_at(13, [=](Engine& runtime) { runtime.write_now(counter_ports.counter_val_i, LogicWord::known(0xcafebabe, 32)); });
  ibex_counter.schedule_at(14, [=](Engine& runtime) { runtime.write_now(counter_ports.clk_i, LogicWord::known(1, 1)); });
  ibex_counter.run();
  require(ibex_counter.read(counter_ports.counter_val_o) == LogicWord::known(0xcafebabedeadbeefULL, 64), "Ibex counter high-word write did not preserve low word");
  const ModuleIR ir_counter{
      "ir_counter",
      {{"clk", 1, LogicWord::x(1)}, {"rst_n", 1, LogicWord::x(1)}, {"d", 8, LogicWord::x(8)}, {"q", 8, LogicWord::x(8)}, {"y", 8, LogicWord::x(8)}},
      {
          {"ir:comb", ProcessKind::combinational, "", "", {{"y", Expr::binary(ExprKind::bit_xor, Expr::variable("q"), Expr::constant(LogicWord::known(0xff, 8))), std::nullopt}}, {}},
          {"ir:ff", ProcessKind::posedge_or_negedge_reset, "clk", "rst_n", {{"q", Expr::variable("d"), std::nullopt}}, {{"q", Expr::constant(LogicWord::known(0, 8)), std::nullopt}}},
      }};
  Engine ir_engine;
  const auto ir_signals = compile_ir(ir_counter, ir_engine);
  ir_engine.write_now(ir_signals.at("clk"), LogicWord::known(0, 1));
  ir_engine.write_now(ir_signals.at("rst_n"), LogicWord::known(0, 1));
  ir_engine.write_now(ir_signals.at("d"), LogicWord::known(0x3c, 8));
  ir_engine.write_now(ir_signals.at("clk"), LogicWord::known(1, 1));
  ir_engine.run();
  require(ir_engine.read(ir_signals.at("q")) == LogicWord::known(0, 8), "IR reset assignment did not execute");
  ir_engine.schedule_at(1, [=](Engine& runtime) { runtime.write_now(ir_signals.at("clk"), LogicWord::known(0, 1)); });
  ir_engine.schedule_at(2, [=](Engine& runtime) { runtime.write_now(ir_signals.at("rst_n"), LogicWord::known(1, 1)); });
  ir_engine.schedule_at(3, [=](Engine& runtime) { runtime.write_now(ir_signals.at("clk"), LogicWord::known(1, 1)); });
  ir_engine.run();
  require(ir_engine.read(ir_signals.at("q")) == LogicWord::known(0x3c, 8), "IR clocked assignment did not execute");
  require(ir_engine.read(ir_signals.at("y")) == LogicWord::known(0xc3, 8), "IR combinational assignment did not execute");
  const ModuleIR ir_compare{
      "ir_compare",
      {{"a", 8, LogicWord::x(8)}, {"b", 8, LogicWord::x(8)}, {"match", 1, LogicWord::x(1)}},
      {{"ir:compare", ProcessKind::combinational, "", "", {{"match", Expr::binary(ExprKind::equal, Expr::variable("a"), Expr::variable("b")), std::nullopt}}, {}}}};
  Engine compare_engine;
  const auto compare_signals = compile_ir(ir_compare, compare_engine);
  compare_engine.write_now(compare_signals.at("a"), LogicWord::known(0x5a, 8));
  compare_engine.write_now(compare_signals.at("b"), LogicWord::known(0x5a, 8));
  compare_engine.run();
  require(compare_engine.read(compare_signals.at("match")) == LogicWord::known(1, 1), "IR equality did not evaluate equal operands");
  compare_engine.write_now(compare_signals.at("b"), LogicWord::known(0x5b, 8));
  compare_engine.run();
  require(compare_engine.read(compare_signals.at("match")) == LogicWord::known(0, 1), "IR equality did not wake on a changed operand");
  const ModuleIR ir_mux{
      "ir_mux",
      {{"sel", 1, LogicWord::x(1)}, {"a", 4, LogicWord::x(4)}, {"b", 4, LogicWord::x(4)}, {"y", 4, LogicWord::x(4)}},
      {{"ir:mux", ProcessKind::combinational, "", "", {{"y", Expr::mux(Expr::variable("sel"), Expr::variable("a"), Expr::variable("b")), std::nullopt}}, {}}}};
  Engine mux_engine;
  const auto mux_signals = compile_ir(ir_mux, mux_engine);
  mux_engine.write_now(mux_signals.at("a"), LogicWord::known(0xa, 4));
  mux_engine.write_now(mux_signals.at("b"), LogicWord::known(0x8, 4));
  mux_engine.write_now(mux_signals.at("sel"), LogicWord::x(1));
  mux_engine.run();
  require(mux_engine.read(mux_signals.at("y")) == LogicWord{0x8, 0x2, 0, 4}, "IR mux did not merge X-selected branches bitwise");
  mux_engine.write_now(mux_signals.at("sel"), LogicWord::known(1, 1));
  mux_engine.run();
  require(mux_engine.read(mux_signals.at("y")) == LogicWord::known(0xa, 4), "IR mux did not select true branch");
  const std::string source_mux_rtl = R"(
    module source_mux(input logic sel, input logic [3:0] a, input logic [3:0] b, output logic [3:0] y);
      assign y = sel ? a : b;
    endmodule
  )";
  Engine source_mux_engine;
  const auto source_mux = RestrictedModule::parse(source_mux_rtl).instantiate(source_mux_engine);
  source_mux_engine.write_now(source_mux.at("a"), LogicWord::known(0xa, 4));
  source_mux_engine.write_now(source_mux.at("b"), LogicWord::known(0x8, 4));
  source_mux_engine.write_now(source_mux.at("sel"), LogicWord::x(1));
  source_mux_engine.run();
  require(source_mux_engine.read(source_mux.at("y")) == LogicWord{0x8, 0x2, 0, 4}, "source conditional did not preserve four-state mux semantics");
  const ModuleIR ir_alu_primitives{
      "ir_alu_primitives",
      {{"a", 8, LogicWord::x(8)}, {"b", 8, LogicWord::x(8)}, {"result", 8, LogicWord::x(8)}, {"less", 1, LogicWord::x(1)}, {"zero", 1, LogicWord::x(1)}},
      {{"ir:alu", ProcessKind::combinational, "", "", {
          {"result", Expr::binary(ExprKind::shift_right_logical, Expr::binary(ExprKind::subtract, Expr::variable("a"), Expr::variable("b")), Expr::constant(LogicWord::known(1, 8))), std::nullopt},
          {"less", Expr::binary(ExprKind::less_than_unsigned, Expr::variable("a"), Expr::variable("b")), std::nullopt},
          {"zero", Expr::unary(ExprKind::logical_not, Expr::variable("a")), std::nullopt},
      }, {}}}};
  Engine alu_engine;
  const auto alu = compile_ir(ir_alu_primitives, alu_engine);
  alu_engine.write_now(alu.at("a"), LogicWord::known(4, 8));
  alu_engine.write_now(alu.at("b"), LogicWord::known(6, 8));
  alu_engine.run();
  require(alu_engine.read(alu.at("result")) == LogicWord::known(127, 8), "IR subtraction or logical shift did not wrap at signal width");
  require(alu_engine.read(alu.at("less")) == LogicWord::known(1, 1), "IR unsigned comparison failed");
  require(alu_engine.read(alu.at("zero")) == LogicWord::known(0, 1), "IR logical negation failed");
  alu_engine.write_now(alu.at("a"), LogicWord::x(8));
  alu_engine.run();
  require(alu_engine.read(alu.at("result")) == LogicWord::x(8), "IR arithmetic did not propagate unknown input");
  require(alu_engine.read(alu.at("less")) == LogicWord::x(1), "IR comparison did not propagate unknown input");
  const ModuleIR hierarchy_register{
      "hierarchy_register",
      {{"clk", 1, LogicWord::x(1)}, {"rst_n", 1, LogicWord::x(1)}, {"d", 8, LogicWord::x(8)}, {"q", 8, LogicWord::x(8)}},
      {{"register:ff", ProcessKind::posedge_or_negedge_reset, "clk", "rst_n", {{"q", Expr::variable("d"), std::nullopt}}, {{"q", Expr::constant(LogicWord::known(0, 8)), std::nullopt}}}}};
  const ModuleIR hierarchy_observer{
      "hierarchy_observer",
      {{"i", 8, LogicWord::x(8)}, {"o", 8, LogicWord::x(8)}},
      {{"observer:comb", ProcessKind::combinational, "", "", {{"o", Expr::binary(ExprKind::bit_xor, Expr::variable("i"), Expr::constant(LogicWord::known(0xff, 8))), std::nullopt}}, {}}}};
  Engine hierarchy_engine;
  const auto top_clk = hierarchy_engine.add_signal("top.clk", LogicWord::x(1));
  const auto top_rst_n = hierarchy_engine.add_signal("top.rst_n", LogicWord::x(1));
  const auto top_d = hierarchy_engine.add_signal("top.d", LogicWord::x(8));
  const auto hierarchy_shared_q = hierarchy_engine.add_signal("top.shared_q", LogicWord::x(8));
  const auto register_instance = compile_ir(hierarchy_register, hierarchy_engine, {{"clk", top_clk}, {"rst_n", top_rst_n}, {"d", top_d}, {"q", hierarchy_shared_q}});
  const auto observer_instance = compile_ir(hierarchy_observer, hierarchy_engine, {{"i", hierarchy_shared_q}});
  hierarchy_engine.write_now(top_clk, LogicWord::known(0, 1));
  hierarchy_engine.write_now(top_rst_n, LogicWord::known(0, 1));
  hierarchy_engine.write_now(top_clk, LogicWord::known(1, 1));
  hierarchy_engine.run();
  require(hierarchy_engine.read(hierarchy_shared_q) == LogicWord::known(0, 8), "hierarchy-bound reset did not update shared net");
  hierarchy_engine.schedule_at(1, [=](Engine& runtime) { runtime.write_now(top_clk, LogicWord::known(0, 1)); });
  hierarchy_engine.schedule_at(2, [=](Engine& runtime) { runtime.write_now(top_rst_n, LogicWord::known(1, 1)); });
  hierarchy_engine.schedule_at(3, [=](Engine& runtime) { runtime.write_now(top_d, LogicWord::known(0x3c, 8)); });
  hierarchy_engine.schedule_at(4, [=](Engine& runtime) { runtime.write_now(top_clk, LogicWord::known(1, 1)); });
  hierarchy_engine.run();
  require(hierarchy_engine.read(register_instance.at("q")) == LogicWord::known(0x3c, 8), "hierarchy register output did not update");
  require(hierarchy_engine.read(observer_instance.at("o")) == LogicWord::known(0xc3, 8), "cross-instance dependency did not wake observer");
  const ModuleIR ir_part_selects{
      "ir_part_selects",
      {{"high", 4, LogicWord::x(4)}, {"low", 4, LogicWord::x(4)}, {"word", 8, LogicWord::x(8)}},
      {{"parts:comb", ProcessKind::combinational, "", "", {
          {"word", Expr::constant(LogicWord::known(0, 8)), std::nullopt},
          {"word", Expr::variable("high"), std::nullopt, {{7, 4}}},
          {"word", Expr::variable("low"), std::nullopt, {{3, 0}}},
      }, {}}}};
  Engine parts_engine;
  const auto parts = compile_ir(ir_part_selects, parts_engine);
  parts_engine.write_now(parts.at("high"), LogicWord::known(0xa, 4));
  parts_engine.write_now(parts.at("low"), LogicWord::known(0x5, 4));
  parts_engine.run();
  require(parts_engine.read(parts.at("word")) == LogicWord::known(0xa5, 8), "IR combinational part-select writes did not compose a word");
  parts_engine.write_now(parts.at("low"), LogicWord::x(4));
  parts_engine.run();
  require(parts_engine.read(parts.at("word")) == LogicWord{0xa0, 0x0f, 0, 8}, "IR part-select write did not preserve four-state masks");
  const ModuleIR ir_clocked_part_selects{
      "ir_clocked_part_selects",
      {{"clk", 1, LogicWord::x(1)}, {"high", 4, LogicWord::x(4)}, {"low", 4, LogicWord::x(4)}, {"q", 8, LogicWord::known(0, 8)}},
      {{"parts:ff", ProcessKind::posedge, "clk", "", {
          {"q", Expr::variable("high"), std::nullopt, {{7, 4}}},
          {"q", Expr::variable("low"), std::nullopt, {{3, 0}}},
          {"q", Expr::constant(LogicWord::known(0xb, 4)), std::nullopt, {{7, 4}}},
      }, {}}}};
  Engine clocked_parts_engine;
  const auto clocked_parts = compile_ir(ir_clocked_part_selects, clocked_parts_engine);
  clocked_parts_engine.write_now(clocked_parts.at("clk"), LogicWord::known(0, 1));
  clocked_parts_engine.write_now(clocked_parts.at("high"), LogicWord::known(0xa, 4));
  clocked_parts_engine.write_now(clocked_parts.at("low"), LogicWord::known(0x5, 4));
  clocked_parts_engine.write_now(clocked_parts.at("clk"), LogicWord::known(1, 1));
  clocked_parts_engine.run();
  require(clocked_parts_engine.read(clocked_parts.at("q")) == LogicWord::known(0xb5, 8), "clocked part-select NBA writes did not merge or preserve last-write-wins ordering");
  Engine memory_engine;
  const auto memory_address = memory_engine.add_signal("memory.address", LogicWord::known(0, 4));
  const auto memory_data = memory_engine.add_signal("memory.data", LogicWord::x(8));
  const auto scratch = memory_engine.add_memory("memory.scratch", 8, 16, LogicWord::known(0, 8));
  const auto memory_read = memory_engine.add_process("memory:read", {memory_address}, {scratch}, [=](Engine& runtime) {
    runtime.write_now(memory_data, runtime.read_memory(scratch, runtime.read(memory_address).as_u64()));
  });
  memory_engine.schedule_active(memory_read);
  memory_engine.run();
  require(memory_engine.read(memory_data) == LogicWord::known(0, 8), "memory read process did not initialize");
  memory_engine.write_memory_now(scratch, 0, LogicWord::known(0x5a, 8));
  memory_engine.run();
  require(memory_engine.read(memory_data) == LogicWord::known(0x5a, 8), "memory write did not wake dependent process");
  memory_engine.write_now(memory_address, LogicWord::known(1, 4));
  memory_engine.run();
  require(memory_engine.read(memory_data) == LogicWord::known(0, 8), "memory read did not wake on address change");
  const ModuleIR ir_rom_reader{
      "ir_rom_reader",
      {{"address", 4, LogicWord::x(4)}, {"instruction", 8, LogicWord::x(8)}},
      {{"rom:read", ProcessKind::combinational, "", "", {
          {"instruction", Expr::memory_read("rom", Expr::variable("address")), std::nullopt},
      }, {}}}};
  Engine ir_memory_engine;
  const auto rom = ir_memory_engine.add_memory("top.rom", 8, 16, LogicWord::known(0, 8));
  ir_memory_engine.write_memory_now(rom, 3, LogicWord::known(0x93, 8));
  const auto ir_memory = compile_ir(ir_rom_reader, ir_memory_engine, {}, {{"rom", rom}});
  ir_memory_engine.write_now(ir_memory.at("address"), LogicWord::known(3, 4));
  ir_memory_engine.run();
  require(ir_memory_engine.read(ir_memory.at("instruction")) == LogicWord::known(0x93, 8), "IR memory-read expression did not fetch memory data");
  ir_memory_engine.write_memory_now(rom, 3, LogicWord::known(0x13, 8));
  ir_memory_engine.run();
  require(ir_memory_engine.read(ir_memory.at("instruction")) == LogicWord::known(0x13, 8), "IR memory read was not sensitive to memory writes");
  ir_memory_engine.write_now(ir_memory.at("address"), LogicWord::x(4));
  ir_memory_engine.run();
  require(ir_memory_engine.read(ir_memory.at("instruction")) == LogicWord::x(8), "IR memory read did not return X for unknown address");
  const std::string ansi_rtl = R"(
    module and_gate(input logic a, input logic b, output logic y);
      assign y = a & b;
    endmodule
  )";
  Engine ansi;
  const auto ansi_model = RestrictedModule::parse(ansi_rtl);
  const auto ansi_signals = ansi_model.instantiate(ansi);
  ansi.write_now(ansi_signals.at("a"), LogicWord::known(1, 1));
  ansi.write_now(ansi_signals.at("b"), LogicWord::known(1, 1));
  ansi.run();
  require(ansi.read(ansi_signals.at("y")) == LogicWord::known(1, 1), "ANSI port declarations failed");
  const std::string parameterized_rtl = R"(
    module ibex_style #(parameter int unsigned DataWidth = 32) (
      output logic [DataWidth-1:0] data_o
    );
      assign data_o = '0;
    endmodule
  )";
  bool parameterized_rejected = false;
  try { (void)RestrictedModule::parse(parameterized_rtl); }
  catch (const std::invalid_argument& error) {
    parameterized_rejected = std::string(error.what()).find("parameterized packed width") != std::string::npos;
  }
  require(parameterized_rejected, "parameterized RTL did not receive a precise unsupported-construct diagnostic");
  std::cout << "PASS: active/NBA ordering, sensitivity wakeups, timed events, waves, and X safety\n";
}
