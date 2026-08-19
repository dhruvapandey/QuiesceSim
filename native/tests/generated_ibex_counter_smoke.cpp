#include "quiescesim/ir.hpp"

#include <iostream>

namespace quiescesim {
ModuleIR generated_ibex_counter_ir();
}

int main() {
  using namespace quiescesim;
  Engine engine;
  const auto signals = compile_ir(generated_ibex_counter_ir(), engine);
  const auto write = [&](const char* name, std::uint64_t value, std::uint8_t width) {
    engine.write_now(signals.at(name), LogicWord::known(value, width));
  };
  write("clk_i", 0, 1);
  write("rst_ni", 0, 1);
  write("counter_inc_i", 0, 1);
  write("counterh_we_i", 0, 1);
  write("counter_we_i", 0, 1);
  write("counter_val_i", 0, 32);
  write("clk_i", 1, 1);
  engine.run();
  if (engine.read(signals.at("counter_val_o")) != LogicWord::known(0, 64)) return 1;
  engine.schedule_at(1, [=](Engine& runtime) { runtime.write_now(signals.at("clk_i"), LogicWord::known(0, 1)); });
  engine.schedule_at(2, [=](Engine& runtime) { runtime.write_now(signals.at("rst_ni"), LogicWord::known(1, 1)); });
  engine.schedule_at(3, [=](Engine& runtime) { runtime.write_now(signals.at("counter_inc_i"), LogicWord::known(1, 1)); });
  engine.schedule_at(4, [=](Engine& runtime) { runtime.write_now(signals.at("clk_i"), LogicWord::known(1, 1)); });
  engine.run();
  if (engine.read(signals.at("counter_val_o")) != LogicWord::known(1, 64)) return 2;
  engine.schedule_at(5, [=](Engine& runtime) { runtime.write_now(signals.at("clk_i"), LogicWord::known(0, 1)); });
  engine.schedule_at(6, [=](Engine& runtime) { runtime.write_now(signals.at("counter_inc_i"), LogicWord::known(0, 1)); });
  engine.schedule_at(7, [=](Engine& runtime) { runtime.write_now(signals.at("counter_we_i"), LogicWord::known(1, 1)); });
  engine.schedule_at(8, [=](Engine& runtime) { runtime.write_now(signals.at("counter_val_i"), LogicWord::known(0xdeadbeef, 32)); });
  engine.schedule_at(9, [=](Engine& runtime) { runtime.write_now(signals.at("clk_i"), LogicWord::known(1, 1)); });
  engine.run();
  if (engine.read(signals.at("counter_val_o")) != LogicWord::known(0x00000000deadbeefULL, 64)) return 3;
  std::cout << "PASS: generated resolved Ibex counter executed natively\n";
}
