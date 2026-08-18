`timescale 1ns/1ps

module ibex_counter64_tb;
  logic clk_i;
  logic rst_ni;
  logic counter_inc_i;
  logic counterh_we_i;
  logic counter_we_i;
  logic [31:0] counter_val_i;
  logic [63:0] counter_val_o;
  logic [63:0] counter_val_upd_o;

  ibex_counter #(.CounterWidth(64), .ProvideValUpd(1)) dut (.*);

  initial begin
    $dumpfile("ibex-counter64-reference.vcd");
    $dumpvars(0, ibex_counter64_tb);
    clk_i = 0;
    rst_ni = 0;
    counter_inc_i = 0;
    counterh_we_i = 0;
    counter_we_i = 0;
    counter_val_i = '0;
    #10 clk_i = 1;
    #1 clk_i = 0;
    rst_ni = 1;
    counter_inc_i = 1;
    #9 clk_i = 1;
    #1;
    if (counter_val_o !== 64'd1) $fatal(1, "counter value mismatch: %h", counter_val_o);
    if (counter_val_upd_o !== 64'd2) $fatal(1, "incremented value mismatch: %h", counter_val_upd_o);
    clk_i = 0;
    counter_inc_i = 0;
    counter_we_i = 1;
    counter_val_i = 32'hdead_beef;
    #10 clk_i = 1;
    #1;
    if (counter_val_o !== 64'h0000_0000_dead_beef) $fatal(1, "low-word write mismatch: %h", counter_val_o);
    clk_i = 0;
    counter_we_i = 0;
    counterh_we_i = 1;
    counter_val_i = 32'hcafe_babe;
    #10 clk_i = 1;
    #1;
    if (counter_val_o !== 64'hcafe_babe_dead_beef) $fatal(1, "high-word write mismatch: %h", counter_val_o);
    $finish;
  end
endmodule
