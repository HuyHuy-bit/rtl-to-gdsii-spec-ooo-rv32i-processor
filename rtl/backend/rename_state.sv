`default_nettype none
module rename_state (
  input wire clk_i, rst_i, recover_i, resources_ready_i,
  input wire branch_recover_i,
  input wire [191:0] branch_rat_i,
  input wire [63:0] branch_reclaim_i,
  input wire [1:0] valid_i, checkpoint_i,
  input wire [9:0] rs1_i, rs2_i, rd_i,
  input wire [1:0] wb_accept_i, commit_i,
  input wire [11:0] wb_destination_i, commit_destination_i, commit_stale_i,
  input wire [9:0] commit_rd_i,
  output wire [1:0] accept_o,
  output wire [11:0] source1_o, source2_o, destination_o, stale_o,
  output wire [3:0] source_ready_o,
  output wire [63:0] allocation_o,
  output wire [191:0] rat_o, committed_o,
  output wire [63:0] free_o, ready_o
);
  logic [191:0] rat_q, rat_d, committed_q, committed_d;
  logic [63:0] free_q, free_d, ready_q, ready_d, ready_view, recovered_free;

  assign rat_o = rat_q;
  assign committed_o = committed_q;
  assign free_o = free_q;
  assign ready_o = ready_q;

  always_comb begin
    ready_view = ready_q;
    if (!rst_i && !recover_i)
      for (int lane = 0; lane < 2; lane++)
        if (wb_accept_i[lane] && wb_destination_i[lane*6 +: 6] != 0)
          ready_view[wb_destination_i[lane*6 +: 6]] = 1;
  end

  rename_bundle planner (
    .rst_i, .recover_i(recover_i || branch_recover_i), .resources_ready_i, .valid_i, .checkpoint_i,
    .rs1_i, .rs2_i, .rd_i, .rat_i(rat_q), .free_i(free_q), .ready_i(ready_view),
    .accept_o, .source1_o, .source2_o, .destination_o, .stale_o,
    .source_ready_o, .allocation_o
  );

  always_comb begin
    rat_d = rat_q;
    committed_d = committed_q;
    free_d = free_q;
    ready_d = ready_view;
    recovered_free = 64'hfffffffffffffffe;
    for (int arch = 1; arch < 32; arch++)
      recovered_free[committed_q[arch*6 +: 6]] = 0;

    // Retirement is ordered; lane one's stale mapping may be lane zero's destination.
    for (int lane = 0; lane < 2; lane++) begin
      if (commit_i[lane] && commit_rd_i[lane*5 +: 5] != 0) begin
        committed_d[commit_rd_i[lane*5 +: 5]*6 +: 6] = commit_destination_i[lane*6 +: 6];
        free_d[commit_stale_i[lane*6 +: 6]] = 1;
        ready_d[commit_stale_i[lane*6 +: 6]] = 0;
      end
    end
    for (int lane = 0; lane < 2; lane++) begin
      if (accept_o[lane] && rd_i[lane*5 +: 5] != 0) begin
        rat_d[rd_i[lane*5 +: 5]*6 +: 6] = destination_o[lane*6 +: 6];
        free_d[destination_o[lane*6 +: 6]] = 0;
        ready_d[destination_o[lane*6 +: 6]] = 0;
      end
    end
    if (branch_recover_i) begin
      rat_d = branch_rat_i;
      committed_d = committed_q;
      free_d = free_q | branch_reclaim_i;
      ready_d = ready_view & ~branch_reclaim_i;
    end
    if (recover_i) begin
      rat_d = committed_q;
      committed_d = committed_q;
      free_d = recovered_free;
      ready_d = ~recovered_free;
    end
    if (rst_i) begin
      for (int arch = 0; arch < 32; arch++) begin
        rat_d[arch*6 +: 6] = 6'(arch);
        committed_d[arch*6 +: 6] = 6'(arch);
      end
      free_d = 64'hffffffff00000000;
      ready_d = 64'h00000000ffffffff;
    end
  end

  always_ff @(posedge clk_i) begin
    rat_q <= rat_d;
    committed_q <= committed_d;
    free_q <= free_d;
    ready_q <= ready_d;
  end

`ifndef SYNTHESIS
  // The ROB and completion arbiter must supply live, ordered, already accepted events.
  always_ff @(posedge clk_i) begin : ownership_checks
    logic [191:0] commit_walk;
    logic [63:0] released, destinations;
    commit_walk = committed_q;
    released = 0;
    destinations = 0;
    if (!rst_i && !recover_i) begin
      if (branch_recover_i) begin
        assert (commit_i == 0 && !branch_reclaim_i[0] && (branch_reclaim_i & free_q) == 0)
          else $fatal(1, "RENAME_STATE_BRANCH_RECOVERY");
        for (int arch = 0; arch < 32; arch++)
          assert (!branch_reclaim_i[branch_rat_i[arch*6 +: 6]])
            else $fatal(1, "RENAME_STATE_RECLAIM_MAPPING");
      end
      assert (commit_i != 2'b10) else $fatal(1, "RENAME_STATE_COMMIT_PREFIX");
      assert (!free_q[0] && ready_q[0] && rat_q[5:0] == 0 && committed_q[5:0] == 0)
        else $fatal(1, "RENAME_STATE_ZERO");
      for (int lane = 0; lane < 2; lane++) begin
        if (wb_accept_i[lane] && wb_destination_i[lane*6 +: 6] != 0) begin
          assert (!free_q[wb_destination_i[lane*6 +: 6]] && !ready_q[wb_destination_i[lane*6 +: 6]]
                  && (!branch_recover_i || !branch_reclaim_i[wb_destination_i[lane*6 +: 6]]))
            else $fatal(1, "RENAME_STATE_WB_OWNER");
          if (lane == 1 && wb_accept_i[0])
            assert (wb_destination_i[11:6] != wb_destination_i[5:0])
              else $fatal(1, "RENAME_STATE_DUPLICATE_WB");
        end
        if (commit_i[lane]) begin
          if (commit_rd_i[lane*5 +: 5] == 0) begin
            assert (commit_destination_i[lane*6 +: 6] == 0 && commit_stale_i[lane*6 +: 6] == 0)
              else $fatal(1, "RENAME_STATE_ZERO_COMMIT");
          end else begin
            assert (commit_destination_i[lane*6 +: 6] != 0 && commit_stale_i[lane*6 +: 6] != 0
                    && commit_destination_i[lane*6 +: 6] != commit_stale_i[lane*6 +: 6]
                    && commit_walk[commit_rd_i[lane*5 +: 5]*6 +: 6] == commit_stale_i[lane*6 +: 6]
                    && !free_q[commit_destination_i[lane*6 +: 6]]
                    && ready_view[commit_destination_i[lane*6 +: 6]]
                    && !released[commit_stale_i[lane*6 +: 6]]
                    && !destinations[commit_destination_i[lane*6 +: 6]])
              else $fatal(1, "RENAME_STATE_COMMIT_OWNER");
            commit_walk[commit_rd_i[lane*5 +: 5]*6 +: 6] = commit_destination_i[lane*6 +: 6];
            released[commit_stale_i[lane*6 +: 6]] = 1;
            destinations[commit_destination_i[lane*6 +: 6]] = 1;
          end
        end
      end
      for (int arch = 1; arch < 32; arch++) begin
        assert (rat_q[arch*6 +: 6] != 0 && committed_q[arch*6 +: 6] != 0
                && !free_q[rat_q[arch*6 +: 6]] && !free_q[committed_q[arch*6 +: 6]]
                && ready_q[committed_q[arch*6 +: 6]])
          else $fatal(1, "RENAME_STATE_MAP_OWNER");
        for (int other = 1; other < arch; other++)
          assert (rat_q[arch*6 +: 6] != rat_q[other*6 +: 6]
                  && committed_q[arch*6 +: 6] != committed_q[other*6 +: 6])
            else $fatal(1, "RENAME_STATE_MAP_ALIAS");
      end
    end
  end
`endif
endmodule
`default_nettype wire
