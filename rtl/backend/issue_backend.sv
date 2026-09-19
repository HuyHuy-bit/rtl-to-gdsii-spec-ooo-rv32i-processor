`default_nettype none
module issue_backend (
  input wire clk_i, rst_i, flush_i, drained_i, resources_ready_i,
  input wire [1:0] valid_i, cfi_i, solo_i,
  input wire [9:0] rs1_i, rs2_i, rd_i,
  input wire [63:0] pc_i,
  input wire [3:0] eligible_i,
  input wire [127:0] payload_i,
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
  input wire trap_ready_i,
  output wire trap_valid_o, trap_accept_o,
  output commit_event_pkg::commit_event_t trap_event_o,
  input wire [1:0] port_ready_i,
  output wire [1:0] issue_o,
  output wire [25:0] issue_id_o,
  output wire [11:0] issue_destination_o,
  output wire [23:0] issue_source_o,
  output wire [127:0] issue_payload_o,
  output wire [127:0] read_data_o,
  output wire [3:0] read_ready_o,
  output wire head_valid_o,
  output wire [12:0] head_id_o,
  output wire [31:0] head_pc_o,
  output wire [5:0] occupancy_o,
  output wire [4:0] issue_occupancy_o,
  output wire identity_drain_o,
  output wire [191:0] rat_o, committed_o,
  output wire [63:0] free_o, ready_o,
  output wire checkpoint_accept_o,
  output wire [2:0] checkpoint_id_o,
  output wire [7:0] checkpoint_valid_o, checkpoint_released_o
);
  wire [1:0] dispatch_ready;
  wire recover = flush_i || trap_accept_o;

  // The queue must hold the whole renamed prefix, so its credits join the allocation stall.
  wire [1:0] selected = {valid_i[1] && !cfi_i[0], valid_i[0]};

  backend_two_wide backend (
    .clk_i, .rst_i, .flush_i, .drained_i,
    .resources_ready_i(resources_ready_i && (selected & ~dispatch_ready) == 0),
    .valid_i, .cfi_i, .solo_i, .rs1_i, .rs2_i, .rd_i, .pc_i,
    .allocate_accept_o, .allocate_id_o,
    .source1_o, .source2_o, .destination_o, .stale_o, .source_ready_o,
    .complete_offer_i, .complete_solo_i, .complete_id_i, .complete_event_i,
    .complete_accept_o, .wb_accept_o, .wb_destination_o, .wb_data_o,
    .resolve_offer_i, .resolve_grant_i, .mispredict_i, .resolve_id_i,
    .resolve_accept_o, .branch_recover_o,
    .retire_ready_i, .retire_valid_o, .retire_accept_o, .retire_rd_o,
    .retire_destination_o, .retire_stale_o, .retire_event_o,
    .trap_ready_i, .trap_valid_o, .trap_accept_o, .trap_event_o,
    .read_address_i(issue_source_o), .read_data_o, .read_ready_o,
    .head_valid_o, .head_id_o, .head_pc_o, .occupancy_o, .identity_drain_o,
    .rat_o, .committed_o, .free_o, .ready_o,
    .checkpoint_accept_o, .checkpoint_id_o, .checkpoint_valid_o, .checkpoint_released_o
  );

  // Accepted allocation dispatches renamed tags and readiness; selection drives the read ports.
  issue_queue queue (
    .clk_i, .rst_i, .flush_i(recover), .recover_i(branch_recover_o),
    .head_slot_i(head_id_o[4:0]), .recover_slot_i(resolve_id_i[4:0]),
    .dispatch_i(allocate_accept_o), .dispatch_ready_o(dispatch_ready), .dispatch_id_i(allocate_id_o),
    .dispatch_source_i({source2_o[11:6], source1_o[11:6], source2_o[5:0], source1_o[5:0]}),
    .dispatch_ready_i(source_ready_o), .dispatch_eligible_i(eligible_i),
    .dispatch_destination_i(destination_o), .dispatch_payload_i(payload_i),
    .wb_accept_i(wb_accept_o), .wb_destination_i(wb_destination_o),
    .port_ready_i, .issue_o, .issue_id_o, .read_address_o(issue_source_o),
    .issue_destination_o, .issue_payload_o, .occupancy_o(issue_occupancy_o)
  );

`ifndef SYNTHESIS
  always_ff @(posedge clk_i) begin
    if (!rst_i) begin
      for (int port_id = 0; port_id < 2; port_id++)
        assert (!issue_o[port_id] || &read_ready_o[port_id*2 +: 2])
          else $fatal(1, "ISSUE_BACKEND_READ");
      assert (!drained_i || issue_occupancy_o == 0) else $fatal(1, "ISSUE_BACKEND_DRAIN");
    end
  end
`endif
endmodule
`default_nettype wire
