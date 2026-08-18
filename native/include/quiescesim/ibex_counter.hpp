#pragma once

#include "quiescesim/engine.hpp"
#include "quiescesim/ir.hpp"

namespace quiescesim {

// Hand-lowered reference kernel for the resolved Ibex ibex_counter variant
// CounterWidth=64, ProvideValUpd=1. This is a temporary oracle for native
// runtime validation; it is intentionally separate from the future general
// resolved-AST lowering pipeline.
struct IbexCounter64Ports {
  SignalId clk_i;
  SignalId rst_ni;
  SignalId counter_inc_i;
  SignalId counterh_we_i;
  SignalId counter_we_i;
  SignalId counter_val_i;
  SignalId counter_val_o;
  SignalId counter_val_upd_o;
};

ModuleIR ibex_counter64_ir();
IbexCounter64Ports instantiate_ibex_counter64(Engine& engine);

}  // namespace quiescesim
