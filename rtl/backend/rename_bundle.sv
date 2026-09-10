`default_nettype none
module rename_bundle (
  input wire rst_i, recover_i, resources_ready_i,
  input wire [1:0] valid_i, checkpoint_i,
  input wire [9:0] rs1_i, rs2_i, rd_i,
  input wire [191:0] rat_i,
  input wire [63:0] free_i, ready_i,
  output logic [1:0] accept_o,
  output logic [11:0] source1_o, source2_o, destination_o, stale_o,
  output logic [3:0] source_ready_o,
  output logic [63:0] allocation_o
);
  logic [1:0] selected;
  logic [63:0] available;
  logic [5:0] destination [0:1], source1 [0:1], source2 [0:1], stale [0:1];
  logic [1:0] operand_ready [0:1];
  logic enough;

  always_comb begin
    selected = valid_i;
    if (checkpoint_i[0]) selected[1] = 0;
    available = free_i & 64'hfffffffffffffffe;
    enough = valid_i != 2'b10;
    for (int lane = 0; lane < 2; lane++) begin
      source1[lane] = rat_i[rs1_i[lane*5 +: 5]*6 +: 6];
      source2[lane] = rat_i[rs2_i[lane*5 +: 5]*6 +: 6];
      stale[lane] = rd_i[lane*5 +: 5] == 0 ? 6'd0 : rat_i[rd_i[lane*5 +: 5]*6 +: 6];
      operand_ready[lane] = {ready_i[source2[lane]], ready_i[source1[lane]]};
      destination[lane] = 0;
      if (selected[lane] && rd_i[lane*5 +: 5] != 0) begin
        for (int phys = 63; phys >= 1; phys--)
          if (available[phys]) destination[lane] = 6'(phys);
        if (destination[lane] == 0) enough = 0;
        available[destination[lane]] = 0;
      end
    end

    // Lane one sees lane zero's new mapping, including its stale tag on WAW.
    if (selected[0] && rd_i[0 +: 5] != 0) begin
      if (rs1_i[5 +: 5] == rd_i[0 +: 5]) begin
        source1[1] = destination[0];
        operand_ready[1][0] = 0;
      end
      if (rs2_i[5 +: 5] == rd_i[0 +: 5]) begin
        source2[1] = destination[0];
        operand_ready[1][1] = 0;
      end
      if (rd_i[5 +: 5] == rd_i[0 +: 5]) stale[1] = destination[0];
    end

    accept_o = 0;
    source1_o = 0;
    source2_o = 0;
    destination_o = 0;
    stale_o = 0;
    source_ready_o = 0;
    allocation_o = 0;
    if (!rst_i && !recover_i && resources_ready_i && enough) begin
      accept_o = selected;
      for (int lane = 0; lane < 2; lane++) begin
        if (selected[lane]) begin
          source1_o[lane*6 +: 6] = source1[lane];
          source2_o[lane*6 +: 6] = source2[lane];
          destination_o[lane*6 +: 6] = destination[lane];
          stale_o[lane*6 +: 6] = stale[lane];
          source_ready_o[lane*2 +: 2] = operand_ready[lane];
          if (destination[lane] != 0) allocation_o[destination[lane]] = 1;
        end
      end
    end
  end

  // Only lane zero can cut a two-instruction bundle; lane one already ends it.
  wire unused_checkpoint_lane1 = checkpoint_i[1];
endmodule
`default_nettype wire
