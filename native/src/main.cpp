#include "quiescesim/frontend.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

using namespace quiescesim;

int main(int argc, char** argv) {
  if (argc != 2 && argc != 4) {
    std::cerr << "usage: quiescesim_run <restricted-systemverilog-file> [--vcd <wave.vcd>]\n";
    return 2;
  }
  if (argc == 4 && std::string(argv[2]) != "--vcd") {
    std::cerr << "expected --vcd <wave.vcd>\n";
    return 2;
  }
  std::ifstream input(argv[1]);
  if (!input) { std::cerr << "cannot read " << argv[1] << '\n'; return 2; }
  std::stringstream source;
  source << input.rdbuf();
  try {
    Engine engine;
    const auto module = RestrictedModule::parse(source.str());
    const auto signals = module.instantiate(engine);
    if (!signals.contains("clk") || !signals.contains("rst_n")) {
      std::cerr << "runner requires declared clk and rst_n signals\n";
      return 2;
    }
    engine.write_now(signals.at("clk"), LogicWord::known(0, 1));
    engine.write_now(signals.at("rst_n"), LogicWord::known(0, 1));
    if (signals.contains("en")) engine.write_now(signals.at("en"), LogicWord::known(1, 1));
    for (std::uint64_t cycle = 1; cycle <= 4; ++cycle) {
      engine.schedule_at(cycle * 10, [=](Engine& sim) { sim.write_now(signals.at("clk"), LogicWord::known(1, 1)); });
      engine.schedule_at(cycle * 10 + 1, [=](Engine& sim) { sim.write_now(signals.at("clk"), LogicWord::known(0, 1)); });
    }
    engine.schedule_at(11, [=](Engine& sim) { sim.write_now(signals.at("rst_n"), LogicWord::known(1, 1)); });
    engine.run();
    std::cout << "module=" << module.name() << " end_time=" << engine.now() << "\n";
    for (const auto& [name, id] : signals) {
      const auto value = engine.read(id);
      std::cout << name << '=';
      if (value.is_known()) std::cout << "0x" << std::hex << value.as_u64() << std::dec;
      else std::cout << "X/Z";
      std::cout << '\n';
    }
    std::cout << "wave_changes=" << engine.waves().size() << '\n';
    std::cout << "guarded_evaluations_skipped=" << engine.skipped_guarded_evaluations() << '\n';
    if (argc == 4) {
      std::ofstream vcd(argv[3]);
      if (!vcd) { std::cerr << "cannot write VCD " << argv[3] << '\n'; return 2; }
      engine.write_vcd(vcd);
      std::cout << "vcd=" << argv[3] << '\n';
    }
  } catch (const std::exception& error) {
    std::cerr << "compile/run error: " << error.what() << '\n';
    return 1;
  }
}
