`default_nettype none
module rename_ownership (
  input wire clk, rst, recover,
  input wire request_allocate, request_writeback, request_commit,
  input wire [4:0] rd, rs1, rs2, watch_arch,
  input wire [5:0] watch_phys
);
  import single_lane_pkg::*;
  logic past_valid = 0;
  logic pending, completed;
  rename_entry_t live_entry;
  logic [5:0] committed [0:31];
  logic [63:0] committed_owners;
  wire allocate = !rst && !recover && !pending && request_allocate;
  wire writeback = !rst && !recover && pending && !completed && live_entry.rd != 0 && request_writeback;
  wire commit = !rst && !recover && pending && completed && request_commit;
  wire can_allocate, sources_ready;
  wire [5:0] source1, source2;
  rename_entry_t entry;
  rename_single dut (
    .clk_i(clk), .rst_i(rst), .recover_i(recover), .rs1_i(rs1), .rs2_i(rs2), .rd_i(rd),
    .source1_o(source1), .source2_o(source2), .sources_ready_o(sources_ready),
    .allocate_i(allocate), .can_allocate_o(can_allocate), .entry_o(entry),
    .wb_accept_i(writeback), .wb_destination_i(live_entry.destination),
    .commit_i(commit), .commit_entry_i(live_entry)
  );

  function automatic logic [5:0] expected_map(input logic [4:0] arch);
    return pending && live_entry.rd != 0 && live_entry.rd == arch
      ? live_entry.destination : committed[arch];
  endfunction
  function automatic logic expected_ready(input logic [4:0] arch);
    return !(pending && !completed && live_entry.rd != 0 && live_entry.rd == arch);
  endfunction
  wire watched_committed = committed_owners[watch_phys];
  wire destination_owned = committed_owners[entry.destination];
  wire watched_live = pending && live_entry.rd != 0 && live_entry.destination == watch_phys;

  // Ownership consists of committed mappings plus the one unretired destination.
  always @(posedge clk) begin
    past_valid <= 1;
    if (!past_valid) assume (rst);
    if (rst) begin
      pending <= 0;
      completed <= 0;
      committed_owners <= 64'h00000000ffffffff;
      for (int i = 0; i < 32; i++) committed[i] <= 6'(i);
    end else if (recover) begin
      pending <= 0;
      completed <= 0;
    end else begin
      if (allocate) begin
        live_entry <= entry;
        pending <= 1;
        completed <= rd == 0;
      end
      if (writeback) completed <= 1;
      if (commit) begin
        if (live_entry.rd != 0) begin
          committed[live_entry.rd] <= live_entry.destination;
          committed_owners <= (committed_owners & ~(64'b1 << live_entry.stale))
                              | (64'b1 << live_entry.destination);
        end
        pending <= 0;
        completed <= 0;
      end
    end

    if (past_valid && !rst) begin
      zero_reserved: assert (!dut.free_q[0] && dut.rat_q[0] == 0 && dut.committed_q[0] == 0);
      ownership: assert (dut.free_q[watch_phys] == !(watched_committed || watched_live));
      committed_map: assert (dut.committed_q[watch_arch] == committed[watch_arch]);
      speculative_map: assert (dut.rat_q[watch_arch] == expected_map(watch_arch));
      owned_readiness: assert (!(watched_committed || watched_live)
                              || dut.ready_q[watch_phys] == (watched_committed || completed));
      source_maps: assert (source1 == expected_map(rs1) && source2 == expected_map(rs2));
      source_readiness: assert (sources_ready == (expected_ready(rs1) && expected_ready(rs2)));
      available: assert (can_allocate == !recover);
      if (allocate) begin
        stale_mapping: assert (entry.stale == committed[rd] && entry.rd == rd);
        fresh_destination: assert (rd == 0 ? entry.destination == 0 : !destination_owned);
      end
    end
  end

  logic saw_commit, saw_recover;
  logic [4:0] retired_rd;
  logic [5:0] released_tag;
  always @(posedge clk) begin
    if (rst) begin
      saw_commit <= 0;
      saw_recover <= 0;
    end else begin
      if (commit && live_entry.rd != 0) begin
        saw_commit <= 1;
        retired_rd <= live_entry.rd;
        released_tag <= live_entry.stale;
      end
      if (recover && pending && live_entry.rd != 0) saw_recover <= 1;
    end
    reset_pending: cover (past_valid && rst && pending);
    if (past_valid && !rst) begin
      nonzero_commit: cover (commit && live_entry.rd != 0);
      zero_commit: cover (commit && live_entry.rd == 0);
      stalled_pending: cover (pending && !completed && !writeback && !recover);
      recover_pending: cover (recover && pending && live_entry.rd != 0 && !completed);
      recover_completed: cover (recover && pending && live_entry.rd != 0 && completed);
      repeated_destination: cover (saw_commit && allocate && rd != 0 && rd == retired_rd);
      recycle_stale: cover (saw_commit && allocate && rd != 0 && entry.destination == released_tag);
      allocate_after_recovery: cover (saw_recover && allocate && rd != 0);
    end
  end
endmodule
`default_nettype wire
