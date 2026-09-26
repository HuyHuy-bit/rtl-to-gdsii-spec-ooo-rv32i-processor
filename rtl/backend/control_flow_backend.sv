`default_nettype none
module control_flow_backend #(parameter bit SYSTEM_SERVICE = 0) (
  input wire clk_i, rst_i, flush_i, drained_i,
  input wire cancel_system_i,
  output wire system_prepare_o, system_busy_o, system_redirect_o,
  output wire [31:0] system_redirect_pc_o,
  output wire system_trap_valid_o,
  output commit_event_pkg::commit_event_t system_trap_event_o,
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
  wire [1:0] cfi, selected, producer_ready, system_op, admitted, dispatch_solo;
  wire [5:0] rename_source, unused_rename_source;
  wire read_ready;
  wire [2:0] unused_read_ready;
  wire head_valid, head_read, serial_offer, serial_accept, illegal_offer, illegal_accept, backend_trap_ready;
  wire [12:0] serial_id, illegal_id;
  wire [31:0] head_pc;
  commit_event_pkg::commit_event_t serial_event, illegal_event;
  wire [1:0] backend_complete_accept;
  wire [1:0] backend_complete_offer = illegal_offer ? {1'b0, completion_enable_i[0]} : completion_valid_o & completion_enable_i;
  wire [25:0] backend_complete_id = illegal_offer ? {13'd0, illegal_id} : completion_id_o;
  wire [1:0] backend_complete_solo = illegal_offer ? 2'b01 : 2'b00;
  wire [5:0] system_source;
  commit_event_pkg::commit_event_t [1:0] backend_complete_event;
  assign backend_complete_event = illegal_offer ? {commit_event_pkg::commit_event_t'('0), illegal_event} : completion_event_o;
  assign completion_accept_o = illegal_offer ? 2'b00 : backend_complete_accept;
  assign illegal_accept = illegal_offer && backend_complete_accept[0];
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
    assign system_op[lane] = SYSTEM_SERVICE && !frontend_fault_i[lane] && decoded[lane].op inside {OP_CSR, OP_MRET, OP_WFI};
    assign cfi[lane] = decoded[lane].op inside {OP_BRANCH, OP_JAL, OP_JALR};
    assign supported_o[lane] = frontend_fault_i[lane] || ((cfi[lane] || system_op[lane] || decoded[lane].op inside {OP_ALU, OP_LUI, OP_AUIPC})
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

  if (SYSTEM_SERVICE) begin : systems
    wire descriptor_busy, killed;
    wire [12:0] system_id;
    wire [31:0] system_instruction, unused_pc;
    head_system_dispatch dispatch (
      .clk_i, .rst_i, .flush_i(flush_i || trap_accept_o || cancel_system_i), .drained_i,
      .valid_i(selected), .system_i(system_op), .cfi0_i(cfi[0]),
      .instruction_i(instruction_i[31:0]), .pc_i(pc_i[31:0]),
      .dispatch_valid_o(admitted), .dispatch_solo_o(dispatch_solo),
      .allocate_accept_i(allocate_accept_o), .allocate_id_i(allocate_id_o[12:0]), .source1_i(rename_source[5:0]),
      .queue_dispatch_o(), .head_valid_i(head_valid), .head_id_i(head_id_o), .head_pc_i(head_pc),
      .recover_i(redirect_o), .recover_slot_i(resolve_id_o[4:0]),
      .serial_accept_i(serial_accept), .serial_id_i(serial_id),
      .busy_o(descriptor_busy), .system_valid_o(head_read), .killed_o(killed),
      .system_id_o(system_id), .system_instruction_o(system_instruction), .system_pc_o(unused_pc), .system_source_o(system_source)
    );
    head_system_controller head (
      .clk_i, .rst_i, .cancel_i(flush_i || cancel_system_i), .retire_accept_i(retire_accept_o),
      .head_valid_i(head_valid), .head_id_i(head_id_o), .head_pc_i(head_pc),
      .system_valid_i(head_read), .system_id_i(system_id), .instruction_i(system_instruction),
      .source_ready_i(read_ready), .source_i(read_data[31:0]),
      .prepare_o(system_prepare_o), .busy_o(system_busy_o),
      .fault_valid_i(fault_pending_o), .fault_event_i(fault_event_o),
      .serial_offer_o(serial_offer), .serial_id_o(serial_id), .serial_event_o(serial_event), .serial_accept_i(serial_accept),
      .illegal_offer_o(illegal_offer), .illegal_id_o(illegal_id), .illegal_event_o(illegal_event), .illegal_accept_i(illegal_accept),
      .trap_valid_o(system_trap_valid_o), .trap_event_o(system_trap_event_o), .trap_accept_i(trap_accept_o),
      .redirect_o(system_redirect_o), .redirect_pc_o(system_redirect_pc_o)
    );
    assign backend_trap_ready = system_trap_valid_o && trap_ready_i;
`ifndef SYNTHESIS
    always_ff @(posedge clk_i) if (!rst_i) begin
      assert (!(system_busy_o && redirect_o)) else $fatal(1, "CF_SYSTEM_RECOVERY");
      assert (!head_read || issue_o == 0) else $fatal(1, "CF_SYSTEM_READ");
      assert (!drained_i || (!descriptor_busy && !system_busy_o)) else $fatal(1, "CF_SYSTEM_DRAIN");
      assert (!killed || !system_prepare_o) else $fatal(1, "CF_SYSTEM_CANCEL");
    end
`endif
  end else begin : no_systems
    assign admitted = selected;
    assign dispatch_solo = 0;
    assign head_read = 0;
    assign system_source = 0;
    assign serial_offer = 0;
    assign serial_id = 0;
    assign serial_event = '0;
    assign illegal_offer = 0;
    assign illegal_id = 0;
    assign illegal_event = '0;
    assign backend_trap_ready = trap_ready_i;
    assign system_prepare_o = 0;
    assign system_busy_o = 0;
    assign system_redirect_o = 0;
    assign system_redirect_pc_o = 0;
    assign system_trap_valid_o = 0;
    assign system_trap_event_o = '0;
    wire unused_system = cancel_system_i ^ head_valid ^ ^head_pc ^ ^rename_source ^ ^read_ready ^ serial_accept ^ illegal_accept;
  end

  issue_backend backend (
    .queue_skip_i(system_op), .head_read_i(head_read), .head_source_i(system_source),
    .serial_offer_i(serial_offer), .serial_id_i(serial_id), .serial_event_i(serial_event), .serial_accept_o(serial_accept),
    .clk_i, .rst_i, .flush_i, .drained_i,
    .resources_ready_i((admitted & ~supported_o) == 0),
    .valid_i(admitted), .cfi_i(cfi), .solo_i(dispatch_solo), .rs1_i(rs1), .rs2_i(rs2), .rd_i(rd), .pc_i,
    .eligible_i(eligible), .payload_i(payload), .allocate_accept_o, .allocate_id_o,
    .source1_o({unused_rename_source, rename_source}), .source2_o(), .destination_o(), .stale_o(), .source_ready_o(),
    .complete_offer_i(backend_complete_offer), .complete_solo_i(backend_complete_solo),
    .complete_id_i(backend_complete_id), .complete_event_i(backend_complete_event),
    .complete_accept_o(backend_complete_accept), .wb_accept_o, .wb_destination_o(), .wb_data_o(),
    .resolve_offer_i(resolve_valid_o), .resolve_grant_i(resolve_grant_i && !(SYSTEM_SERVICE && (system_busy_o || fault_pending_o))), .mispredict_i(resolve_mispredict_o), .resolve_id_i(resolve_id_o),
    .resolve_accept_o, .branch_recover_o(redirect_o),
    .retire_ready_i, .retire_valid_o, .retire_accept_o,
    .retire_rd_o(), .retire_destination_o(), .retire_stale_o(), .retire_event_o,
    .trap_ready_i(backend_trap_ready), .trap_valid_o(fault_pending_o), .trap_accept_o, .trap_event_o(fault_event_o),
    .port_ready_i(producer_ready & execution_ready_i), .issue_o, .issue_id_o, .issue_destination_o(),
    .issue_source_o(), .issue_payload_o(issue_payload), .read_data_o(read_data), .read_ready_o({unused_read_ready, read_ready}),
    .head_valid_o(head_valid), .head_id_o, .head_pc_o(head_pc), .occupancy_o, .issue_occupancy_o, .identity_drain_o,
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
