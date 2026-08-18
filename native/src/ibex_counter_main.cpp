#include "quiescesim/ibex_counter.hpp"

#include <iostream>

using namespace quiescesim;

int main() {
  try {
    Engine engine;
    const auto ports = instantiate_ibex_counter64(engine);
    engine.write_now(ports.clk_i, LogicWord::known(0, 1));
    engine.write_now(ports.rst_ni, LogicWord::known(0, 1));
    engine.write_now(ports.counter_inc_i, LogicWord::known(0, 1));
    engine.write_now(ports.counterh_we_i, LogicWord::known(0, 1));
    engine.write_now(ports.counter_we_i, LogicWord::known(0, 1));
    engine.write_now(ports.counter_val_i, LogicWord::known(0, 32));
    engine.write_now(ports.clk_i, LogicWord::known(1, 1));
    engine.run();

    engine.schedule_at(1, [=](Engine& sim) { sim.write_now(ports.clk_i, LogicWord::known(0, 1)); });
    engine.schedule_at(2, [=](Engine& sim) { sim.write_now(ports.rst_ni, LogicWord::known(1, 1)); });
    engine.schedule_at(3, [=](Engine& sim) { sim.write_now(ports.counter_inc_i, LogicWord::known(1, 1)); });
    engine.schedule_at(4, [=](Engine& sim) { sim.write_now(ports.clk_i, LogicWord::known(1, 1)); });
    engine.run();
    const auto value = engine.read(ports.counter_val_o).as_u64();
    const auto updated = engine.read(ports.counter_val_upd_o).as_u64();
    std::cout << "counter_val_o=" << value << "\n"
              << "counter_val_upd_o=" << updated << "\n"
              << "end_time=" << engine.now() << "\n";
    return (value == 1 && updated == 2) ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "Ibex counter native run failed: " << error.what() << '\n';
    return 1;
  }
}
