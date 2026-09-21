`default_nettype none
module fetch_execution_core #(parameter bit FRONTEND_FAULTS = 1) (
  input wire clk_i, rst_i, enable_i, flush_i, drained_i,
  input wire [31:0] flush_pc_i,
  output wire request_valid_o,
  input wire request_ready_i,
  output memory_protocol_pkg::mem_request_t request_o,
  input wire response_valid_i,
  output wire response_ready_o,
  input memory_protocol_pkg::mem_response_t response_i,
  input wire [1:0] execution_ready_i, completion_enable_i, retire_ready_i,
  input wire resolve_grant_i,
  output wire [1:0] retire_valid_o, retire_accept_o,
  output commit_event_pkg::commit_event_t [1:0] retire_event_o,
  output wire redirect_o,
  output wire [31:0] redirect_pc_o,
  output wire [1:0] fetch_valid_o, dispatch_o, unsupported_o, producer_busy_o,
  output wire [63:0] fetch_pc_o, fetch_instruction_o,
  output wire fetch_busy_o, fetch_fault_o,
  output wire [31:0] fetch_fault_cause_o,
  output wire backend_fault_o,
  output commit_event_pkg::commit_event_t backend_fault_event_o,
  output wire [5:0] occupancy_o,
  output wire identity_drain_o, fatal_o
);
  wire [1:0] supported, backend_valid, frontend_fault;
  wire [63:0] frontend_cause, frontend_value;
  if (FRONTEND_FAULTS) begin : faults
    frontend_fault_decode decode (
      .valid_i(fetch_valid_o), .instruction_i(fetch_instruction_o), .pc_i(fetch_pc_o),
      .fetch_fault_i(fetch_fault_o), .fetch_cause_i(fetch_fault_cause_o),
      .fault_o(frontend_fault), .cause_o(frontend_cause), .value_o(frontend_value)
    );
  end else begin : no_faults
    assign frontend_fault = 0;
    assign frontend_cause = 0;
    assign frontend_value = 0;
  end
  wire [63:0] predicted_pc = {fetch_pc_o[63:32] + 32'd4, fetch_pc_o[31:0] + 32'd4};
  wire branch_redirect;
  wire [31:0] branch_pc;
  wire running = !rst_i && !fatal_o;
  assign redirect_o = running && (flush_i || branch_redirect);
  assign redirect_pc_o = flush_i ? flush_pc_i : branch_pc;
  assign backend_valid = fetch_valid_o & {2{running && (FRONTEND_FAULTS || !fetch_fault_o) && !drained_i}};
  assign unsupported_o = fetch_valid_o & ~supported & {2{!fetch_fault_o}};

  fetch_two_wide frontend (
    .clk_i, .rst_i, .enable_i(enable_i && !drained_i),
    .redirect_i(redirect_o), .redirect_pc_i(redirect_pc_o),
    .request_valid_o, .request_ready_i, .request_o,
    .response_valid_i, .response_ready_o, .response_i,
    .valid_o(fetch_valid_o), .take_i(dispatch_o), .instruction_o(fetch_instruction_o), .pc_o(fetch_pc_o),
    .fault_o(fetch_fault_o), .fault_cause_o(fetch_fault_cause_o), .busy_o(fetch_busy_o), .fatal_o
  );
  control_flow_backend backend (
    .clk_i, .rst_i, .flush_i(flush_i && running), .drained_i,
    .valid_i(backend_valid), .instruction_i(fetch_instruction_o), .pc_i(fetch_pc_o),
    .frontend_fault_i(frontend_fault), .frontend_cause_i(frontend_cause), .frontend_value_i(frontend_value),
    .predicted_pc_i(predicted_pc), .predicted_taken_i(2'b00),
    .supported_o(supported), .allocate_accept_o(dispatch_o), .allocate_id_o(),
    .execution_ready_i(execution_ready_i & {2{running}}),
    .completion_enable_i(completion_enable_i & {2{running}}),
    .retire_ready_i(retire_ready_i & {2{running}}), .resolve_grant_i(resolve_grant_i && running),
    .resolve_valid_o(), .resolve_accept_o(), .resolve_taken_o(), .resolve_mispredict_o(),
    .resolve_id_o(), .resolve_pc_o(branch_pc), .redirect_o(branch_redirect),
    .issue_o(), .producer_busy_o, .issue_id_o(),
    .completion_valid_o(), .completion_accept_o(), .wb_accept_o(), .completion_id_o(), .completion_event_o(),
    .retire_valid_o, .retire_accept_o, .retire_event_o,
    .fault_pending_o(backend_fault_o), .fault_event_o(backend_fault_event_o),
    .head_id_o(), .occupancy_o, .issue_occupancy_o(), .checkpoint_valid_o(), .identity_drain_o
  );
`ifndef SYNTHESIS
  always_ff @(posedge clk_i) begin
    if (!rst_i) begin
      assert (!drained_i || (!enable_i && !fetch_busy_o && fetch_valid_o == 0 && !fatal_o))
        else $fatal(1, "FETCH_CORE_DRAIN");
      assert (!redirect_o || (dispatch_o == 0 && retire_accept_o == 0))
        else $fatal(1, "FETCH_CORE_REDIRECT");
    end
  end
`endif
endmodule
`default_nettype wire
