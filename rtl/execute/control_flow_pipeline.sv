`default_nettype none
module control_flow_pipeline (
  input wire clk_i, rst_i, flush_i,
  input wire launch_i,
  output wire ready_o, busy_o,
  input wire [12:0] id_i,
  input wire [31:0] instruction_i, pc_i, source1_i, source2_i,
  input wire predicted_taken_i,
  input wire [31:0] predicted_pc_i,
  output wire valid_o,
  output logic [12:0] id_o,
  output commit_event_pkg::commit_event_t event_o,
  input wire take_i,
  output wire resolve_valid_o,
  output logic taken_o, mispredict_o,
  output wire [31:0] next_pc_o,
  input wire resolve_take_i
);
  import single_lane_pkg::*;
  import commit_event_pkg::*;
  decoded_t decoded;
  wire [31:0] result, next_pc, cause, trap_value;
  wire execute_trap;
  wire cfi = decoded.op inside {OP_BRANCH, OP_JAL, OP_JALR};
  logic pending_q, cfi_q, resolved_q, taken;
  commit_event_t built;

  assign busy_o = pending_q && !rst_i && !flush_i;
  assign ready_o = !rst_i && !flush_i && (!pending_q || take_i);
  assign resolve_valid_o = busy_o && cfi_q && !event_o.trap && !resolved_q;
  assign valid_o = busy_o && (!cfi_q || event_o.trap || resolved_q);
  assign next_pc_o = event_o.pc_after;

  decode_single decode (.instruction_i, .decoded_o(decoded));
  execute_single execute (
    .decoded_i(decoded), .pc_i, .instruction_i, .source1_i, .source2_i,
    .csr_value_i(32'd0), .mepc_i(32'd0), .csr_legal_i(1'b0),
    .result_o(result), .next_pc_o(next_pc), .address_o(),
    .trap_o(execute_trap), .cause_o(cause), .trap_value_o(trap_value)
  );

  always_comb begin
    taken = decoded.op inside {OP_JAL, OP_JALR};
    if (decoded.op == OP_BRANCH) begin
      case (decoded.funct3)
        0: taken = source1_i == source2_i;
        1: taken = source1_i != source2_i;
        4: taken = $signed(source1_i) < $signed(source2_i);
        5: taken = $signed(source1_i) >= $signed(source2_i);
        6: taken = source1_i < source2_i;
        7: taken = source1_i >= source2_i;
        default: taken = 0;
      endcase
    end
    built = '0;
    built.valid = 1;
    built.retired = !execute_trap;
    built.privilege = 3;
    built.instruction = instruction_i;
    built.pc_before = pc_i;
    built.pc_after = next_pc;
    built.rs1_addr = decoded.rs1;
    built.rs1_value = source1_i;
    built.rs2_addr = decoded.rs2;
    built.rs2_value = source2_i;
    built.trap = execute_trap;
    built.trap_cause = cause;
    built.trap_value = trap_value;
    if (!execute_trap && decoded.rd != 0) begin
      built.rd_addr = decoded.rd;
      built.rd_value = result;
      built.rd_write_mask = '1;
    end
  end

  // Resolution is acknowledged once; the result waits for a later completion grant.
  always_ff @(posedge clk_i) begin
    if (rst_i || flush_i) begin
      pending_q <= 0;
      cfi_q <= 0;
      resolved_q <= 0;
    end else begin
      if (resolve_take_i) resolved_q <= 1;
      if (take_i) pending_q <= 0;
      if (launch_i) begin
        pending_q <= 1;
        cfi_q <= cfi;
        resolved_q <= 0;
        id_o <= id_i;
        event_o <= built;
        taken_o <= taken;
        mispredict_o <= cfi && (predicted_taken_i != taken || predicted_pc_i != next_pc);
      end
    end
  end

`ifndef SYNTHESIS
  always_ff @(posedge clk_i) begin
    if (!rst_i && !flush_i) begin
      assert (!launch_i || ready_o) else $fatal(1, "CF_PIPE_CAPACITY");
      assert (!take_i || valid_o) else $fatal(1, "CF_PIPE_TAKE");
      assert (!resolve_take_i || resolve_valid_o) else $fatal(1, "CF_PIPE_RESOLVE");
      assert (!launch_i || decoded.op inside {OP_ALU, OP_LUI, OP_AUIPC, OP_BRANCH, OP_JAL, OP_JALR})
        else $fatal(1, "CF_PIPE_OPERATION");
    end
  end
`endif
endmodule
`default_nettype wire
