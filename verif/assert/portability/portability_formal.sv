`default_nettype none
module portability_formal(input wire clk, rst, advance, input wire [7:0] value);
  wire initialized;
  logic past_valid = 0;
  logic previous_rst;
  logic [7:0] previous_value;
  assert_portability #(.MUTATION(`MUTATION)) dut (
    .clk_i(clk), .rst_i(rst), .advance_i(advance), .value_i(value),
    .initialized_o(initialized)
  );
  always @(posedge clk) begin
    past_valid <= 1;
    previous_rst <= rst;
    previous_value <= value;
    if (!past_valid) assume (rst);
    reset_reentry: cover (past_valid && initialized && rst);
    stalled: cover (past_valid && initialized && !rst && !advance);
    sample_change: cover (past_valid && initialized && !rst && !previous_rst && value != previous_value);
  end
endmodule
`default_nettype wire
