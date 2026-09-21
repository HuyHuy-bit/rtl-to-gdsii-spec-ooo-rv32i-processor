`default_nettype none
module backend_two_wide (
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
  input wire serial_offer_i,
  input wire [12:0] serial_id_i,
  input commit_event_pkg::commit_event_t serial_event_i,
  output wire serial_ready_o, serial_accept_o,
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
  wire [1:0] rob_allocate_ready, rob_allocate_accept, rob_complete_ready;
  wire [11:0] rob_wb_destination;
  wire serial_selected = serial_offer_i && serial_ready_o;
  wire [1:0] wb_live = serial_selected ? 2'b01 : rob_complete_ready;
  assign wb_destination_o = serial_selected ? {6'd0, retire_destination_o[5:0]} : rob_wb_destination;
  wire recover = flush_i || trap_accept_o;
  wire [1:0] rename_commit, commit_request;
  wire rob_resolve_ready, rename_resolve, rename_recover;
  wire [63:0] allocation, reclaim;
  logic [1:0] selected, write_offer, completion_take;
  logic [2:0] checkpoint_q [32];

  always_comb begin
    selected = valid_i;
    if (cfi_i[0]) selected[1] = 0;
  end
  for (genvar lane = 0; lane < 2; lane++) begin : completion_control
    assign write_offer[lane] = serial_selected
        ? (lane == 0 && retire_ready_i[0] && serial_event_i.rd_write_mask != 0)
        : (complete_offer_i[lane] && !complete_event_i[lane].trap);
    assign completion_take[lane] = complete_offer_i[lane] && rob_complete_ready[lane]
        && (complete_event_i[lane].trap || wb_destination_o[lane*6 +: 6] == 0 || wb_accept_o[lane]);
  end
  always_comb begin
    for (int port_id = 0; port_id < 4; port_id++) begin
      read_ready_o[port_id] = ready_o[read_address_i[port_id*6 +: 6]]
        && !reclaim[read_address_i[port_id*6 +: 6]];
      for (int lane = 0; lane < 2; lane++)
        if (wb_accept_o[lane] && wb_destination_o[lane*6 +: 6] == read_address_i[port_id*6 +: 6])
          read_ready_o[port_id] = 1;
      if (rst_i || recover) read_ready_o[port_id] = 0;
    end
  end

  assign commit_request[0] = retire_valid_o[0] && retire_ready_i[0]
      && (!serial_selected || retire_destination_o[5:0] == 0 || wb_accept_o[0]);
  assign commit_request[1] = retire_valid_o[1] && retire_ready_i[1] && commit_request[0];
  assign wb_data_o = serial_selected ? {32'd0, serial_event_i.rd_value}
      : {complete_event_i[1].rd_value, complete_event_i[0].rd_value};

  // Checkpoint slots are looked up only through a validated, unresolved ROB identity.
  always_ff @(posedge clk_i) begin
    for (int lane = 0; lane < 2; lane++)
      if (allocate_accept_o[lane] && cfi_i[lane])
        checkpoint_q[allocate_id_o[lane*13 +: 5]] <= checkpoint_id_o;
  end

  rename_recovery rename_owner (
    .clk_i, .rst_i, .recover_i(recover),
    .resources_ready_i(resources_ready_i && !drained_i && (selected & ~rob_allocate_ready) == 0),
    .valid_i, .checkpoint_i(cfi_i), .rs1_i, .rs2_i, .rd_i,
    .wb_offer_i(write_offer), .wb_live_i(wb_live), .wb_destination_i(wb_destination_o),
    .wb_accept_o, .commit_i(commit_request), .commit_accept_o(rename_commit),
    .commit_rd_i(retire_rd_o), .commit_destination_i(retire_destination_o), .commit_stale_i(retire_stale_o),
    .resolve_i(resolve_offer_i && resolve_grant_i), .resolve_live_i(rob_resolve_ready),
    .mispredict_i, .resolve_id_i(checkpoint_q[resolve_id_i[4:0]]),
    .resolve_accept_o(rename_resolve), .branch_recover_o(rename_recover),
    .accept_o(allocate_accept_o), .source1_o, .source2_o, .destination_o, .stale_o, .source_ready_o,
    .allocation_o(allocation), .rat_o, .committed_o, .free_o, .ready_o,
    .checkpoint_accept_o, .checkpoint_id_o, .checkpoint_valid_o, .checkpoint_released_o, .reclaim_o(reclaim)
  );

  rob_two_wide rob (
    .clk_i, .rst_i, .flush_i, .drained_i,
    .allocate_i(allocate_accept_o), .allocate_cfi_i(cfi_i), .allocate_solo_i(solo_i),
    .allocate_rd_i(rd_i), .allocate_pc_i(pc_i), .allocate_destination_i(destination_o), .allocate_stale_i(stale_o),
    .allocate_ready_o(rob_allocate_ready), .allocate_accept_o(rob_allocate_accept), .allocate_id_o,
    .complete_offer_i, .complete_take_i(completion_take), .complete_solo_i, .complete_id_i, .complete_event_i,
    .complete_ready_o(rob_complete_ready), .complete_accept_o, .complete_destination_o(rob_wb_destination),
    .resolve_offer_i, .resolve_take_i(rename_resolve), .mispredict_i, .resolve_id_i,
    .resolve_ready_o(rob_resolve_ready), .resolve_accept_o, .branch_recover_o,
    .retire_take_i(rename_commit), .retire_valid_o, .retire_accept_o, .retire_rd_o,
    .retire_destination_o, .retire_stale_o, .retire_event_o,
    .serial_offer_i, .serial_id_i, .serial_event_i, .serial_ready_o, .serial_accept_o,
    .trap_take_i(trap_ready_i && trap_valid_o), .trap_valid_o, .trap_accept_o, .trap_event_o,
    .head_valid_o, .head_id_o, .head_pc_o, .occupancy_o, .identity_drain_o
  );

  prf_4r2w registers (
    .clk_i, .rst_i, .raddr_i(read_address_i), .rdata_o(read_data_o),
    .wb_accept_i(wb_accept_o), .waddr_i(wb_destination_o), .wdata_i(wb_data_o)
  );

`ifndef SYNTHESIS
  always_ff @(posedge clk_i) begin
    if (!rst_i) begin
      assert (allocate_accept_o == rob_allocate_accept && retire_accept_o == rename_commit
              && resolve_accept_o == rename_resolve && branch_recover_o == rename_recover)
        else $fatal(1, "BACKEND_ATOMIC_CONTROL");
      assert ((wb_accept_o & ~(complete_accept_o | {1'b0, serial_accept_o})) == 0 && checkpoint_accept_o == (|(allocate_accept_o & cfi_i)))
        else $fatal(1, "BACKEND_ATOMIC_RESULT");
      assert (!serial_accept_o || (retire_accept_o == 1 && complete_accept_o == 0
              && wb_accept_o == (retire_destination_o[5:0] == 0 ? 2'b00 : 2'b01)))
        else $fatal(1, "BACKEND_SERIAL_ATOMIC");
      assert (!drained_i || (!serial_offer_i && valid_i == 0 && complete_offer_i == 0 && !resolve_offer_i))
        else $fatal(1, "BACKEND_DRAIN");
    end
  end
`endif
  wire unused_allocation = ^allocation;
endmodule
`default_nettype wire
