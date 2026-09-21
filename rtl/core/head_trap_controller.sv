`default_nettype none
module head_trap_controller (
  input wire clk_i, rst_i, cancel_i,
  input wire [1:0] retire_count_i,
  input wire fault_valid_i,
  input commit_event_pkg::commit_event_t fault_event_i,
  output wire trap_valid_o,
  input wire trap_ready_i,
  output wire trap_accept_o,
  output commit_event_pkg::commit_event_t trap_event_o
);
  import commit_event_pkg::*;
  wire request_ready, response_valid, csr_accept, csr_trap, legal;
  wire [31:0] next_pc;
  commit_csr_effect_t [3:0] effects;
  commit_event_t saved_q;
  wire prepare = fault_valid_i && request_ready;
  assign trap_valid_o = response_valid && fault_valid_i;
  assign trap_accept_o = trap_valid_o && trap_ready_i;

  csr_two_wide state_bank (
    .clk_i, .rst_i, .cancel_i, .retire_count_i,
    .request_valid_i(fault_valid_i && !cancel_i), .request_ready_o(request_ready),
    .instruction_i(fault_event_i.instruction), .source_i(32'd0), .pc_i(fault_event_i.pc_before),
    .trap_i(1'b1), .cause_i(fault_event_i.trap_cause), .trap_value_i(fault_event_i.trap_value),
    .response_valid_o(response_valid), .response_ready_i(trap_ready_i && fault_valid_i),
    .accept_o(csr_accept), .retired_o(), .trap_accept_o(csr_trap),
    .legal_o(legal), .read_o(), .value_o(), .next_pc_o(next_pc), .effects_o(effects),
    .mtvec_o(), .mepc_o()
  );
  always_ff @(posedge clk_i) begin
    if (prepare) saved_q <= fault_event_i;
  end
  always_comb begin
    trap_event_o = '0;
    if (trap_valid_o) begin
      trap_event_o = saved_q;
      trap_event_o.valid = 1;
      trap_event_o.retired = 0;
      trap_event_o.pc_after = next_pc;
      trap_event_o.csr_effects = effects;
    end
  end
`ifndef SYNTHESIS
  always_ff @(posedge clk_i) begin
    if (!rst_i && !cancel_i) begin
      assert (!fault_valid_i || (fault_event_i.valid && fault_event_i.trap && !fault_event_i.retired))
        else $fatal(1, "HEAD_TRAP_INPUT");
      assert (!response_valid || (fault_valid_i && legal && fault_event_i == saved_q))
        else $fatal(1, "HEAD_TRAP_HELD");
      assert (trap_accept_o == csr_accept && trap_accept_o == csr_trap)
        else $fatal(1, "HEAD_TRAP_ATOMIC");
    end
  end
`endif
endmodule
`default_nettype wire
