`default_nettype none
module rob_two_wide (
  input wire clk_i, rst_i, flush_i, drained_i,
  input wire [1:0] allocate_i, allocate_cfi_i, allocate_solo_i,
  input wire [9:0] allocate_rd_i,
  input wire [63:0] allocate_pc_i,
  input wire [11:0] allocate_destination_i, allocate_stale_i,
  output logic [1:0] allocate_ready_o, allocate_accept_o,
  output logic [25:0] allocate_id_o,
  input wire [1:0] complete_offer_i, complete_take_i, complete_solo_i,
  input wire [25:0] complete_id_i,
  input commit_event_pkg::commit_event_t [1:0] complete_event_i,
  output logic [1:0] complete_ready_o, complete_accept_o,
  output logic [11:0] complete_destination_o,
  input wire resolve_offer_i, resolve_take_i, mispredict_i,
  input wire [12:0] resolve_id_i,
  output logic resolve_ready_o, resolve_accept_o, branch_recover_o,
  input wire [1:0] retire_take_i,
  output logic [1:0] retire_valid_o, retire_accept_o,
  output logic [9:0] retire_rd_o,
  output logic [11:0] retire_destination_o, retire_stale_o,
  output commit_event_pkg::commit_event_t [1:0] retire_event_o,
  input wire trap_take_i,
  output logic trap_valid_o, trap_accept_o,
  output commit_event_pkg::commit_event_t trap_event_o,
  output logic head_valid_o,
  output logic [12:0] head_id_o,
  output logic [31:0] head_pc_o,
  output logic [5:0] occupancy_o,
  output logic identity_drain_o
);
  import commit_event_pkg::*;
`ifndef SYNTHESIS
  initial begin
    assert (COMMIT_SLOTS == 2 && COMMIT_EVENT_SHA256 != "") else $fatal(1, "ROB2_EVENT_SCHEMA");
  end
`endif
  logic [31:0] valid_q, done_q, cfi_q, resolved_q, solo_q, exhausted_q;
  logic [7:0] generation_q [32], next_generation_q [32];
  logic [4:0] rd_q [32];
  logic [31:0] pc_q [32];
  logic [5:0] destination_q [32], stale_q [32];
  commit_event_t event_q [32];
  logic [4:0] head_q, tail_q, second_head, second_tail, resolve_slot;
  logic [5:0] count_q;
  logic [63:0] order_q;
  logic [4:0] resolve_age;
  logic clear_window;

  assign second_head = head_q + 5'd1;
  assign second_tail = tail_q + 5'd1;
  assign resolve_slot = resolve_id_i[4:0];
  assign resolve_age = resolve_slot - head_q;
  assign occupancy_o = count_q;
  assign identity_drain_o = |exhausted_q;
  assign head_valid_o = count_q != 0 && !rst_i && !flush_i;
  assign head_pc_o = pc_q[head_q];
  assign head_id_o = {generation_q[head_q], head_q};
  assign clear_window = flush_i || trap_accept_o;

  always_comb begin
    trap_valid_o = head_valid_o && done_q[head_q] && event_q[head_q].trap;
    trap_accept_o = trap_valid_o && trap_take_i;
    trap_event_o = event_q[head_q];
    trap_event_o.valid = trap_valid_o;
    trap_event_o.order = order_q;
    trap_event_o.retired = 0;
    resolve_ready_o = !rst_i && !clear_window && valid_q[resolve_slot]
      && generation_q[resolve_slot] == resolve_id_i[12:5]
      && cfi_q[resolve_slot] && !resolved_q[resolve_slot];
    resolve_accept_o = resolve_offer_i && resolve_take_i && resolve_ready_o;
    branch_recover_o = resolve_accept_o && mispredict_i;

    allocate_id_o = {{next_generation_q[second_tail], second_tail}, {next_generation_q[tail_q], tail_q}};
    allocate_ready_o = 0;
    if (!rst_i && !clear_window && !branch_recover_o && !drained_i) begin
      allocate_ready_o[0] = count_q < 32 && !exhausted_q[tail_q];
      allocate_ready_o[1] = allocate_ready_o[0] && count_q < 31 && !exhausted_q[second_tail];
    end
    allocate_accept_o = allocate_i & allocate_ready_o;

    complete_ready_o = 0;
    complete_destination_o = 0;
    for (int lane = 0; lane < 2; lane++) begin
      complete_destination_o[lane*6 +: 6] = destination_q[complete_id_i[lane*13 +: 5]];
      complete_ready_o[lane] = !rst_i && !clear_window && valid_q[complete_id_i[lane*13 +: 5]]
        && generation_q[complete_id_i[lane*13 +: 5]] == complete_id_i[lane*13+5 +: 8]
        && !done_q[complete_id_i[lane*13 +: 5]]
        && (!branch_recover_o || 5'(complete_id_i[lane*13 +: 5] - head_q) <= resolve_age);
    end
    if (complete_offer_i[0] && complete_id_i[12:0] == complete_id_i[25:13])
      complete_ready_o[1] = 0;
    complete_accept_o = complete_offer_i & complete_take_i & complete_ready_o;

    retire_valid_o = 0;
    if (!rst_i && !clear_window && !branch_recover_o) begin
      retire_valid_o[0] = count_q != 0 && done_q[head_q] && !event_q[head_q].trap
        && (!cfi_q[head_q] || resolved_q[head_q]);
      retire_valid_o[1] = retire_valid_o[0] && count_q > 1 && done_q[second_head]
        && !event_q[second_head].trap && (!cfi_q[second_head] || resolved_q[second_head])
        && !solo_q[head_q] && !solo_q[second_head];
    end
    retire_accept_o = retire_take_i & retire_valid_o;
    retire_rd_o = {rd_q[second_head], rd_q[head_q]};
    retire_destination_o = {destination_q[second_head], destination_q[head_q]};
    retire_stale_o = {stale_q[second_head], stale_q[head_q]};
    retire_event_o[0] = event_q[head_q];
    retire_event_o[1] = event_q[second_head];
    for (int lane = 0; lane < 2; lane++) begin
      retire_event_o[lane].valid = retire_valid_o[lane];
      retire_event_o[lane].order = order_q + 64'(lane);
      retire_event_o[lane].retired = 1;
    end
  end

  always_ff @(posedge clk_i) begin
    if (rst_i) begin
      valid_q <= 0;
      done_q <= 0;
      cfi_q <= 0;
      resolved_q <= 0;
      solo_q <= 0;
      exhausted_q <= 0;
      head_q <= 0;
      tail_q <= 0;
      count_q <= 0;
      order_q <= 0;
      for (int slot = 0; slot < 32; slot++) begin
        generation_q[slot] <= 0;
        next_generation_q[slot] <= 0;
      end
    end else begin
      order_q <= order_q + 64'($countones(retire_accept_o)) + 64'(trap_accept_o);
      if (clear_window) begin
        valid_q <= 0;
        done_q <= 0;
        head_q <= 0;
        tail_q <= 0;
        count_q <= 0;
      end else begin
        if (resolve_accept_o) resolved_q[resolve_slot] <= 1;
        if (branch_recover_o) begin
          for (int slot = 0; slot < 32; slot++)
            if (5'(5'(slot) - head_q) > resolve_age) begin
              valid_q[slot] <= 0;
              done_q[slot] <= 0;
            end
          tail_q <= resolve_slot + 5'd1;
          count_q <= {1'b0, resolve_age} + 6'd1;
        end else begin
          head_q <= head_q + 5'($countones(retire_accept_o));
          tail_q <= tail_q + 5'($countones(allocate_accept_o));
          count_q <= count_q + 6'($countones(allocate_accept_o)) - 6'($countones(retire_accept_o));
          for (int lane = 0; lane < 2; lane++) begin
            if (retire_accept_o[lane]) begin
              valid_q[5'(head_q + 5'(lane))] <= 0;
              done_q[5'(head_q + 5'(lane))] <= 0;
            end
            if (allocate_accept_o[lane]) begin
              valid_q[5'(tail_q + 5'(lane))] <= 1;
              done_q[5'(tail_q + 5'(lane))] <= 0;
              cfi_q[5'(tail_q + 5'(lane))] <= allocate_cfi_i[lane];
              resolved_q[5'(tail_q + 5'(lane))] <= 0;
              solo_q[5'(tail_q + 5'(lane))] <= allocate_solo_i[lane];
              pc_q[5'(tail_q + 5'(lane))] <= allocate_pc_i[lane*32 +: 32];
              rd_q[5'(tail_q + 5'(lane))] <= allocate_rd_i[lane*5 +: 5];
              destination_q[5'(tail_q + 5'(lane))] <= allocate_destination_i[lane*6 +: 6];
              stale_q[5'(tail_q + 5'(lane))] <= allocate_stale_i[lane*6 +: 6];
              generation_q[5'(tail_q + 5'(lane))] <= next_generation_q[5'(tail_q + 5'(lane))];
              next_generation_q[5'(tail_q + 5'(lane))] <= next_generation_q[5'(tail_q + 5'(lane))] + 8'd1;
              if (next_generation_q[5'(tail_q + 5'(lane))] == 8'hff)
                exhausted_q[5'(tail_q + 5'(lane))] <= 1;
            end
          end
        end
        for (int lane = 0; lane < 2; lane++)
          if (complete_accept_o[lane]) begin
            done_q[complete_id_i[lane*13 +: 5]] <= 1;
            event_q[complete_id_i[lane*13 +: 5]] <= complete_event_i[lane];
            solo_q[complete_id_i[lane*13 +: 5]] <= solo_q[complete_id_i[lane*13 +: 5]] || complete_solo_i[lane];
          end
      end
      // Reusing generation zero requires every external holder to acknowledge quiescence.
      if (drained_i && count_q == 0 && !clear_window) begin
        exhausted_q <= 0;
        for (int slot = 0; slot < 32; slot++) next_generation_q[slot] <= 0;
      end
    end
  end

`ifndef SYNTHESIS
  always_ff @(posedge clk_i) begin
    if (!rst_i) begin
      assert (count_q <= 32 && $countones(valid_q) == int'(count_q)) else $fatal(1, "ROB2_COUNT");
      for (int slot = 0; slot < 32; slot++)
        assert (valid_q[slot] == (6'(5'(5'(slot) - head_q)) < count_q)) else $fatal(1, "ROB2_WINDOW");
      assert (!drained_i || (count_q == 0 && complete_offer_i == 0 && !resolve_offer_i && allocate_i == 0))
        else $fatal(1, "ROB2_DRAIN");
      if (!clear_window && !branch_recover_o) begin
        assert (allocate_i != 2'b10 && (allocate_i & ~allocate_ready_o) == 0) else $fatal(1, "ROB2_ALLOCATE");
        assert (retire_take_i != 2'b10 && (retire_take_i & ~retire_valid_o) == 0) else $fatal(1, "ROB2_RETIRE");
      end
      if (!clear_window) begin
        assert ((complete_take_i & ~(complete_offer_i & complete_ready_o)) == 0) else $fatal(1, "ROB2_COMPLETE");
        assert (!resolve_take_i || (resolve_offer_i && resolve_ready_o)) else $fatal(1, "ROB2_RESOLVE");
        assert (!trap_take_i || trap_valid_o) else $fatal(1, "ROB2_TRAP");
      end
    end
  end
`endif
endmodule
`default_nettype wire
