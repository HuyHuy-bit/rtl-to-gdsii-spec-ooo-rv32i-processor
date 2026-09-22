`default_nettype none
module head_system_controller (
  input wire clk_i, rst_i, cancel_i,
  input wire [1:0] retire_accept_i,
  input wire head_valid_i,
  input wire [12:0] head_id_i,
  input wire [31:0] head_pc_i,
  input wire system_valid_i, source_ready_i,
  input wire [12:0] system_id_i,
  input wire [31:0] instruction_i, source_i,
  output wire prepare_o, busy_o,
  input wire fault_valid_i,
  input commit_event_pkg::commit_event_t fault_event_i,
  output wire serial_offer_o,
  output wire [12:0] serial_id_o,
  output commit_event_pkg::commit_event_t serial_event_o,
  input wire serial_accept_i,
  output wire illegal_offer_o,
  output wire [12:0] illegal_id_o,
  output commit_event_pkg::commit_event_t illegal_event_o,
  input wire illegal_accept_i,
  output wire trap_valid_o,
  output commit_event_pkg::commit_event_t trap_event_o,
  input wire trap_accept_i,
  output wire redirect_o,
  output wire [31:0] redirect_pc_o
);
  import single_lane_pkg::*;
  import commit_event_pkg::*;
  decoded_t decoded;
  commit_event_t saved_q, base_event;
  logic [12:0] id_q;
  logic trap_q, mret_q;
  wire request_ready, response_valid, legal, csr_accept, csr_retired, csr_trap;
  wire [31:0] value, next_pc;
  wire [64:0] unused_state;
  commit_csr_effect_t [3:0] effects;
  wire system_request = system_valid_i && source_ready_i && head_valid_i && system_id_i == head_id_i;
  wire request = fault_valid_i || system_request;
  wire prepare = request && request_ready;
  wire owner_live = head_valid_i && head_id_i == id_q && head_pc_i == saved_q.pc_before;
  wire accepted_serial = serial_offer_o && serial_accept_i;
  wire accepted_trap = trap_valid_o && trap_accept_i;
  wire accepted_illegal = illegal_offer_o && illegal_accept_i;

  decode_single decode (.instruction_i, .decoded_o(decoded));
  assign prepare_o = prepare && !fault_valid_i;
  assign busy_o = response_valid;
  assign serial_offer_o = response_valid && owner_live && !trap_q && legal;
  assign illegal_offer_o = response_valid && owner_live && !trap_q && !legal;
  assign trap_valid_o = response_valid && owner_live && trap_q && fault_valid_i;
  assign serial_id_o = id_q;
  assign illegal_id_o = id_q;
  assign redirect_o = accepted_trap || (accepted_serial && mret_q);
  assign redirect_pc_o = redirect_o ? next_pc : 32'd0;

  always_comb begin
    base_event = '0;
    base_event.valid = 1;
    base_event.privilege = 3;
    base_event.instruction = instruction_i;
    base_event.pc_before = head_pc_i;
    base_event.pc_after = head_pc_i;
    base_event.rs1_addr = decoded.rs1;
    base_event.rs1_value = decoded.rs1 == 0 ? 32'd0 : source_i;
    base_event.rd_addr = decoded.rd;
    if (prepare && fault_valid_i) base_event = fault_event_i;
  end

  always_ff @(posedge clk_i) begin
    if (prepare) begin
      saved_q <= base_event;
      id_q <= head_id_i;
      trap_q <= fault_valid_i;
      mret_q <= !fault_valid_i && decoded.op == OP_MRET;
    end
  end

  // An illegal CSR first completes as a raw fault; the next head request enters the trap.
  always_comb begin
    serial_event_o = '0;
    illegal_event_o = '0;
    trap_event_o = '0;
    if (serial_offer_o) begin
      serial_event_o = saved_q;
      serial_event_o.retired = 1;
      serial_event_o.pc_after = next_pc;
      serial_event_o.rd_value = saved_q.rd_addr == 0 ? 32'd0 : value;
      serial_event_o.rd_write_mask = saved_q.rd_addr == 0 ? 32'd0 : 32'hffffffff;
      serial_event_o.csr_effects = effects;
    end
    if (illegal_offer_o) begin
      illegal_event_o = saved_q;
      illegal_event_o.rd_addr = 0;
      illegal_event_o.trap = 1;
      illegal_event_o.trap_cause = 2;
      illegal_event_o.trap_value = saved_q.instruction;
    end
    if (trap_valid_o) begin
      trap_event_o = saved_q;
      trap_event_o.pc_after = next_pc;
      trap_event_o.csr_effects = effects;
    end
  end

  csr_two_wide state_bank (
    .clk_i, .rst_i, .cancel_i,
    .retire_count_i(2'($countones(retire_accept_i & ~{1'b0, serial_accept_i}))),
    .request_valid_i(request), .request_ready_o(request_ready),
    .instruction_i(fault_valid_i ? fault_event_i.instruction : instruction_i), .source_i,
    .pc_i(fault_valid_i ? fault_event_i.pc_before : head_pc_i),
    .trap_i(fault_valid_i), .cause_i(fault_event_i.trap_cause), .trap_value_i(fault_event_i.trap_value),
    .response_valid_o(response_valid), .response_ready_i(accepted_serial || accepted_illegal || accepted_trap),
    .accept_o(csr_accept), .retired_o(csr_retired), .trap_accept_o(csr_trap),
    .legal_o(legal), .read_o(unused_state[64]), .value_o(value), .next_pc_o(next_pc), .effects_o(effects),
    .mtvec_o(unused_state[63:32]), .mepc_o(unused_state[31:0])
  );
`ifndef SYNTHESIS
  always_ff @(posedge clk_i) begin
    if (!rst_i && !cancel_i) begin
      assert (!prepare_o || decoded.op inside {OP_CSR, OP_MRET}) else $fatal(1, "HEAD_SYSTEM_OPERATION");
      assert (!fault_valid_i || (head_valid_i && fault_event_i.valid && fault_event_i.trap
              && !fault_event_i.retired && fault_event_i.pc_before == head_pc_i))
        else $fatal(1, "HEAD_SYSTEM_FAULT");
      assert (!response_valid || owner_live) else $fatal(1, "HEAD_SYSTEM_OWNER");
      assert (!trap_valid_o || fault_event_i == saved_q) else $fatal(1, "HEAD_SYSTEM_HELD_FAULT");
      assert ((!serial_accept_i || serial_offer_o) && (!illegal_accept_i || illegal_offer_o)
              && (!trap_accept_i || trap_valid_o)) else $fatal(1, "HEAD_SYSTEM_ACCEPT");
      assert (csr_retired == accepted_serial && csr_trap == accepted_trap
              && csr_accept == (accepted_serial || accepted_trap || accepted_illegal))
        else $fatal(1, "HEAD_SYSTEM_ATOMIC");
      assert (!serial_accept_i || retire_accept_i == 1) else $fatal(1, "HEAD_SYSTEM_RETIRE");
    end
  end
`endif
endmodule
`default_nettype wire
