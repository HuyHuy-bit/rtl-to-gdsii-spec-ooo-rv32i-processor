`default_nettype none
module frontend_fault_decode (
  input wire [1:0] valid_i,
  input wire [63:0] instruction_i, pc_i,
  input wire fetch_fault_i,
  input wire [31:0] fetch_cause_i,
  output wire [1:0] fault_o,
  output wire [63:0] cause_o, value_o
);
  import single_lane_pkg::*;
  import platform_pkg::*;
  decoded_t [1:0] decoded;
  for (genvar lane = 0; lane < 2; lane++) begin : lanes
    decode_single decode (.instruction_i(instruction_i[lane*32 +: 32]), .decoded_o(decoded[lane]));
    assign fault_o[lane] = valid_i[lane] && (fetch_fault_i ||
      decoded[lane].op inside {OP_ILLEGAL, OP_ECALL, OP_EBREAK});
    assign cause_o[lane*32 +: 32] = fetch_fault_i ? fetch_cause_i :
      decoded[lane].op == OP_ECALL ? CAUSE_ENVIRONMENT_CALL_FROM_M_MODE :
      decoded[lane].op == OP_EBREAK ? CAUSE_BREAKPOINT : CAUSE_ILLEGAL_INSTRUCTION;
    assign value_o[lane*32 +: 32] = fetch_fault_i ? pc_i[lane*32 +: 32] :
      decoded[lane].op == OP_ECALL ? 32'd0 :
      decoded[lane].op == OP_EBREAK ? pc_i[lane*32 +: 32] : instruction_i[lane*32 +: 32];
  end
endmodule
`default_nettype wire
