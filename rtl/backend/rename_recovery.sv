`default_nettype none
module rename_recovery (
  input wire clk_i, rst_i, recover_i, resources_ready_i,
  input wire [1:0] valid_i, checkpoint_i,
  input wire [9:0] rs1_i, rs2_i, rd_i,
  input wire [1:0] wb_offer_i, wb_live_i, commit_i,
  input wire [11:0] wb_destination_i, commit_destination_i, commit_stale_i,
  input wire [9:0] commit_rd_i,
  input wire resolve_i, resolve_live_i, mispredict_i,
  input wire [2:0] resolve_id_i,
  output wire [1:0] accept_o,
  output wire [11:0] source1_o, source2_o, destination_o, stale_o,
  output wire [3:0] source_ready_o,
  output wire [63:0] allocation_o,
  output wire [191:0] rat_o, committed_o,
  output wire [63:0] free_o, ready_o,
  output logic [1:0] wb_accept_o,
  output wire [1:0] commit_accept_o,
  output wire checkpoint_accept_o,
  output wire [2:0] checkpoint_id_o,
  output wire [7:0] checkpoint_valid_o, checkpoint_released_o,
  output wire resolve_accept_o, branch_recover_o,
  output wire [63:0] reclaim_o
);
  wire checkpoint_ready;
  wire needs_checkpoint = (valid_i[0] && checkpoint_i[0])
                          || (valid_i == 2'b11 && !checkpoint_i[0] && checkpoint_i[1]);
  wire reserve_ready = resources_ready_i && (!needs_checkpoint || checkpoint_ready);
  wire checkpoint_create = |(accept_o & checkpoint_i);
  wire [191:0] restore_rat;
  logic [191:0] snapshot;

  assign resolve_accept_o = resolve_i && resolve_live_i && checkpoint_valid_o[resolve_id_i]
                            && !rst_i && !recover_i;
  assign commit_accept_o = rst_i || recover_i || branch_recover_o ? 2'b00 : commit_i;

  // Surviving results may finish during branch recovery; killed tags cannot write or wake up.
  always_comb begin
    wb_accept_o = 0;
    for (int lane = 0; lane < 2; lane++) begin
      if (!rst_i && !recover_i && wb_offer_i[lane] && wb_live_i[lane]
          && wb_destination_i[lane*6 +: 6] != 0
          && !free_o[wb_destination_i[lane*6 +: 6]]
          && !ready_o[wb_destination_i[lane*6 +: 6]]
          && !reclaim_o[wb_destination_i[lane*6 +: 6]]) begin
        if (lane == 0 || !wb_accept_o[0] || wb_destination_i[11:6] != wb_destination_i[5:0])
          wb_accept_o[lane] = 1;
      end
    end
    snapshot = rat_o;
    for (int lane = 0; lane < 2; lane++)
      if (accept_o[lane] && rd_i[lane*5 +: 5] != 0)
        snapshot[rd_i[lane*5 +: 5]*6 +: 6] = destination_o[lane*6 +: 6];
  end

  rename_state state_owner (
    .clk_i, .rst_i, .recover_i, .resources_ready_i(reserve_ready),
    .branch_recover_i(branch_recover_o), .branch_rat_i(restore_rat), .branch_reclaim_i(reclaim_o),
    .valid_i, .checkpoint_i, .rs1_i, .rs2_i, .rd_i,
    .wb_accept_i(wb_accept_o), .commit_i(commit_accept_o),
    .wb_destination_i, .commit_destination_i, .commit_stale_i, .commit_rd_i,
    .accept_o, .source1_o, .source2_o, .destination_o, .stale_o, .source_ready_o,
    .allocation_o, .rat_o, .committed_o, .free_o, .ready_o
  );
  rename_checkpoints checkpoints (
    .clk_i, .rst_i, .flush_i(recover_i), .create_i(checkpoint_create),
    .snapshot_i(snapshot), .allocation_i(allocation_o),
    .resolve_i(resolve_accept_o), .mispredict_i, .resolve_id_i,
    .create_ready_o(checkpoint_ready), .create_accept_o(checkpoint_accept_o), .create_id_o(checkpoint_id_o),
    .restore_valid_o(branch_recover_o), .restore_rat_o(restore_rat), .reclaim_o,
    .released_o(checkpoint_released_o), .valid_o(checkpoint_valid_o)
  );

`ifndef SYNTHESIS
  always_ff @(posedge clk_i) begin
    if (!rst_i && !recover_i) begin
      assert (checkpoint_accept_o == checkpoint_create) else $fatal(1, "RENAME_RECOVERY_ATOMIC_CREATE");
      assert (!(branch_recover_o && (|accept_o || |commit_accept_o)))
        else $fatal(1, "RENAME_RECOVERY_BOUNDARY");
    end
  end
`endif
endmodule
`default_nettype wire
