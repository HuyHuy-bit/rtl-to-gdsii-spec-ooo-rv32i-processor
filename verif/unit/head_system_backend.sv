`default_nettype none
module head_system_backend (
  input wire clk_i, rst_i, flush_i, drained_i, resources_ready_i,
  input wire [1:0] valid_i, cfi_i, solo_i,
  input wire [9:0] rs1_i, rs2_i, rd_i,
  input wire [63:0] pc_i,
  output wire [1:0] allocate_accept_o,
  output wire [25:0] allocate_id_o,
  output wire [11:0] source1_o, source2_o, destination_o, stale_o,
  output wire [3:0] source_ready_o,
  input wire [1:0] complete_offer_i, complete_solo_i,
  input wire [25:0] complete_id_i,
  input commit_event_pkg::commit_event_t [1:0] complete_event_i,
  output wire [1:0] complete_accept_o, wb_accept_o,
  output wire [11:0] wb_destination_o,
  output wire [63:0] wb_data_o,
  input wire resolve_offer_i, resolve_grant_i, mispredict_i,
  input wire [12:0] resolve_id_i,
  output wire resolve_accept_o, branch_recover_o,
  input wire [1:0] retire_ready_i,
  output wire [1:0] retire_valid_o, retire_accept_o,
  output wire [9:0] retire_rd_o,
  output wire [11:0] retire_destination_o, retire_stale_o,
  output commit_event_pkg::commit_event_t [1:0] retire_event_o,
  input wire system_valid_i, illegal_ready_i,
  input wire [12:0] system_id_i,
  input wire [31:0] system_instruction_i,
  output wire system_prepare_o, system_busy_o, serial_offer_o, serial_accept_o,
  output wire illegal_offer_o, illegal_accept_o,
  output commit_event_pkg::commit_event_t serial_event_o, illegal_event_o,
  output wire redirect_o,
  output wire [31:0] redirect_pc_o,
  input wire trap_ready_i,
  output wire trap_valid_o, trap_accept_o,
  output commit_event_pkg::commit_event_t trap_event_o,
  input wire [23:0] read_address_i,
  output wire [127:0] read_data_o,
  output logic [3:0] read_ready_o,
  output wire head_valid_o,
  output wire [12:0] head_id_o,
  output wire [31:0] head_pc_o,
  output wire [5:0] occupancy_o,
  output wire identity_drain_o,
  output wire [191:0] rat_o, committed_o,
  output wire [63:0] free_o, ready_o,
  output wire checkpoint_accept_o,
  output wire [2:0] checkpoint_id_o,
  output wire [7:0] checkpoint_valid_o, checkpoint_released_o
);

  import commit_event_pkg::*;
  wire serial_ready, raw_trap_valid, raw_trap_accept;
  wire [12:0] serial_id, illegal_id;
  wire [1:0] complete_accept;
  commit_event_t raw_trap_event;
  wire [1:0] offers = illegal_offer_o ? {1'b0, illegal_ready_i} : complete_offer_i;
  wire [25:0] ids = illegal_offer_o ? {13'd0, illegal_id} : complete_id_i;
  wire [1:0] solos = illegal_offer_o ? 2'b01 : complete_solo_i;
  commit_event_t [1:0] events;
  assign events = illegal_offer_o ? {commit_event_t'('0), illegal_event_o} : complete_event_i;
  assign complete_accept_o = illegal_offer_o ? 2'b00 : complete_accept;
  assign illegal_accept_o = illegal_offer_o && complete_accept[0];
  assign trap_accept_o = trap_valid_o && trap_ready_i;

  head_system_controller controller (
    .clk_i, .rst_i, .cancel_i(flush_i), .retire_accept_i(retire_accept_o),
    .head_valid_i(head_valid_o), .head_id_i(head_id_o), .head_pc_i(head_pc_o),
    .system_valid_i, .system_id_i, .instruction_i(system_instruction_i),
    .source_ready_i(read_ready_o[0]), .source_i(read_data_o[31:0]),
    .prepare_o(system_prepare_o), .busy_o(system_busy_o),
    .fault_valid_i(raw_trap_valid), .fault_event_i(raw_trap_event),
    .serial_offer_o, .serial_id_o(serial_id), .serial_event_o, .serial_accept_i(serial_accept_o),
    .illegal_offer_o, .illegal_id_o(illegal_id), .illegal_event_o, .illegal_accept_i(illegal_accept_o),
    .trap_valid_o, .trap_event_o, .trap_accept_i(trap_accept_o), .redirect_o, .redirect_pc_o
  );
  backend_two_wide backend (
    .clk_i,
    .rst_i,
    .flush_i,
    .drained_i,
    .resources_ready_i,
    .valid_i,
    .cfi_i,
    .solo_i,
    .rs1_i,
    .rs2_i,
    .rd_i,
    .pc_i,
    .allocate_accept_o,
    .allocate_id_o,
    .source1_o,
    .source2_o,
    .destination_o,
    .stale_o,
    .source_ready_o,
    .complete_offer_i(offers),
    .complete_solo_i(solos),
    .complete_id_i(ids),
    .complete_event_i(events),
    .complete_accept_o(complete_accept),
    .wb_accept_o,
    .wb_destination_o,
    .wb_data_o,
    .resolve_offer_i,
    .resolve_grant_i(resolve_grant_i && !system_busy_o && !raw_trap_valid),
    .mispredict_i,
    .resolve_id_i,
    .resolve_accept_o,
    .branch_recover_o,
    .retire_ready_i,
    .retire_valid_o,
    .retire_accept_o,
    .retire_rd_o,
    .retire_destination_o,
    .retire_stale_o,
    .retire_event_o,
    .serial_offer_i(serial_offer_o),
    .serial_id_i(serial_id),
    .serial_event_i(serial_event_o),
    .serial_ready_o(serial_ready),
    .serial_accept_o,
    .trap_ready_i(trap_accept_o),
    .trap_valid_o(raw_trap_valid),
    .trap_accept_o(raw_trap_accept),
    .trap_event_o(raw_trap_event),
    .read_address_i,
    .read_data_o,
    .read_ready_o,
    .head_valid_o,
    .head_id_o,
    .head_pc_o,
    .occupancy_o,
    .identity_drain_o,
    .rat_o,
    .committed_o,
    .free_o,
    .ready_o,
    .checkpoint_accept_o,
    .checkpoint_id_o,
    .checkpoint_valid_o,
    .checkpoint_released_o
  );
`ifndef SYNTHESIS
  always_ff @(posedge clk_i) if (!rst_i) begin
    assert (trap_accept_o == raw_trap_accept) else $fatal(1, "SYSTEM_BACKEND_TRAP");
    assert (!serial_accept_o || serial_ready) else $fatal(1, "SYSTEM_BACKEND_SERIAL");
  end
`endif
endmodule
`default_nettype wire
