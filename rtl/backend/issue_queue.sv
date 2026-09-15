`default_nettype none
module issue_queue (
  input wire clk_i, rst_i, flush_i,
  input wire recover_i,
  input wire [4:0] head_slot_i, recover_slot_i,
  input wire [1:0] dispatch_i,
  output logic [1:0] dispatch_ready_o,
  input wire [25:0] dispatch_id_i,
  input wire [23:0] dispatch_source_i,
  input wire [3:0] dispatch_ready_i, dispatch_eligible_i,
  input wire [11:0] dispatch_destination_i,
  input wire [127:0] dispatch_payload_i,
  input wire [1:0] wb_accept_i,
  input wire [11:0] wb_destination_i,
  input wire [1:0] port_ready_i,
  output logic [1:0] issue_o,
  output logic [25:0] issue_id_o,
  output logic [23:0] read_address_o,
  output logic [11:0] issue_destination_o,
  output logic [127:0] issue_payload_o,
  output logic [4:0] occupancy_o
);
  logic [15:0] valid_q;
  logic [12:0] id_q [16];
  logic [11:0] source_q [16];
  logic [1:0] ready_q [16], eligible_q [16];
  logic [5:0] destination_q [16];
  logic [63:0] payload_q [16];
  logic [1:0] awake [16];
  logic [15:0] chosen [2];
  logic [3:0] free_slot [2];
  logic [4:0] best_age [2];
  logic found_free;
  wire stop = rst_i || flush_i || recover_i;

  always_comb begin
    occupancy_o = 0;
    free_slot[0] = 0;
    free_slot[1] = 0;
    found_free = 0;
    for (int slot = 0; slot < 16; slot++) begin
      occupancy_o = occupancy_o + 5'(valid_q[slot]);
      if (!valid_q[slot]) begin
        if (!found_free) free_slot[0] = 4'(slot);
        else free_slot[1] = 4'(slot);
        found_free = 1;
      end
    end
    dispatch_ready_o[0] = !stop && occupancy_o < 16;
    dispatch_ready_o[1] = !stop && occupancy_o < 15;
  end

  // Only the backend's accepted writes may wake a resident instruction.
  always_comb begin
    for (int slot = 0; slot < 16; slot++) begin
      awake[slot] = ready_q[slot];
      for (int operand = 0; operand < 2; operand++) begin
        awake[slot][operand] = awake[slot][operand] || source_q[slot][operand*6 +: 6] == 0;
        for (int lane = 0; lane < 2; lane++)
          if (wb_accept_i[lane] && wb_destination_i[lane*6 +: 6] != 0
              && wb_destination_i[lane*6 +: 6] == source_q[slot][operand*6 +: 6])
            awake[slot][operand] = 1;
      end
    end
  end

  // Port zero selects first; ROB-relative age is independent of IQ storage order.
  always_comb begin
    issue_o = 0;
    issue_id_o = 0;
    read_address_o = 0;
    issue_destination_o = 0;
    issue_payload_o = 0;
    for (int port_id = 0; port_id < 2; port_id++) begin
      chosen[port_id] = 0;
      best_age[port_id] = 0;
      for (int slot = 0; slot < 16; slot++) begin
        if (!stop && port_ready_i[port_id] && valid_q[slot] && (&awake[slot])
            && eligible_q[slot][port_id] && (port_id == 0 || !chosen[0][slot])
            && (!issue_o[port_id] || 5'(id_q[slot][4:0] - head_slot_i) < best_age[port_id])) begin
          issue_o[port_id] = 1;
          chosen[port_id] = 16'b1 << slot;
          best_age[port_id] = 5'(id_q[slot][4:0] - head_slot_i);
          issue_id_o[port_id*13 +: 13] = id_q[slot];
          read_address_o[port_id*12 +: 12] = source_q[slot];
          issue_destination_o[port_id*6 +: 6] = destination_q[slot];
          issue_payload_o[port_id*64 +: 64] = payload_q[slot];
        end
      end
    end
  end

  always_ff @(posedge clk_i) begin
    if (rst_i || flush_i) valid_q <= 0;
    else begin
      for (int slot = 0; slot < 16; slot++) begin
        if (valid_q[slot]) ready_q[slot] <= awake[slot];
        if (recover_i && 5'(id_q[slot][4:0] - head_slot_i) > 5'(recover_slot_i - head_slot_i))
          valid_q[slot] <= 0;
        if (chosen[0][slot] || chosen[1][slot]) valid_q[slot] <= 0;
      end
      for (int lane = 0; lane < 2; lane++) begin
        if (!stop && dispatch_i[lane]) begin
          valid_q[free_slot[lane]] <= 1;
          id_q[free_slot[lane]] <= dispatch_id_i[lane*13 +: 13];
          source_q[free_slot[lane]] <= dispatch_source_i[lane*12 +: 12];
          ready_q[free_slot[lane]] <= dispatch_ready_i[lane*2 +: 2];
          eligible_q[free_slot[lane]] <= dispatch_eligible_i[lane*2 +: 2];
          destination_q[free_slot[lane]] <= dispatch_destination_i[lane*6 +: 6];
          payload_q[free_slot[lane]] <= dispatch_payload_i[lane*64 +: 64];
        end
      end
    end
  end

`ifndef SYNTHESIS
  always_ff @(posedge clk_i) begin
    if (!stop) begin
      assert (dispatch_i != 2'b10 && (dispatch_i & ~dispatch_ready_o) == 0) else $fatal(1, "IQ_DISPATCH");
      for (int lane = 0; lane < 2; lane++) begin
        if (dispatch_i[lane]) begin
          assert (dispatch_eligible_i[lane*2 +: 2] != 0) else $fatal(1, "IQ_ELIGIBILITY");
          for (int slot = 0; slot < 16; slot++)
            assert (!valid_q[slot] || id_q[slot][4:0] != dispatch_id_i[lane*13 +: 5])
              else $fatal(1, "IQ_IDENTITY");
        end
      end
      assert (dispatch_i != 2'b11 || dispatch_id_i[4:0] != dispatch_id_i[17:13])
        else $fatal(1, "IQ_IDENTITY");
      assert ((chosen[0] & chosen[1]) == 0) else $fatal(1, "IQ_DUPLICATE_ISSUE");
    end
  end
`endif
endmodule
`default_nettype wire
