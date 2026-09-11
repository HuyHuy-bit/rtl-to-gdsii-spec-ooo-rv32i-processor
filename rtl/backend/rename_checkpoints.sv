`default_nettype none
module rename_checkpoints (
  input wire clk_i, rst_i, flush_i,
  input wire create_i,
  input wire [191:0] snapshot_i,
  input wire [63:0] allocation_i,
  input wire resolve_i, mispredict_i,
  input wire [2:0] resolve_id_i,
  output wire create_ready_o, create_accept_o,
  output wire [2:0] create_id_o,
  output logic restore_valid_o,
  output logic [191:0] restore_rat_o,
  output logic [63:0] reclaim_o,
  output logic [7:0] released_o,
  output wire [7:0] valid_o
);
  logic [7:0] valid_q, valid_d;
  logic [191:0] snapshot_q [0:7], snapshot_d [0:7];
  logic [63:0] history_q [0:7], history_d [0:7];
  logic [7:0] older_q [0:7], older_d [0:7];
  logic [2:0] candidate;
  wire recovering = resolve_i && mispredict_i;

  always_comb begin
    candidate = 0;
    for (int slot = 7; slot >= 0; slot--)
      if (!valid_q[slot]) candidate = 3'(slot);
  end
  assign valid_o = valid_q;
  assign create_ready_o = !rst_i && !flush_i && !recovering && !(&valid_q);
  assign create_accept_o = create_i && create_ready_o;
  assign create_id_o = create_accept_o ? candidate : 3'd0;

  always_comb begin
    restore_valid_o = 0;
    restore_rat_o = 0;
    reclaim_o = 0;
    released_o = 0;
    if (!rst_i && !flush_i && resolve_i && valid_q[resolve_id_i]) begin
      released_o[resolve_id_i] = 1;
      if (mispredict_i) begin
        restore_valid_o = 1;
        restore_rat_o = snapshot_q[resolve_id_i];
        reclaim_o = history_q[resolve_id_i];
        released_o = valid_q & ~older_q[resolve_id_i];
      end
    end
  end

  always_comb begin
    valid_d = valid_q & ~released_o;
    for (int slot = 0; slot < 8; slot++) begin
      snapshot_d[slot] = snapshot_q[slot];
      older_d[slot] = older_q[slot] & ~released_o;
      history_d[slot] = history_q[slot];
      if (recovering)
        history_d[slot] = history_q[slot] & ~reclaim_o;
      else if (valid_q[slot])
        history_d[slot] = history_q[slot] | allocation_i;
    end
    // The new snapshot includes the CFI; its younger-allocation history starts empty.
    if (create_accept_o) begin
      valid_d[candidate] = 1;
      snapshot_d[candidate] = snapshot_i;
      older_d[candidate] = valid_q & ~released_o;
      history_d[candidate] = 0;
    end
    if (rst_i || flush_i) valid_d = 0;
    for (int slot = 0; slot < 8; slot++) begin
      if (!valid_d[slot]) begin
        snapshot_d[slot] = 0;
        history_d[slot] = 0;
        older_d[slot] = 0;
      end
    end
  end

  always_ff @(posedge clk_i) begin
    valid_q <= valid_d;
    for (int slot = 0; slot < 8; slot++) begin
      snapshot_q[slot] <= snapshot_d[slot];
      history_q[slot] <= history_d[slot];
      older_q[slot] <= older_d[slot];
    end
  end

`ifndef SYNTHESIS
  always_ff @(posedge clk_i) begin
    if (!rst_i && !flush_i) begin
      if (resolve_i)
        assert (valid_q[resolve_id_i]) else $fatal(1, "CHECKPOINT_RESOLVE_LIVE");
      if (!recovering) begin
        assert (!allocation_i[0]) else $fatal(1, "CHECKPOINT_ALLOCATE_ZERO");
        assert (!create_i || create_accept_o || allocation_i == 0)
          else $fatal(1, "CHECKPOINT_STALLED_ALLOCATION");
        for (int slot = 0; slot < 8; slot++)
          if (valid_q[slot])
            assert ((history_q[slot] & allocation_i) == 0)
              else $fatal(1, "CHECKPOINT_DUPLICATE_ALLOCATION");
      end
      if (create_accept_o) begin
        assert (snapshot_i[5:0] == 0) else $fatal(1, "CHECKPOINT_MAP_ZERO");
        for (int arch = 1; arch < 32; arch++) begin
          assert (snapshot_i[arch*6 +: 6] != 0) else $fatal(1, "CHECKPOINT_MAP_ZERO");
          for (int other = 1; other < arch; other++)
            assert (snapshot_i[arch*6 +: 6] != snapshot_i[other*6 +: 6])
              else $fatal(1, "CHECKPOINT_MAP_ALIAS");
        end
      end
      for (int slot = 0; slot < 8; slot++) begin
        assert ((older_q[slot] & ~valid_q) == 0 && !older_q[slot][slot] && !history_q[slot][0])
          else $fatal(1, "CHECKPOINT_HISTORY");
        if (valid_q[slot])
          for (int other = 0; other < slot; other++)
            if (valid_q[other])
              assert (older_q[slot][other] != older_q[other][slot])
                else $fatal(1, "CHECKPOINT_AGE_ORDER");
      end
    end
  end
`endif
endmodule
`default_nettype wire
