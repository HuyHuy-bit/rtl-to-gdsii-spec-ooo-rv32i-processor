`default_nettype none
module integer_backend (
  input wire clk_i, rst_i, flush_i, drained_i,
  input wire [1:0] valid_i,
  input wire [63:0] instruction_i, pc_i,
  output wire [1:0] supported_o, allocate_accept_o,
  output wire [25:0] allocate_id_o,
  input wire [1:0] execution_ready_i, completion_enable_i, retire_ready_i,
  output wire [1:0] issue_o,
  output wire [25:0] issue_id_o,
  output wire [1:0] completion_valid_o, completion_accept_o, wb_accept_o,
  output wire [25:0] completion_id_o,
  output commit_event_pkg::commit_event_t [1:0] completion_event_o,
  output wire [1:0] retire_valid_o, retire_accept_o,
  output commit_event_pkg::commit_event_t [1:0] retire_event_o,
  output wire [5:0] occupancy_o,
  output wire [4:0] issue_occupancy_o,
  output wire identity_drain_o
);
  import single_lane_pkg::*;
  decoded_t [1:0] decoded;
  wire [9:0] rs1, rs2, rd;
  wire [1:0] port_ready;
  wire [127:0] issue_payload, read_data;
  wire [127:0] payload = {pc_i[63:32], instruction_i[63:32], pc_i[31:0], instruction_i[31:0]};

  for (genvar lane = 0; lane < 2; lane++) begin : lanes
    wire producer_ready;
    decode_single decode (.instruction_i(instruction_i[lane*32 +: 32]), .decoded_o(decoded[lane]));
    assign supported_o[lane] = decoded[lane].op inside {OP_ALU, OP_LUI, OP_AUIPC}
      && pc_i[lane*32 +: 2] == 0;
    assign rs1[lane*5 +: 5] = decoded[lane].rs1;
    assign rs2[lane*5 +: 5] = decoded[lane].rs2;
    assign rd[lane*5 +: 5] = decoded[lane].rd;
    assign port_ready[lane] = producer_ready && execution_ready_i[lane];
    alu_pipeline producer (
      .clk_i, .rst_i, .flush_i, .launch_i(issue_o[lane]), .ready_o(producer_ready),
      .id_i(issue_id_o[lane*13 +: 13]),
      .instruction_i(issue_payload[lane*64 +: 32]), .pc_i(issue_payload[lane*64+32 +: 32]),
      .source1_i(read_data[lane*64 +: 32]), .source2_i(read_data[lane*64+32 +: 32]),
      .valid_o(completion_valid_o[lane]), .id_o(completion_id_o[lane*13 +: 13]),
      .event_o(completion_event_o[lane]), .take_i(completion_accept_o[lane])
    );
  end

  issue_backend backend (
    .clk_i, .rst_i, .flush_i, .drained_i,
    .resources_ready_i((valid_i & ~supported_o) == 0),
    .valid_i, .cfi_i(2'b00), .solo_i(2'b00), .rs1_i(rs1), .rs2_i(rs2), .rd_i(rd), .pc_i,
    .eligible_i(4'b1111), .payload_i(payload), .allocate_accept_o, .allocate_id_o,
    .source1_o(), .source2_o(), .destination_o(), .stale_o(), .source_ready_o(),
    .complete_offer_i(completion_valid_o & completion_enable_i), .complete_solo_i(2'b00),
    .complete_id_i(completion_id_o), .complete_event_i(completion_event_o),
    .complete_accept_o(completion_accept_o), .wb_accept_o, .wb_destination_o(), .wb_data_o(),
    .resolve_offer_i(1'b0), .resolve_grant_i(1'b0), .mispredict_i(1'b0), .resolve_id_i(13'd0),
    .resolve_accept_o(), .branch_recover_o(),
    .retire_ready_i, .retire_valid_o, .retire_accept_o,
    .retire_rd_o(), .retire_destination_o(), .retire_stale_o(), .retire_event_o,
    .trap_ready_i(1'b0), .trap_valid_o(), .trap_accept_o(), .trap_event_o(),
    .port_ready_i(port_ready), .issue_o, .issue_id_o, .issue_destination_o(),
    .issue_source_o(), .issue_payload_o(issue_payload), .read_data_o(read_data), .read_ready_o(),
    .head_valid_o(), .head_id_o(), .head_pc_o(), .occupancy_o, .issue_occupancy_o, .identity_drain_o,
    .rat_o(), .committed_o(), .free_o(), .ready_o(),
    .checkpoint_accept_o(), .checkpoint_id_o(), .checkpoint_valid_o(), .checkpoint_released_o()
  );

`ifndef SYNTHESIS
  always_ff @(posedge clk_i) begin
    if (!rst_i) begin
      assert (valid_i != 2'b10) else $fatal(1, "INTEGER_PREFIX");
      assert (!drained_i || completion_valid_o == 0) else $fatal(1, "INTEGER_DRAIN");
    end
  end
`endif
endmodule
`default_nettype wire
