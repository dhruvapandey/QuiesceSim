#include "quiescesim/ir.hpp"

#include <iostream>
#include <unordered_map>

namespace quiescesim {
ModuleIR build_ibex_alu();
}

int main() {
  using namespace quiescesim;
  Engine engine;
  const auto imd_input = engine.add_memory("ibex_alu.imd_val_q_i", 32, 2, LogicWord::known(0, 32));
  const auto imd_output = engine.add_memory("ibex_alu.imd_val_d_o", 32, 2, LogicWord::x(32));
  const auto signals = compile_ir(build_ibex_alu(), engine, {},
                                  {{"imd_val_q_i", imd_input}, {"imd_val_d_o", imd_output}});
  const auto write = [&](const char* name, std::uint64_t value, std::uint8_t width) {
    engine.write_now(signals.at(name), LogicWord::known(value, width));
  };
  write("operator_i", 0, 7);
  write("operand_a_i", 1, 32);
  write("operand_b_i", 2, 32);
  write("instr_first_cycle_i", 1, 1);
  write("multdiv_operand_a_i", 0, 33);
  write("multdiv_operand_b_i", 0, 33);
  write("multdiv_sel_i", 0, 1);
  engine.run();
  if (engine.read(signals.at("result_o")) != LogicWord::known(3, 32)) return 3;
  if (engine.read_memory(imd_output, 0) != LogicWord::known(0, 32)) return 1;
  if (engine.read_memory(imd_output, 1) != LogicWord::known(0, 32)) return 2;
  std::cout << "PASS: generated resolved Ibex ALU executed natively\n";
}
