`default_nettype none
module alu_pipeline (
  input wire clk_i, rst_i, flush_i,
  input wire launch_i,
  output wire ready_o,
  input wire [12:0] id_i,
  input wire [31:0] instruction_i, pc_i, source1_i, source2_i,
  output wire valid_o,
  output logic [12:0] id_o,
  output commit_event_pkg::commit_event_t event_o,
  input wire take_i
);
  import single_lane_pkg::*;
  import commit_event_pkg::*;
  decoded_t decoded;
  wire [31:0] result, next_pc;
  wire execute_trap;
  logic pending_q;
  commit_event_t built;

  assign valid_o = pending_q && !rst_i && !flush_i;
  assign ready_o = !rst_i && !flush_i && (!pending_q || take_i);

  decode_single decode (.instruction_i, .decoded_o(decoded));
  execute_single execute (
    .decoded_i(decoded), .pc_i, .instruction_i, .source1_i, .source2_i,
    .csr_value_i(32'd0), .mepc_i(32'd0), .csr_legal_i(1'b0),
    .result_o(result), .next_pc_o(next_pc), .address_o(),
    .trap_o(execute_trap), .cause_o(), .trap_value_o()
  );

  always_comb begin
    built = '0;
    built.valid = 1;
    built.retired = 1;
    built.privilege = 3;
    built.instruction = instruction_i;
    built.pc_before = pc_i;
    built.pc_after = next_pc;
    built.rs1_addr = decoded.rs1;
    built.rs1_value = source1_i;
    built.rs2_addr = decoded.rs2;
    built.rs2_value = source2_i;
    if (decoded.rd != 0) begin
      built.rd_addr = decoded.rd;
      built.rd_value = result;
      built.rd_write_mask = '1;
    end
  end

  // A held result is replaced only by accepted completion plus a new launch.
  always_ff @(posedge clk_i) begin
    if (rst_i || flush_i) pending_q <= 0;
    else begin
      if (take_i) pending_q <= 0;
      if (launch_i) begin
        pending_q <= 1;
        id_o <= id_i;
        event_o <= built;
      end
    end
  end

`ifndef SYNTHESIS
  always_ff @(posedge clk_i) begin
    if (!rst_i && !flush_i) begin
      assert (!launch_i || ready_o) else $fatal(1, "ALU_PIPE_CAPACITY");
      assert (!take_i || valid_o) else $fatal(1, "ALU_PIPE_TAKE");
      assert (!launch_i || (!execute_trap && decoded.op inside {OP_ALU, OP_LUI, OP_AUIPC}))
        else $fatal(1, "ALU_PIPE_OPERATION");
    end
  end
`endif
endmodule
`default_nettype wire
