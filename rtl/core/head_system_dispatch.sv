`default_nettype none
module head_system_dispatch (
  input wire clk_i, rst_i, flush_i, drained_i,
  input wire [1:0] valid_i, system_i,
  input wire cfi0_i,
  input wire [31:0] instruction_i, pc_i,
  output wire [1:0] dispatch_valid_o, dispatch_solo_o,
  input wire [1:0] allocate_accept_i,
  input wire [12:0] allocate_id_i,
  input wire [5:0] source1_i,
  output wire [1:0] queue_dispatch_o,
  input wire head_valid_i,
  input wire [12:0] head_id_i,
  input wire [31:0] head_pc_i,
  input wire recover_i,
  input wire [4:0] recover_slot_i,
  input wire serial_accept_i,
  input wire [12:0] serial_id_i,
  output wire busy_o, system_valid_o, killed_o,
  output wire [12:0] system_id_o,
  output wire [31:0] system_instruction_o, system_pc_o,
  output wire [5:0] system_source_o
);
  logic valid_q;
  logic [12:0] id_q;
  logic [31:0] instruction_q, pc_q;
  logic [5:0] source_q;
  wire blocked = rst_i || flush_i || drained_i || recover_i;
  wire younger = 5'(id_q[4:0] - head_id_i[4:0]) > 5'(recover_slot_i - head_id_i[4:0]);
  wire kill = recover_i && younger;
  wire capture = allocate_accept_i[0] && system_i[0];
  wire release_owner = serial_accept_i && serial_id_i == id_q;

  // A lane-one system operation is replayed alone; a held descriptor blocks all younger dispatch.
  assign dispatch_valid_o[0] = valid_i[0] && !valid_q && !blocked;
  assign dispatch_valid_o[1] = dispatch_valid_o[0] && valid_i[1]
      && !cfi0_i && !system_i[0] && !system_i[1];
  assign dispatch_solo_o = dispatch_valid_o & system_i;
  assign queue_dispatch_o = allocate_accept_i & ~system_i & {2{!blocked && !valid_q}};
  assign busy_o = valid_q;
  assign killed_o = valid_q && (rst_i || flush_i || kill);
  assign system_valid_o = valid_q && !blocked && head_valid_i
      && head_id_i == id_q && head_pc_i == pc_q;
  assign system_id_o = valid_q ? id_q : 13'd0;
  assign system_instruction_o = valid_q ? instruction_q : 32'd0;
  assign system_pc_o = valid_q ? pc_q : 32'd0;
  assign system_source_o = valid_q ? source_q : 6'd0;

  always_ff @(posedge clk_i) begin
    if (rst_i || flush_i || kill) valid_q <= 0;
    else if (release_owner) valid_q <= 0;
    else if (capture) begin
      valid_q <= 1;
      id_q <= allocate_id_i[12:0];
      instruction_q <= instruction_i[31:0];
      pc_q <= pc_i[31:0];
      source_q <= source1_i[5:0];
    end
  end

`ifndef SYNTHESIS
  always_ff @(posedge clk_i) if (!rst_i) begin
    assert (valid_i != 2'b10) else $fatal(1, "SYSTEM_DISPATCH_PREFIX");
    assert ((allocate_accept_i & ~dispatch_valid_o) == 0 && allocate_accept_i != 2'b10)
      else $fatal(1, "SYSTEM_DISPATCH_ALLOCATION");
    assert (!capture || (!cfi0_i && allocate_accept_i == 1
        && (instruction_i[31:0] inside {32'h30200073, 32'h10500073} || (instruction_i[6:0] == 7'h73
        && instruction_i[14:12] inside {3'd1, 3'd2, 3'd3, 3'd5, 3'd6, 3'd7}))))
      else $fatal(1, "SYSTEM_DISPATCH_OPERATION");
    assert (!capture || (!(instruction_i[31:0] == 32'h30200073 || instruction_i[14]
        || instruction_i[19:15] == 0) || source1_i[5:0] == 0))
      else $fatal(1, "SYSTEM_DISPATCH_SOURCE");
    assert (!serial_accept_i || (system_valid_o && serial_id_i == id_q))
      else $fatal(1, "SYSTEM_DISPATCH_RETIRE");
    assert (!drained_i || !valid_q) else $fatal(1, "SYSTEM_DISPATCH_DRAIN");
    assert (!recover_i || head_valid_i) else $fatal(1, "SYSTEM_DISPATCH_RECOVERY");
  end
`endif
endmodule
`default_nettype wire
