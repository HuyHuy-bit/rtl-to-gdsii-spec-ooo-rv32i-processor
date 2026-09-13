`default_nettype none
module rename_recovery_ownership (
  input wire clk, rst, recover, resources,
  input wire [1:0] valid, checkpoint, rd0, rd1, rs10, rs11, rs20, rs21,
  input wire [1:0] wb_offer, wb_live, wb_index0, wb_index1,
  input wire [11:0] stale_wb_tags,
  input wire resolve, resolve_live, mispredict,
  input wire [2:0] resolve_id,
  input wire [1:0] retire_request
);
  typedef struct packed {
    logic [4:0] rd;
    logic [5:0] tag, stale;
    logic done, branch;
    logic [2:0] slot;
  } entry_t;
  entry_t entries [0:3], next_entries [0:3], added [0:1];
  logic [2:0] count, next_count;
  logic [191:0] committed, next_committed, map, walk;
  logic [63:0] owned, expected_ready, available, ready_view, expected_reclaim;
  logic [7:0] slots, expected_released;
  logic [1:0] expected_accept, expected_wb, commit_offer, expected_commit;
  logic [11:0] expected_destination, expected_stale, expected_source1, expected_source2;
  logic [3:0] expected_source_ready;
  logic [63:0] expected_allocation;
  logic expected_resolve, expected_recover, expected_create, room;
  logic [2:0] expected_slot;
  integer boundary, selected_count, retire_count, add_count, keep_count;
  logic [11:0] wb_tags, commit_tags, commit_stales;
  logic [9:0] commit_rds;
  wire [9:0] rds = {3'b0, rd1, 3'b0, rd0};
  wire [9:0] rs1s = {3'b0, rs11, 3'b0, rs10};
  wire [9:0] rs2s = {3'b0, rs21, 3'b0, rs20};
  wire [1:0] accept, wb_accept, commit_accept;
  wire [11:0] destination, stale, source1, source2;
  wire [3:0] source_ready;
  wire [63:0] allocation, free_tags, ready_tags, reclaim;
  wire [191:0] rat, committed_map;
  wire create, resolve_accept, branch_recover;
  wire [2:0] create_id;
  wire [7:0] slot_valid, released;
  logic past_valid;
  initial past_valid = 0;

  rename_recovery dut (
    .clk_i(clk), .rst_i(rst), .recover_i(recover), .resources_ready_i(resources && room),
    .valid_i(valid), .checkpoint_i(checkpoint), .rd_i(rds), .rs1_i(rs1s), .rs2_i(rs2s),
    .wb_offer_i(wb_offer), .wb_live_i(wb_live), .wb_destination_i(wb_tags),
    .commit_i(commit_offer), .commit_destination_i(commit_tags), .commit_stale_i(commit_stales), .commit_rd_i(commit_rds),
    .resolve_i(resolve), .resolve_live_i(resolve_live), .mispredict_i(mispredict), .resolve_id_i(resolve_id),
    .accept_o(accept), .wb_accept_o(wb_accept), .commit_accept_o(commit_accept),
    .destination_o(destination), .stale_o(stale), .source1_o(source1), .source2_o(source2), .source_ready_o(source_ready),
    .allocation_o(allocation), .free_o(free_tags), .ready_o(ready_tags), .rat_o(rat), .committed_o(committed_map),
    .checkpoint_accept_o(create), .checkpoint_id_o(create_id), .checkpoint_valid_o(slot_valid),
    .checkpoint_released_o(released), .resolve_accept_o(resolve_accept), .branch_recover_o(branch_recover), .reclaim_o(reclaim)
  );

  // Reconstruct ownership and branch age from instructions, without DUT snapshots or histories.
  always_comb begin
    map = committed;
    owned = 0;
    expected_ready = 0;
    slots = 0;
    boundary = -1;
    for (int arch = 0; arch < 32; arch++) begin
      owned[committed[arch*6 +: 6]] = 1;
      expected_ready[committed[arch*6 +: 6]] = 1;
    end
    for (int n = 0; n < 4; n++) begin
      if (n < count) begin
        if (entries[n].rd != 0) begin
          map[entries[n].rd*6 +: 6] = entries[n].tag;
          owned[entries[n].tag] = 1;
          expected_ready[entries[n].tag] = entries[n].done;
        end
        if (entries[n].branch) begin
          slots[entries[n].slot] = 1;
          if (entries[n].slot == resolve_id) boundary = n;
        end
      end
    end
    expected_resolve = !rst && !recover && resolve && resolve_live && boundary >= 0;
    expected_recover = expected_resolve && mispredict;
    expected_reclaim = 0;
    expected_released = 0;
    if (expected_resolve) begin
      expected_released[resolve_id] = 1;
      for (int n = 0; n < 4; n++) begin
        if (n < count && expected_recover) begin
          if (n > boundary && entries[n].rd != 0) expected_reclaim[entries[n].tag] = 1;
          if (n >= boundary && entries[n].branch) expected_released[entries[n].slot] = 1;
        end
      end
    end

    wb_tags = stale_wb_tags;
    if (wb_live[0]) wb_tags[5:0] = 3'(wb_index0) < count ? entries[wb_index0].tag : 6'd0;
    if (wb_live[1]) wb_tags[11:6] = 3'(wb_index1) < count ? entries[wb_index1].tag : 6'd0;
    ready_view = expected_ready;
    expected_wb = 0;
    for (int lane = 0; lane < 2; lane++) begin
      if (!rst && !recover && wb_offer[lane] && wb_live[lane] && wb_tags[lane*6 +: 6] != 0
          && owned[wb_tags[lane*6 +: 6]] && !ready_view[wb_tags[lane*6 +: 6]]
          && !expected_reclaim[wb_tags[lane*6 +: 6]]) begin
        expected_wb[lane] = 1;
        ready_view[wb_tags[lane*6 +: 6]] = 1;
      end
    end

    commit_offer = 0;
    commit_tags = 0;
    commit_stales = 0;
    commit_rds = 0;
    for (int lane = 0; lane < 2; lane++) begin
      if (lane < count && retire_request[lane] && (lane == 0 || commit_offer[0])
          && !entries[lane].branch && (entries[lane].rd == 0 || ready_view[entries[lane].tag])) begin
        commit_offer[lane] = 1;
        commit_tags[lane*6 +: 6] = entries[lane].tag;
        commit_stales[lane*6 +: 6] = entries[lane].stale;
        commit_rds[lane*5 +: 5] = entries[lane].rd;
      end
    end
    expected_commit = rst || recover || expected_recover ? 2'b00 : commit_offer;
    retire_count = int'(expected_commit[0]) + int'(expected_commit[1]);
    selected_count = !valid[0] ? 0 : valid[1] && !checkpoint[0] ? 2 : 1;
    room = int'(count) + selected_count <= 4;
    expected_accept = 0;
    if (!rst && !recover && !expected_recover && resources && room && valid != 2'b10)
      expected_accept = selected_count == 2 ? 2'b11 : selected_count == 1 ? 2'b01 : 2'b00;
    expected_create = |(expected_accept & checkpoint);
    expected_slot = 0;
    if (expected_create)
      for (int slot = 7; slot >= 0; slot--)
        if (!slots[slot]) expected_slot = 3'(slot);
    expected_destination = 0;
    expected_stale = 0;
    expected_source1 = 0;
    expected_source2 = 0;
    expected_source_ready = 0;
    expected_allocation = 0;
    available = ~owned & 64'h0000000f0000000e;
    walk = map;
    for (int lane = 0; lane < 2; lane++) begin
      added[lane] = '0;
      if (expected_accept[lane]) begin
        added[lane].rd = rds[lane*5 +: 5];
        added[lane].done = rds[lane*5 +: 5] == 0;
        added[lane].branch = checkpoint[lane];
        added[lane].slot = expected_slot;
        expected_source1[lane*6 +: 6] = walk[rs1s[lane*5 +: 5]*6 +: 6];
        expected_source2[lane*6 +: 6] = walk[rs2s[lane*5 +: 5]*6 +: 6];
        expected_source_ready[lane*2] = ready_view[expected_source1[lane*6 +: 6]];
        expected_source_ready[lane*2+1] = ready_view[expected_source2[lane*6 +: 6]];
        if (added[lane].rd != 0) begin
          for (int tag = 63; tag >= 1; tag--)
            if (available[tag]) added[lane].tag = 6'(tag);
          added[lane].stale = walk[added[lane].rd*6 +: 6];
          available[added[lane].tag] = 0;
          ready_view[added[lane].tag] = 0;
          walk[added[lane].rd*6 +: 6] = added[lane].tag;
          expected_allocation[added[lane].tag] = 1;
        end
        expected_destination[lane*6 +: 6] = added[lane].tag;
        expected_stale[lane*6 +: 6] = added[lane].stale;
      end
    end

    next_committed = committed;
    next_count = count;
    add_count = int'(expected_accept[0]) + int'(expected_accept[1]);
    keep_count = expected_recover ? boundary+1 : int'(count)-retire_count;
    for (int n = 0; n < 4; n++) begin
      next_entries[n] = '0;
      if (n < keep_count) begin
        next_entries[n] = entries[n+retire_count];
        for (int lane = 0; lane < 2; lane++)
          if (expected_wb[lane] && next_entries[n].tag == wb_tags[lane*6 +: 6]) next_entries[n].done = 1;
        if (expected_resolve && next_entries[n].slot == resolve_id) next_entries[n].branch = 0;
      end
      for (int lane = 0; lane < 2; lane++)
        if (expected_accept[lane] && n == keep_count+lane) next_entries[n] = added[lane];
    end
    next_count = 3'(keep_count + add_count);
    for (int lane = 0; lane < 2; lane++)
      if (expected_commit[lane] && entries[lane].rd != 0)
        next_committed[entries[lane].rd*6 +: 6] = entries[lane].tag;
    if (rst || recover) begin
      next_count = 0;
      for (int n = 0; n < 4; n++) next_entries[n] = '0;
      if (rst)
        for (int arch = 0; arch < 32; arch++) next_committed[arch*6 +: 6] = 6'(arch);
    end
  end

  always @(posedge clk) begin
    past_valid <= 1;
    if (!past_valid) assume (rst);
    count <= next_count;
    committed <= next_committed;
    for (int n = 0; n < 4; n++) entries[n] <= next_entries[n];
    if (past_valid) begin
      ledger_capacity: assert (count <= 4);
      ownership: assert (free_tags == ~owned);
      fixed_free: assert ((free_tags & ~64'h0000000f0000000e) == 64'hfffffff000000000);
      fixed_ready: assert ((ready_tags & ~64'h0000000f0000000e) == 64'h00000000fffffff1);
      readiness: assert (ready_tags == expected_ready);
      speculative_map: assert (rat == map);
      committed_state: assert (committed_map == committed);
      checkpoint_owners: assert (slot_valid == slots);
      rename_acceptance: assert (accept == expected_accept);
      rename_destinations: assert (destination == expected_destination && stale == expected_stale && allocation == expected_allocation);
      source_maps: assert (source1 == expected_source1 && source2 == expected_source2 && source_ready == expected_source_ready);
      writeback_acceptance: assert (wb_accept == expected_wb);
      retirement_acceptance: assert (commit_accept == expected_commit);
      resolution_acceptance: assert (resolve_accept == expected_resolve && branch_recover == expected_recover);
      recovery_packet: assert (reclaim == expected_reclaim && released == expected_released);
      checkpoint_creation: assert (create == expected_create && create_id == expected_slot);
    end
  end

  logic [191:0] checkpoint_maps [0:7];
  logic [63:0] checkpoint_tags [0:7];
  logic [7:0] checkpoint_older [0:7];
  logic [191:0] checkpoint_walk;
  always_comb begin
    checkpoint_walk = committed;
    for (int slot = 0; slot < 8; slot++) begin
      checkpoint_maps[slot] = 0;
      checkpoint_tags[slot] = 0;
      checkpoint_older[slot] = 0;
    end
    for (int n = 0; n < 4; n++) begin
      if (n < count) begin
        if (entries[n].rd != 0) checkpoint_walk[entries[n].rd*6 +: 6] = entries[n].tag;
        if (entries[n].branch) begin
          checkpoint_maps[entries[n].slot] = checkpoint_walk;
          for (int other = 0; other < 4; other++) begin
            if (other < count && other > n && entries[other].rd != 0)
              checkpoint_tags[entries[n].slot][entries[other].tag] = 1;
            if (other < n && entries[other].branch)
              checkpoint_older[entries[n].slot][entries[other].slot] = 1;
          end
        end
      end
    end
  end

  logic [191:0] ledger_maps [0:4];
  logic [63:0] ledger_owners [0:4];
  always_comb begin
    ledger_maps[0] = committed;
    ledger_owners[0] = 0;
    for (int arch = 0; arch < 32; arch++) ledger_owners[0][committed[arch*6 +: 6]] = 1;
    for (int n = 0; n < 4; n++) begin
      ledger_maps[n+1] = ledger_maps[n];
      ledger_owners[n+1] = ledger_owners[n];
      if (n < count && entries[n].rd != 0) begin
        ledger_maps[n+1][entries[n].rd*6 +: 6] = entries[n].tag;
        ledger_owners[n+1][entries[n].tag] = 1;
      end
    end
  end

  // These checked state relations strengthen induction without assuming DUT correctness.
  always @(posedge clk)
    if (past_valid) reference_zero: assert (committed[5:0] == 0);
  for (genvar arch = 1; arch < 32; arch++) begin : arch_invariants
    always @(posedge clk) if (past_valid) begin
      reference_nonzero: assert (committed[arch*6 +: 6] != 0);
      if (arch < 4) reference_pool: assert (committed[arch*6+2 +: 3] == 0);
      if (arch >= 4) untouched_registers: assert (committed[arch*6 +: 6] == 6'(arch));
    end
    for (genvar other = 1; other < 32; other++) begin : pairs
      always @(posedge clk) if (past_valid && arch < 4 && other != arch)
        reference_unique: assert (committed[arch*6 +: 6] != committed[other*6 +: 6]);
    end
  end
  for (genvar n = 0; n < 4; n++) begin : entry_invariants
    always @(posedge clk) if (past_valid) begin
      if (n < count) begin
        reference_rd: assert (entries[n].rd < 4);
        reference_tag_bound: assert (entries[n].tag[4:2] == 0 && entries[n].stale[4:2] == 0);
        reference_slot_bound: assert (!entries[n].branch || entries[n].slot < 4);
        if (entries[n].rd == 0) begin
          reference_zero_entry: assert (entries[n].tag == 0 && entries[n].stale == 0 && entries[n].done);
        end else begin
          reference_owner: assert (!ledger_owners[n][entries[n].tag] && entries[n].stale == ledger_maps[n][entries[n].rd*6 +: 6]);
        end
      end else reference_unused: assert (entries[n] == '0);
    end
    for (genvar other = 0; other < n; other++) begin : pairs
      always @(posedge clk) if (past_valid && n < count)
        reference_slots: assert (!entries[n].branch || !entries[other].branch || entries[n].slot != entries[other].slot);
    end
  end
  for (genvar slot = 0; slot < 8; slot++) begin : checkpoint_invariants
    always @(posedge clk) if (past_valid) begin
      checkpoint_snapshots: assert (dut.checkpoints.snapshot_q[slot] == checkpoint_maps[slot]);
      checkpoint_histories: assert (dut.checkpoints.history_q[slot] == checkpoint_tags[slot]);
      checkpoint_ages: assert (dut.checkpoints.older_q[slot] == checkpoint_older[slot]);
    end
  end

  logic saw_recover, saw_retire;
  logic [63:0] retired_stales, killed_tags;
  logic [7:0] released_slots;
  always @(posedge clk) begin
    if (rst) begin
      saw_recover <= 0;
      saw_retire <= 0;
      retired_stales <= 0;
      killed_tags <= 0;
      released_slots <= 0;
    end else begin
      if (expected_recover) saw_recover <= 1;
      if (|expected_commit) saw_retire <= 1;
      killed_tags <= killed_tags | expected_reclaim;
      released_slots <= released_slots | expected_released;
      for (int lane = 0; lane < 2; lane++)
        if (expected_commit[lane] && entries[lane].rd != 0) retired_stales[entries[lane].stale] <= 1;
    end
    reset_pending: cover (past_valid && rst && count > 1);
    if (past_valid && !rst) begin
      four_inflight: cover (count == 4);
      dual_waw: cover (accept == 3 && rd0 != 0 && rd0 == rd1);
      lane_one_cfi: cover (create && accept == 3);
      dual_retirement: cover (commit_accept == 3);
      dual_waw_retirement: cover (commit_accept == 3 && entries[0].rd != 0 && entries[0].rd == entries[1].rd);
      dual_writeback: cover (wb_accept == 3);
      out_of_order_completion: cover (count >= 2 && !entries[0].done && wb_accept[0]
                                      && wb_tags[5:0] == entries[1].tag && !wb_accept[1]);
      retire_and_rename: cover (|commit_accept && |accept);
      nested_recovery: cover (branch_recover && boundary > 0 && entries[0].branch && |reclaim);
      younger_checkpoint_kill: cover (branch_recover && (released & (released-1)) != 0);
      surviving_writeback: cover (branch_recover && |wb_accept);
      branch_link_writeback: cover (branch_recover && wb_accept[0] && entries[boundary].rd != 0
                                   && wb_tags[5:0] == entries[boundary].tag);
      killed_writeback: cover (branch_recover && wb_offer[0] && wb_live[0] && reclaim[wb_tags[5:0]]);
      recovery_retire_collision: cover (branch_recover && |commit_offer);
      correct_resolve_create: cover (resolve_accept && !mispredict && create);
      retired_tag_reuse: cover (saw_retire && |(allocation & retired_stales));
      killed_tag_reuse: cover (saw_recover && |(allocation & killed_tags));
      checkpoint_slot_reuse: cover (create && released_slots[create_id]);
      stale_writeback_reuse: cover (wb_offer[0] && !wb_live[0] && killed_tags[wb_tags[5:0]]
                                   && owned[wb_tags[5:0]] && !expected_ready[wb_tags[5:0]]);
      stale_resolution_reuse: cover (resolve && !resolve_live && slots[resolve_id] && released_slots[resolve_id]);
      full_recovery: cover (recover && count > 1);
    end
  end
endmodule
`default_nettype wire
