`default_nettype none
module fetch_two_wide (
  input wire clk_i, rst_i, enable_i,
  input wire redirect_i,
  input wire [31:0] redirect_pc_i,
  output wire request_valid_o,
  input wire request_ready_i,
  output memory_protocol_pkg::mem_request_t request_o,
  input wire response_valid_i,
  output wire response_ready_o,
  input memory_protocol_pkg::mem_response_t response_i,
  output logic [1:0] valid_o,
  input wire [1:0] take_i,
  output logic [63:0] instruction_o, pc_o,
  output wire fault_o,
  output wire [31:0] fault_cause_o,
  output wire busy_o,
  output logic fatal_o
);
  import memory_protocol_pkg::*;
  import platform_pkg::*;
  import single_lane_pkg::executable;
  typedef enum logic [2:0] {EMPTY, REQUEST, RESPONSE, BUFFER, FAULT, STOP} state_e;
  state_e state_q;
  logic [31:0] pc_q;
  logic [26:0] request_line_q;
  logic [255:0] line_q;
  logic [3:0] id_q;
  logic discard_q, misaligned_q;
  wire active = !rst_i && !fatal_o;
  wire [31:0] advance = take_i[1] ? 32'd8 : 32'd4;
  wire malformed = response_valid_i && (state_q != RESPONSE
    || response_i.transaction_id != id_q
    || !(response_i.status inside {MEM_STATUS_OK, MEM_STATUS_ACCESS_FAULT})
    || response_i.uncached_read_data != 0
    || (response_i.status != MEM_STATUS_OK && response_i.line_read_data != 0));

  // A redirect cannot withdraw an offered request or release an owned ID.
  assign request_valid_o = active && state_q == REQUEST;
  assign response_ready_o = active && state_q == RESPONSE;
  assign busy_o = active && (state_q == REQUEST || state_q == RESPONSE);
  assign fault_o = valid_o[0] && state_q == FAULT;
  assign fault_cause_o = misaligned_q ? CAUSE_INSTRUCTION_ADDRESS_MISALIGNED
                                    : CAUSE_INSTRUCTION_ACCESS_FAULT;
  always_comb begin
    request_o = '0;
    request_o.transaction_id = id_q;
    request_o.address = {request_line_q, 5'd0};
    valid_o = '0;
    pc_o = {pc_q + 32'd4, pc_q};
    instruction_o = '0;
    if (active && !redirect_i) begin
      if (state_q == BUFFER) begin
        valid_o = pc_q[4:2] == 3'd7 ? 2'b01 : 2'b11;
        instruction_o[31:0] = line_q[pc_q[4:2]*32 +: 32];
        if (valid_o[1]) instruction_o[63:32] = line_q[(int'(pc_q[4:2])+1)*32 +: 32];
      end else if (state_q == FAULT) valid_o = 2'b01;
    end
  end
  always_ff @(posedge clk_i) begin
    if (rst_i) begin
      state_q <= EMPTY;
      pc_q <= RESET_PC;
      id_q <= 0;
      discard_q <= 0;
      misaligned_q <= 0;
      fatal_o <= 0;
    end else if (!fatal_o) begin
      if (malformed) fatal_o <= 1;
      case (state_q)
        EMPTY: if (enable_i && !redirect_i) begin
          misaligned_q <= pc_q[1:0] != 0;
          if (pc_q[1:0] != 0 || !executable(pc_q)) state_q <= FAULT;
          else begin
            request_line_q <= pc_q[31:5];
            discard_q <= 0;
            state_q <= REQUEST;
          end
        end
        REQUEST: if (request_ready_i) state_q <= RESPONSE;
        RESPONSE: if (response_valid_i && !malformed) begin
          id_q <= id_q + 1'b1;
          discard_q <= 0;
          if (discard_q || redirect_i) state_q <= EMPTY;
          else if (response_i.status == MEM_STATUS_ACCESS_FAULT) begin
            misaligned_q <= 0;
            state_q <= FAULT;
          end else begin
            line_q <= response_i.line_read_data;
            state_q <= BUFFER;
          end
        end
        BUFFER: if (take_i[0] && !redirect_i) begin
          pc_q <= pc_q + advance;
          if ({1'b0, pc_q[4:2]} + (take_i[1] ? 4'd2 : 4'd1) >= 4'd8)
            state_q <= EMPTY;
        end
        FAULT: if (take_i[0] && !redirect_i) state_q <= STOP;
        default: ;
      endcase
      if (redirect_i) begin
        pc_q <= redirect_pc_i;
        if (state_q == REQUEST || state_q == RESPONSE) begin
          if (!(state_q == RESPONSE && response_valid_i && !malformed)) discard_q <= 1;
        end else state_q <= EMPTY;
      end
    end
  end
`ifndef SYNTHESIS
  always_ff @(posedge clk_i) begin
    if (!rst_i) begin
      assert (!(take_i[1] && !take_i[0]) && (take_i & ~valid_o) == 0)
        else $fatal(1, "FETCH_PREFIX");
    end
  end
`endif
endmodule
`default_nettype wire
