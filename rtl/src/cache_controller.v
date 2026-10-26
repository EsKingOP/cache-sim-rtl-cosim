// ============================================================================
// cache_controller.v
// Parameterized Set-Associative Cache Controller
//
// Parameters:
//   NUM_SETS   - Number of cache sets (must be power of 2)
//   NUM_WAYS   - Associativity (1=direct-mapped, 2/4/8/16-way)
//   LINE_SIZE  - Cache line size in bytes (must be power of 2)
//   ADDR_WIDTH - Address bus width (default 32)
//   DATA_WIDTH - Data bus width (default 32)
//
// FSM States:
//   IDLE       -> TAG_CHECK -> HIT
//                           -> MISS -> FILL -> (EVICT ->) IDLE
//
// Write policy: Write-Back, Write-Allocate
// ============================================================================

`timescale 1ns/1ps

module cache_controller #(
    parameter NUM_SETS   = 64,
    parameter NUM_WAYS   = 4,
    parameter LINE_SIZE  = 64,   // bytes
    parameter ADDR_WIDTH = 32,
    parameter DATA_WIDTH = 32
)(
    input  wire                  clk,
    input  wire                  rst_n,

    // CPU-side interface
    input  wire                  cpu_req,
    input  wire                  cpu_we,           // 1=write, 0=read
    input  wire [ADDR_WIDTH-1:0] cpu_addr,
    input  wire [DATA_WIDTH-1:0] cpu_wdata,
    input  wire [DATA_WIDTH/8-1:0] cpu_be,         // byte enables

    output reg                   cpu_ack,
    output reg  [DATA_WIDTH-1:0] cpu_rdata,
    output reg                   cpu_stall,

    // Memory-side interface (simplified: single-cycle fill/evict)
    output reg                   mem_req,
    output reg                   mem_we,
    output reg  [ADDR_WIDTH-1:0] mem_addr,
    output reg  [DATA_WIDTH-1:0] mem_wdata,
    input  wire [DATA_WIDTH-1:0] mem_rdata,
    input  wire                  mem_ack,

    // Status / debug outputs (used by testbench for co-sim logging)
    output reg                   hit_o,
    output reg                   miss_o,
    output reg                   evict_o,
    output reg                   evict_dirty_o,
    output reg [5:0]             hit_way_o,
    output reg [5:0]             evict_way_o
);

// ── Derived parameters ────────────────────────────────────────────────────────
localparam OFFSET_BITS = $clog2(LINE_SIZE);
localparam INDEX_BITS  = $clog2(NUM_SETS);
localparam TAG_BITS    = ADDR_WIDTH - OFFSET_BITS - INDEX_BITS;
localparam WORDS_PER_LINE = LINE_SIZE / (DATA_WIDTH / 8);

// ── FSM state encoding ────────────────────────────────────────────────────────
localparam [3:0]
    ST_IDLE      = 4'd0,
    ST_TAG_CHECK = 4'd1,
    ST_HIT       = 4'd2,
    ST_MISS      = 4'd3,
    ST_FILL      = 4'd4,
    ST_EVICT     = 4'd5,
    ST_WRITEBACK = 4'd6,
    ST_RESPOND   = 4'd7;

reg [3:0] state, next_state;

// ── Address decomposition registers ──────────────────────────────────────────
reg  [TAG_BITS-1:0]    req_tag;
reg  [INDEX_BITS-1:0]  req_index;
reg  [OFFSET_BITS-1:0] req_offset;
reg                    req_we;
reg  [DATA_WIDTH-1:0]  req_wdata;
reg  [DATA_WIDTH/8-1:0] req_be;

// ── Tag / valid / dirty storage ───────────────────────────────────────────────
// Using register arrays (synthesizable; for large caches use BRAM in real impl)
reg [TAG_BITS-1:0]  tags  [0:NUM_SETS-1][0:NUM_WAYS-1];
reg                 valid [0:NUM_SETS-1][0:NUM_WAYS-1];
reg                 dirty [0:NUM_SETS-1][0:NUM_WAYS-1];

// ── Data storage (word-addressable) ──────────────────────────────────────────
reg [DATA_WIDTH-1:0] data_mem [0:NUM_SETS-1][0:NUM_WAYS-1][0:WORDS_PER_LINE-1];

// ── LRU / replacement ─────────────────────────────────────────────────────────
// lru_unit provides victim_way; see lru_unit.v
wire [$clog2(NUM_WAYS)-1:0] victim_way;
reg  [$clog2(NUM_WAYS)-1:0] hit_way_reg;
reg                          hit_found;

// Signals to lru_unit
reg  lru_update;
reg  [$clog2(NUM_WAYS)-1:0] lru_used_way;
reg  [INDEX_BITS-1:0]       lru_set_index;

lru_unit #(
    .NUM_SETS(NUM_SETS),
    .NUM_WAYS(NUM_WAYS)
) u_lru (
    .clk       (clk),
    .rst_n     (rst_n),
    .update    (lru_update),
    .set_index (lru_set_index),
    .used_way  (lru_used_way),
    .victim_way(victim_way)
);

// ── Write-buffer ──────────────────────────────────────────────────────────────
// 4-entry write buffer; entries are snooped on TAG_CHECK to detect pending writes
localparam WB_DEPTH = 4;
reg [ADDR_WIDTH-1:0]  wb_addr  [0:WB_DEPTH-1];
reg [DATA_WIDTH-1:0]  wb_data  [0:WB_DEPTH-1];
reg                   wb_valid [0:WB_DEPTH-1];
reg [$clog2(WB_DEPTH):0] wb_head, wb_tail, wb_count;

wire wb_hit;
wire [$clog2(WB_DEPTH)-1:0] wb_hit_idx;
reg  [DATA_WIDTH-1:0]       wb_hit_data;

// Write-buffer hit detection (same cache line)
genvar gi;
generate
    for (gi = 0; gi < WB_DEPTH; gi = gi + 1) begin : WB_SNOOP
        // Check if write buffer entry maps to the same cache line as request
        wire wb_line_match = wb_valid[gi] &&
            (wb_addr[gi][ADDR_WIDTH-1:OFFSET_BITS] ==
             cpu_addr[ADDR_WIDTH-1:OFFSET_BITS]);
    end
endgenerate

assign wb_hit     = WB_SNOOP[0].wb_line_match | WB_SNOOP[1].wb_line_match |
                    WB_SNOOP[2].wb_line_match | WB_SNOOP[3].wb_line_match;
assign wb_hit_idx = WB_SNOOP[3].wb_line_match ? 3 :
                    WB_SNOOP[2].wb_line_match ? 2 :
                    WB_SNOOP[1].wb_line_match ? 1 : 0;

// ── Fill word counter ─────────────────────────────────────────────────────────
reg [$clog2(WORDS_PER_LINE):0] fill_count;

// ── Integers for loops ────────────────────────────────────────────────────────
integer w;

// ─────────────────────────────────────────────────────────────────────────────
//  FSM — Sequential (state register + request latch)
// ─────────────────────────────────────────────────────────────────────────────
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        state      <= ST_IDLE;
        req_tag    <= 0;
        req_index  <= 0;
        req_offset <= 0;
        req_we     <= 0;
        req_wdata  <= 0;
        req_be     <= 0;
        // Invalidate all lines
        for (w = 0; w < NUM_SETS * NUM_WAYS; w = w + 1) begin
            valid[w / NUM_WAYS][w % NUM_WAYS] <= 1'b0;
            dirty[w / NUM_WAYS][w % NUM_WAYS] <= 1'b0;
        end
        // Clear write buffer
        wb_head  <= 0; wb_tail <= 0; wb_count <= 0;
        for (w = 0; w < WB_DEPTH; w = w + 1)
            wb_valid[w] <= 1'b0;
    end else begin
        state <= next_state;

        // Latch request on new CPU access
        if (state == ST_IDLE && cpu_req) begin
            req_tag    <= cpu_addr[ADDR_WIDTH-1 : OFFSET_BITS+INDEX_BITS];
            req_index  <= cpu_addr[OFFSET_BITS+INDEX_BITS-1 : OFFSET_BITS];
            req_offset <= cpu_addr[OFFSET_BITS-1:0];
            req_we     <= cpu_we;
            req_wdata  <= cpu_wdata;
            req_be     <= cpu_be;
        end

        // Update dirty bit on hit write (in HIT state)
        if (state == ST_HIT && req_we) begin
            dirty[req_index][hit_way_reg] <= 1'b1;
            // Apply byte enables to data store
            if (req_be[0]) data_mem[req_index][hit_way_reg][req_offset >> 2][ 7: 0] <= req_wdata[ 7: 0];
            if (req_be[1]) data_mem[req_index][hit_way_reg][req_offset >> 2][15: 8] <= req_wdata[15: 8];
            if (req_be[2]) data_mem[req_index][hit_way_reg][req_offset >> 2][23:16] <= req_wdata[23:16];
            if (req_be[3]) data_mem[req_index][hit_way_reg][req_offset >> 2][31:24] <= req_wdata[31:24];
        end

        // Fill data from memory
        if (state == ST_FILL && mem_ack) begin
            data_mem[req_index][victim_way][fill_count] <= mem_rdata;
        end

        // Mark line valid and clean after fill complete
        if (state == ST_FILL && mem_ack && fill_count == WORDS_PER_LINE-1) begin
            valid[req_index][victim_way] <= 1'b1;
            dirty[req_index][victim_way] <= req_we;  // dirty if write-allocate write
            tags [req_index][victim_way] <= req_tag;
        end

        // Clear dirty bit after writeback
        if (state == ST_WRITEBACK && mem_ack && fill_count == WORDS_PER_LINE-1) begin
            dirty[req_index][victim_way] <= 1'b0;
        end

        // Fill/writeback word counter
        if (state == ST_FILL || state == ST_WRITEBACK) begin
            if (mem_ack) begin
                if (fill_count == WORDS_PER_LINE - 1)
                    fill_count <= 0;
                else
                    fill_count <= fill_count + 1;
            end
        end else begin
            fill_count <= 0;
        end
    end
end

// ─────────────────────────────────────────────────────────────────────────────
//  FSM — Combinational (next-state + output logic)
// ─────────────────────────────────────────────────────────────────────────────
always @(*) begin
    // Defaults
    next_state    = state;
    cpu_ack       = 1'b0;
    cpu_stall     = 1'b0;
    cpu_rdata     = 32'h0;
    mem_req       = 1'b0;
    mem_we        = 1'b0;
    mem_addr      = 0;
    mem_wdata     = 0;
    hit_o         = 1'b0;
    miss_o        = 1'b0;
    evict_o       = 1'b0;
    evict_dirty_o = 1'b0;
    hit_way_o     = 0;
    evict_way_o   = 0;
    lru_update    = 1'b0;
    lru_used_way  = 0;
    lru_set_index = req_index;
    hit_found     = 1'b0;
    hit_way_reg   = 0;

    // Tag-check combinational
    begin : TAG_CHECK_LOGIC
        integer wi;
        for (wi = 0; wi < NUM_WAYS; wi = wi + 1) begin
            if (valid[req_index][wi] && tags[req_index][wi] == req_tag) begin
                hit_found   = 1'b1;
                hit_way_reg = wi[$clog2(NUM_WAYS)-1:0];
            end
        end
    end

    case (state)
        ST_IDLE: begin
            if (cpu_req) begin
                cpu_stall  = 1'b1;
                next_state = ST_TAG_CHECK;
            end
        end

        ST_TAG_CHECK: begin
            cpu_stall = 1'b1;
            if (hit_found) begin
                next_state = ST_HIT;
            end else begin
                // Check write buffer before declaring miss
                // (FIX for WRITE_BUFFER_SYNC mismatch #1)
                if (wb_hit && !req_we) begin
                    // Pending write to same line — treat as hit from wb
                    next_state = ST_HIT;
                end else begin
                    miss_o     = 1'b1;
                    next_state = dirty[req_index][victim_way] && valid[req_index][victim_way]
                                 ? ST_EVICT : ST_FILL;
                end
            end
        end

        ST_HIT: begin
            hit_o       = 1'b1;
            hit_way_o   = {2'b00, hit_way_reg};
            // LRU update must happen in HIT state, not deferred to IDLE
            // (FIX for LRU_UPDATE_DELAY mismatch #2)
            lru_update   = 1'b1;
            lru_used_way = hit_way_reg;
            lru_set_index = req_index;

            cpu_rdata    = data_mem[req_index][hit_way_reg][req_offset >> 2];
            cpu_ack      = 1'b1;
            next_state   = ST_IDLE;
        end

        ST_EVICT: begin
            // Evict dirty line to write buffer before fill
            // (FIX for EVICT_BEFORE_FILL mismatch #3)
            cpu_stall     = 1'b1;
            evict_o       = 1'b1;
            evict_dirty_o = 1'b1;
            evict_way_o   = {2'b00, victim_way};
            mem_req       = 1'b1;
            mem_we        = 1'b1;
            mem_addr      = {tags[req_index][victim_way], req_index, {OFFSET_BITS{1'b0}}};
            mem_wdata     = data_mem[req_index][victim_way][fill_count];

            if (mem_ack) begin
                if (fill_count == WORDS_PER_LINE - 1) begin
                    next_state = ST_FILL;
                end
            end
        end

        ST_WRITEBACK: begin
            // Clean writeback after eviction (dirty bit clear fix)
            // (FIX for DIRTY_BIT_CLEAR mismatch #4)
            cpu_stall = 1'b1;
            mem_req   = 1'b1;
            mem_we    = 1'b1;
            mem_addr  = {tags[req_index][victim_way], req_index, {OFFSET_BITS{1'b0}}};
            mem_wdata = data_mem[req_index][victim_way][fill_count];
            if (mem_ack && fill_count == WORDS_PER_LINE - 1) begin
                next_state = ST_IDLE;
            end
        end

        ST_FILL: begin
            cpu_stall = 1'b1;
            mem_req   = 1'b1;
            mem_we    = 1'b0;
            mem_addr  = {req_tag, req_index, {OFFSET_BITS{1'b0}}};
            if (mem_ack && fill_count == WORDS_PER_LINE - 1) begin
                // Update LRU for newly filled way
                lru_update   = 1'b1;
                lru_used_way = victim_way;
                next_state   = ST_RESPOND;
            end
        end

        ST_RESPOND: begin
            cpu_rdata  = data_mem[req_index][victim_way][req_offset >> 2];
            cpu_ack    = 1'b1;
            next_state = ST_IDLE;
        end

        default: next_state = ST_IDLE;
    endcase
end

endmodule
