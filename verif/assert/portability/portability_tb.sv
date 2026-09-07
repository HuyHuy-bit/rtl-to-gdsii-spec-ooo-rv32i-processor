`default_nettype none
module portability_tb;
  bit clk;
  bit rst, advance;
  bit [7:0] value;
  wire initialized;
  assert_portability #(.MUTATION(`MUTATION)) dut (
    .clk_i(clk), .rst_i(rst), .advance_i(advance), .value_i(value),
    .initialized_o(initialized)
  );
  always #5 clk = !clk;
  initial begin
    rst = 1;
    for (int cycle = 0; cycle < 24; cycle++) begin
      @(negedge clk);
      rst = cycle < 2 || cycle == 12 || cycle == 13;
      advance = cycle % 3 == 0;
      value = 8'(cycle * 17);
    end
    @(negedge clk);
    assert (initialized) else $fatal(1, "PORT_TEST_INCOMPLETE");
    $display("PORTABILITY PASS cycles=24 reset_pulses=2");
    $finish;
  end
endmodule
`default_nettype wire
