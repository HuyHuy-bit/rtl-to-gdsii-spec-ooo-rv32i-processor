`default_nettype none
module control_flow_backend (
  input wire clk_i, rst_i, flush_i, drained_i,
  input wire [1:0] valid_i,
  input wire [63:0] instruction_i, pc_i, predicted_pc_i,
  input wire [1:0] predicted_taken_i,
  input wire [1:0] frontend_fault_i,
  input wire [63:0] frontend_cause_i, frontend_value_i,
  output wire [1:0] supported_o, allocate_accept_o,
  output wire [25:0] allocate_id_o,
  input wire [1:0] execution_ready_i, completion_enable_i, retire_ready_i,
  input wire resolve_grant_i,
  output wire resolve_valid_o, resolve_accept_o, resolve_taken_o, resolve_mispredict_o,
  output wire [12:0] resolve_id_o,
  output wire [31:0] resolve_pc_o,
  output wire redirect_o,
  output wire [1:0] issue_o, producer_busy_o,
  output wire [25:0] issue_id_o,
  output wire [1:0] completion_valid_o, completion_accept_o, wb_accept_o,
  output wire [25:0] completion_id_o,
  output commit_event_pkg::commit_event_t [1:0] completion_event_o,
  output wire [1:0] retire_valid_o, retire_accept_o,
  output commit_event_pkg::commit_event_t [1:0] retire_event_o,
  input wire trap_ready_i,
  output wire trap_accept_o,
  output wire fault_pending_o,
  output commit_event_pkg::commit_event_t fault_event_o,
  output wire [12:0] head_id_o,
  output wire [5:0] occupancy_o,
  output wire [4:0] issue_occupancy_o,
  output wire [7:0] checkpoint_valid_o,
  output wire identity_drain_o
);
  import single_lane_pkg::*;
  decoded_t [1:0] decoded;
  wire [9:0] rs1, rs2, rd;
  wire [1:0] cfi, selected, producer_ready;
  wire [3:0] eligible;
  wire [127:0] issue_payload, read_data;
  logic [31:0] prediction_pc_q [32];
  logic [31:0] prediction_taken_q;
  wire [127:0] payload;
  wire [63:0] executable_instruction;
  commit_event_pkg::commit_event_t [1:0] producer_event;
  logic [31:0] fault_q;
  logic [31:0] fault_instruction_q [32], fault_pc_q [32], fault_cause_q [32], fault_value_q [32];
  wire producer_flush = flush_i || trap_accept_o;
  wire cancel_alu1 = redirect_o && 5'(completion_id_o[17:13] - head_id_o[4:0])
      > 5'(resolve_id_o[4:0] - head_id_o[4:0]);

  assign selected = {valid_i[1] && !cfi[0] && !frontend_fault_i[0], valid_i[0]};
  assign resolve_id_o = completion_id_o[12:0];
  assign producer_busy_o[1] = completion_valid_o[1];

  for (genvar lane = 0; lane < 2; lane++) begin : lanes
    assign executable_instruction[lane*32 +: 32] = frontend_fault_i[lane] ? 32'h00000013 : instruction_i[lane*32 +: 32];
    assign payload[lane*64 +: 64] = {frontend_fault_i[lane] ? 32'd0 : pc_i[lane*32 +: 32], executable_instruction[lane*32 +: 32]};
    decode_single decode (.instruction_i(executable_instruction[lane*32 +: 32]), .decoded_o(decoded[lane]));
    assign cfi[lane] = decoded[lane].op inside {OP_BRANCH, OP_JAL, OP_JALR};
    assign supported_o[lane] = frontend_fault_i[lane] || ((cfi[lane] || decoded[lane].op inside {OP_ALU, OP_LUI, OP_AUIPC})
      && pc_i[lane*32 +: 2] == 0);
    assign rs1[lane*5 +: 5] = decoded[lane].rs1;
    assign rs2[lane*5 +: 5] = decoded[lane].rs2;
    assign rd[lane*5 +: 5] = decoded[lane].rd;
    assign eligible[lane*2 +: 2] = cfi[lane] ? 2'b01 : 2'b11;
  end

  always_ff @(posedge clk_i) begin
    for (int lane = 0; lane < 2; lane++) begin
      if (allocate_accept_o[lane]) begin
        fault_q[allocate_id_o[lane*13 +: 5]] <= frontend_fault_i[lane];
        if (frontend_fault_i[lane]) begin
          fault_instruction_q[allocate_id_o[lane*13 +: 5]] <= instruction_i[lane*32 +: 32];
          fault_pc_q[allocate_id_o[lane*13 +: 5]] <= pc_i[lane*32 +: 32];
          fault_cause_q[allocate_id_o[lane*13 +: 5]] <= frontend_cause_i[lane*32 +: 32];
          fault_value_q[allocate_id_o[lane*13 +: 5]] <= frontend_value_i[lane*32 +: 32];
        end
      end
      if (allocate_accept_o[lane] && cfi[lane]) begin
        prediction_pc_q[allocate_id_o[lane*13 +: 5]] <= predicted_pc_i[lane*32 +: 32];
        prediction_taken_q[allocate_id_o[lane*13 +: 5]] <= predicted_taken_i[lane];
      end
    end
  end

  // Faults use a resultless scheduling token; only their saved metadata reaches the ROB.
  always_comb begin
    completion_event_o = producer_event;
    for (int lane = 0; lane < 2; lane++) begin
      if (fault_q[completion_id_o[lane*13 +: 5]]) begin
        completion_event_o[lane] = '0;
        completion_event_o[lane].valid = 1;
        completion_event_o[lane].privilege = 3;
        completion_event_o[lane].instruction = fault_instruction_q[completion_id_o[lane*13 +: 5]];
        completion_event_o[lane].pc_before = fault_pc_q[completion_id_o[lane*13 +: 5]];
        completion_event_o[lane].pc_after = fault_pc_q[completion_id_o[lane*13 +: 5]];
        completion_event_o[lane].trap = 1;
        completion_event_o[lane].trap_cause = fault_cause_q[completion_id_o[lane*13 +: 5]];
        completion_event_o[lane].trap_value = fault_value_q[completion_id_o[lane*13 +: 5]];
      end
    end
  end

  control_flow_pipeline producer0 (
    .clk_i, .rst_i, .flush_i(producer_flush), .launch_i(issue_o[0]), .ready_o(producer_ready[0]), .busy_o(producer_busy_o[0]),
    .id_i(issue_id_o[12:0]), .instruction_i(issue_payload[31:0]), .pc_i(issue_payload[63:32]),
    .source1_i(read_data[31:0]), .source2_i(read_data[63:32]),
    .predicted_taken_i(prediction_taken_q[issue_id_o[4:0]]), .predicted_pc_i(prediction_pc_q[issue_id_o[4:0]]),
    .valid_o(completion_valid_o[0]), .id_o(completion_id_o[12:0]), .event_o(producer_event[0]),
    .take_i(completion_accept_o[0]), .resolve_valid_o, .taken_o(resolve_taken_o),
    .mispredict_o(resolve_mispredict_o), .next_pc_o(resolve_pc_o), .resolve_take_i(resolve_accept_o)
  );

  // The sole resolver is port zero; only a younger port-one held result can be canceled here.
  alu_pipeline producer1 (
    .clk_i, .rst_i, .flush_i(producer_flush || cancel_alu1), .launch_i(issue_o[1]), .ready_o(producer_ready[1]),
    .id_i(issue_id_o[25:13]), .instruction_i(issue_payload[95:64]), .pc_i(issue_payload[127:96]),
    .source1_i(read_data[95:64]), .source2_i(read_data[127:96]),
    .valid_o(completion_valid_o[1]), .id_o(completion_id_o[25:13]), .event_o(producer_event[1]),
    .take_i(completion_accept_o[1])
  );

  issue_backend backend (
    .clk_i, .rst_i, .flush_i, .drained_i,
    .resources_ready_i((selected & ~supported_o) == 0),
    .valid_i(selected), .cfi_i(cfi), .solo_i(2'b00), .rs1_i(rs1), .rs2_i(rs2), .rd_i(rd), .pc_i,
    .eligible_i(eligible), .payload_i(payload), .allocate_accept_o, .allocate_id_o,
    .source1_o(), .source2_o(), .destination_o(), .stale_o(), .source_ready_o(),
    .complete_offer_i(completion_valid_o & completion_enable_i), .complete_solo_i(2'b00),
    .complete_id_i(completion_id_o), .complete_event_i(completion_event_o),
    .complete_accept_o(completion_accept_o), .wb_accept_o, .wb_destination_o(), .wb_data_o(),
    .resolve_offer_i(resolve_valid_o), .resolve_grant_i, .mispredict_i(resolve_mispredict_o), .resolve_id_i(resolve_id_o),
    .resolve_accept_o, .branch_recover_o(redirect_o),
    .retire_ready_i, .retire_valid_o, .retire_accept_o,
    .retire_rd_o(), .retire_destination_o(), .retire_stale_o(), .retire_event_o,
    .trap_ready_i, .trap_valid_o(fault_pending_o), .trap_accept_o, .trap_event_o(fault_event_o),
    .port_ready_i(producer_ready & execution_ready_i), .issue_o, .issue_id_o, .issue_destination_o(),
    .issue_source_o(), .issue_payload_o(issue_payload), .read_data_o(read_data), .read_ready_o(),
    .head_valid_o(), .head_id_o, .head_pc_o(), .occupancy_o, .issue_occupancy_o, .identity_drain_o,
    .rat_o(), .committed_o(), .free_o(), .ready_o(),
    .checkpoint_accept_o(), .checkpoint_id_o(), .checkpoint_valid_o, .checkpoint_released_o()
  );

`ifndef SYNTHESIS
  always_ff @(posedge clk_i) begin
    if (!rst_i) begin
      assert (valid_i != 2'b10) else $fatal(1, "CF_BACKEND_PREFIX");
      assert (!drained_i || producer_busy_o == 0) else $fatal(1, "CF_BACKEND_DRAIN");
      assert (!resolve_accept_o || (resolve_valid_o && resolve_grant_i)) else $fatal(1, "CF_BACKEND_RESOLVE");
      assert (!redirect_o || (allocate_accept_o == 0 && issue_o == 0 && retire_accept_o == 0))
        else $fatal(1, "CF_BACKEND_RECOVERY");
    end
  end
`endif
endmodule
`default_nettype wire
