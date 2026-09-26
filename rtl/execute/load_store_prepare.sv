`default_nettype none
module load_store_prepare (
  input wire clk_i, rst_i, flush_i,
  input wire launch_i,
  output wire ready_o,
  input wire [12:0] id_i,
  input wire [31:0] instruction_i, pc_i, source1_i, source2_i,
  output wire valid_o,
  input wire take_i,
  output wire [12:0] id_o,
  output wire [31:0] instruction_o, pc_o, source1_o, source2_o,
  output wire [31:0] address_o, write_data_o, cause_o, trap_value_o,
  output wire [3:0] byte_mask_o,
  output wire [1:0] size_o,
  output wire store_o, load_unsigned_o, uncached_o, head_only_o, fault_o
);
  import single_lane_pkg::*;
  import platform_pkg::*;
  typedef struct packed {
    logic [12:0] id;
    logic [31:0] instruction, pc, source1, source2, address, write_data, cause, trap_value;
    logic [3:0] mask;
    logic [1:0] size;
    logic store, load_unsigned, uncached, head_only, fault;
  } descriptor_t;
  descriptor_t built, saved_q, visible;
  decoded_t decoded;
  logic pending_q;
  wire [31:0] base = decoded.rs1 == 0 ? 32'd0 : source1_i;
  wire [31:0] data = decoded.rs2 == 0 ? 32'd0 : source2_i;
  wire [31:0] address, cause, trap_value;
  wire fault;
  wire [63:0] unused_execute;
  wire idempotent = (in_region(address, BRAM_BASE, BRAM_SIZE) && BRAM_IDEMPOTENT)
    || (in_region(address, UART_BASE, UART_SIZE) && UART_IDEMPOTENT)
    || (in_region(address, GPIO_BASE, GPIO_SIZE) && GPIO_IDEMPOTENT);

  assign valid_o = pending_q && !rst_i && !flush_i;
  assign ready_o = !rst_i && !flush_i && (!pending_q || take_i);
  assign visible = valid_o ? saved_q : '0;
  assign {id_o, instruction_o, pc_o, source1_o, source2_o, address_o,
    write_data_o, cause_o, trap_value_o, byte_mask_o, size_o,
    store_o, load_unsigned_o, uncached_o, head_only_o, fault_o} = visible;

  decode_single decode (.instruction_i, .decoded_o(decoded));
  execute_single execute (
    .decoded_i(decoded), .pc_i, .instruction_i, .source1_i(base), .source2_i(data),
    .csr_value_i(32'd0), .mepc_i(32'd0), .csr_legal_i(1'b0),
    .result_o(unused_execute[31:0]), .next_pc_o(unused_execute[63:32]), .address_o(address),
    .trap_o(fault), .cause_o(cause), .trap_value_o(trap_value)
  );

  always_comb begin
    built = '0;
    built.id = id_i;
    built.instruction = instruction_i;
    built.pc = pc_i;
    built.source1 = base;
    built.source2 = data;
    built.address = address;
    built.store = decoded.op == OP_STORE;
    built.size = decoded.funct3[1:0];
    built.load_unsigned = decoded.op == OP_LOAD && decoded.funct3[2];
    built.fault = fault;
    built.cause = cause;
    built.trap_value = trap_value;
    built.head_only = fault || built.store || !cached(address) || !idempotent;
    if (!fault) begin
      built.uncached = !cached(address);
      case (built.size)
        0: built.mask = 4'b0001 << address[1:0];
        1: built.mask = 4'b0011 << address[1:0];
        default: built.mask = 4'b1111;
      endcase
      if (built.store) built.write_data = (data << {address[1:0], 3'b000}) & byte_mask(built.mask);
    end
  end

  // This descriptor authorizes no memory side effect; its consumer owns ordering and bus admission.
  always_ff @(posedge clk_i) begin
    if (rst_i || flush_i) pending_q <= 0;
    else begin
      if (take_i) pending_q <= 0;
      if (launch_i) begin
        pending_q <= 1;
        saved_q <= built;
      end
    end
  end
`ifndef SYNTHESIS
  always_ff @(posedge clk_i) if (!rst_i && !flush_i) begin
    assert (!launch_i || ready_o) else $fatal(1, "LS_PREP_CAPACITY");
    assert (!take_i || valid_o) else $fatal(1, "LS_PREP_TAKE");
    assert (!launch_i || decoded.op inside {OP_LOAD, OP_STORE}) else $fatal(1, "LS_PREP_OPERATION");
    assert (!launch_i || pc_i[1:0] == 0) else $fatal(1, "LS_PREP_PC");
  end
`endif
endmodule
`default_nettype wire
