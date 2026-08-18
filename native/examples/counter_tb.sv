`timescale 1ns/1ps

module counter_tb;
  logic clk;
  logic rst_n;
  logic en;
  logic [7:0] q;

  counter dut (.clk(clk), .rst_n(rst_n), .en(en), .q(q));

  initial begin
    $dumpfile("counter-reference.vcd");
    $dumpvars(0, counter_tb);
    clk = 0;
    rst_n = 0;
    en = 1;
    #10 clk = 1;
    #1 clk = 0;
    rst_n = 1;
    #9 clk = 1;
    #1 clk = 0;
    #9 clk = 1;
    #1 clk = 0;
    #9 clk = 1;
    #1 clk = 0;
    #1;
    if (q !== 8'h03) $fatal(1, "unexpected q=%h", q);
    $finish;
  end
endmodule
