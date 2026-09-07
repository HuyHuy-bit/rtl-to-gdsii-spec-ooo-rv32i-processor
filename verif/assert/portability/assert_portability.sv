`default_nettype none
module assert_portability #(
  parameter int MUTATION = 0
) (
  input wire clk_i, rst_i, advance_i,
  input wire [7:0] value_i,
  output logic initialized_o
);
  logic [3:0] mask_q, previous_mask_q;
  logic [1:0] index_q;
  logic [7:0] value_q, previous_value_q;
  logic previous_advance_q, history_valid_q;
  wire [1:0] accepted = advance_i ? (MUTATION == 1 ? 2'b11 : 2'b01) : 2'b10;
  wire [3:0] active_mask = rst_i ? 4'b0 : mask_q;
  wire [2:0] count = MUTATION == 2 ? 3'd2 : 3'd1;
  wire [1:0] selected_index = MUTATION == 6 ? index_q + 2'd1 : index_q;

  always_ff @(posedge clk_i) begin
    if (rst_i) begin
      mask_q <= 4'b0001;
      index_q <= 0;
      initialized_o <= 0;
      history_valid_q <= 0;
    end else begin
      initialized_o <= 1;
      history_valid_q <= initialized_o;
      if (advance_i || MUTATION == 5) begin
        mask_q <= {mask_q[2:0], mask_q[3]};
        index_q <= index_q + 2'd1;
      end
    end
    value_q <= value_i;
    previous_value_q <= value_i;
    previous_mask_q <= mask_q;
    previous_advance_q <= advance_i;
  end

`ifndef SYNTHESIS
  // Clocked assertions observe state before this edge's nonblocking updates.
  always_ff @(posedge clk_i) begin
    no_duplicate: assert (!(accepted[0] && accepted[1])) else $fatal(1, "PORT_DUPLICATE");
    if (initialized_o && (!rst_i || MUTATION == 3)) begin
      population: assert ($countones(active_mask) == int'(count)) else $fatal(1, "PORT_COUNT");
    end
    if (initialized_o && !rst_i) begin
      indexed: assert (mask_q[selected_index]) else $fatal(1, "PORT_INDEX");
      sampled: assert (value_q == (MUTATION == 4 ? value_i : previous_value_q))
        else $fatal(1, "PORT_SAMPLE");
      if (history_valid_q && !previous_advance_q)
        held: assert (mask_q == previous_mask_q) else $fatal(1, "PORT_HOLD");
    end
  end
`endif
endmodule
`default_nettype wire
