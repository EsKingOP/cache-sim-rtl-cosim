// tb_cache_controller.v - Verilog Testbench with Co-Simulation Trace Logging
`timescale 1ns/1ps

module tb_cache_controller;

    parameter NUM_SETS   = 64;
    parameter NUM_WAYS   = 4;
    parameter LINE_SIZE  = 64;
    parameter ADDR_WIDTH = 32;
    parameter DATA_WIDTH = 32;

    reg clk;
    reg rst_n;

    // CPU side
    reg                  cpu_req;
    reg                  cpu_we;
    reg [ADDR_WIDTH-1:0] cpu_addr;
    reg [DATA_WIDTH-1:0] cpu_wdata;
    reg [DATA_WIDTH/8-1:0] cpu_be;
    wire                 cpu_ack;
    wire [DATA_WIDTH-1:0] cpu_rdata;
    wire                 cpu_stall;

    // Memory side
    wire                 mem_req;
    wire                 mem_we;
    wire [ADDR_WIDTH-1:0] mem_addr;
    wire [DATA_WIDTH-1:0] mem_wdata;
    reg  [DATA_WIDTH-1:0] mem_rdata;
    reg                  mem_ack;

    // Status outputs
    wire                 hit_o;
    wire                 miss_o;
    wire                 evict_o;
    wire                 evict_dirty_o;
    wire [5:0]           hit_way_o;
    wire [5:0]           evict_way_o;

    // DUT instantiation
    cache_controller #(
        .NUM_SETS(NUM_SETS),
        .NUM_WAYS(NUM_WAYS),
        .LINE_SIZE(LINE_SIZE),
        .ADDR_WIDTH(ADDR_WIDTH),
        .DATA_WIDTH(DATA_WIDTH)
    ) dut (
        .clk(clk),
        .rst_n(rst_n),
        .cpu_req(cpu_req),
        .cpu_we(cpu_we),
        .cpu_addr(cpu_addr),
        .cpu_wdata(cpu_wdata),
        .cpu_be(cpu_be),
        .cpu_ack(cpu_ack),
        .cpu_rdata(cpu_rdata),
        .cpu_stall(cpu_stall),
        .mem_req(mem_req),
        .mem_we(mem_we),
        .mem_addr(mem_addr),
        .mem_wdata(mem_wdata),
        .mem_rdata(mem_rdata),
        .mem_ack(mem_ack),
        .hit_o(hit_o),
        .miss_o(miss_o),
        .evict_o(evict_o),
        .evict_dirty_o(evict_dirty_o),
        .hit_way_o(hit_way_o),
        .evict_way_o(evict_way_o)
    );

    // Clock generator: 100MHz (10ns period)
    always #5 clk = ~clk;

    // Memory simulation mock
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            mem_ack   <= 1'b0;
            mem_rdata <= 32'h0;
        end else begin
            if (mem_req && !mem_ack) begin
                mem_ack   <= 1'b1;
                mem_rdata <= 32'hA5A50000 | (mem_addr[15:0]);
            end else begin
                mem_ack   <= 1'b0;
            end
        end
    end

    // File descriptors
    integer trace_fd, log_fd;
    string trace_file = "sample_trace.trace";
    string log_file   = "rtl_sim.log";

    string line;
    reg [8*32-1:0] str_token;
    reg [63:0] access_idx;
    reg [ADDR_WIDTH-1:0] trace_addr;
    reg [DATA_WIDTH-1:0] trace_data;
    reg [7:0] trace_op;
    integer scan_status;

    initial begin
        clk = 0;
        rst_n = 0;
        cpu_req = 0;
        cpu_we = 0;
        cpu_addr = 0;
        cpu_wdata = 0;
        cpu_be = 4'b1111;
        access_idx = 0;

        if ($value$plusargs("TRACE=%s", trace_file)) begin
            $display("[TB] Loading trace file: %s", trace_file);
        end
        if ($value$plusargs("LOG=%s", log_file)) begin
            $display("[TB] Writing output log to: %s", log_file);
        end

        trace_fd = $fopen(trace_file, "r");
        if (trace_fd == 0) begin
            $display("[-] Error: Unable to open trace file %s", trace_file);
            $finish;
        end

        log_fd = $fopen(log_file, "w");
        if (log_fd == 0) begin
            $display("[-] Error: Unable to open output log file %s", log_file);
            $finish;
        end

        // Reset pulse
        #20;
        rst_n = 1;
        #10;

        // Trace processing loop
        while (!$feof(trace_fd)) begin
            scan_status = $fscanf(trace_fd, "%c %h %h\n", trace_op, trace_addr, trace_data);
            if (scan_status >= 2) begin
                @(posedge clk);
                while (cpu_stall) @(posedge clk);

                cpu_req   <= 1'b1;
                cpu_we    <= (trace_op == "W" || trace_op == "w") ? 1'b1 : 1'b0;
                cpu_addr  <= trace_addr;
                cpu_wdata <= (scan_status == 3) ? trace_data : 32'hBAADF00D;
                cpu_be    <= 4'b1111;

                @(posedge clk);
                cpu_req   <= 1'b0;

                // Wait for response/ack
                while (!cpu_ack) @(posedge clk);

                // Compute set index
                // OFFSET_BITS = 6, INDEX_BITS = 6
                // Log entry format:
                // <idx> <R|W> 0x<addr> <HIT|MISS|EVICT_DIRTY|EVICT_CLEAN> SET=<n> WAY_HIT=<n> WAY_EVICT=<n>
                if (hit_o) begin
                    $fdisplay(log_fd, "%0d %c 0x%08h HIT SET=%0d WAY_HIT=%0d WAY_EVICT=-1",
                              access_idx, trace_op, trace_addr, (trace_addr >> 6) & (NUM_SETS - 1), hit_way_o);
                end else if (evict_o && evict_dirty_o) begin
                    $fdisplay(log_fd, "%0d %c 0x%08h EVICT_DIRTY SET=%0d WAY_HIT=-1 WAY_EVICT=%0d",
                              access_idx, trace_op, trace_addr, (trace_addr >> 6) & (NUM_SETS - 1), evict_way_o);
                end else if (evict_o) begin
                    $fdisplay(log_fd, "%0d %c 0x%08h EVICT_CLEAN SET=%0d WAY_HIT=-1 WAY_EVICT=%0d",
                              access_idx, trace_op, trace_addr, (trace_addr >> 6) & (NUM_SETS - 1), evict_way_o);
                end else begin
                    $fdisplay(log_fd, "%0d %c 0x%08h MISS SET=%0d WAY_HIT=-1 WAY_EVICT=-1",
                              access_idx, trace_op, trace_addr, (trace_addr >> 6) & (NUM_SETS - 1));
                end

                access_idx = access_idx + 1;
            end
        end

        $fclose(trace_fd);
        $fclose(log_fd);
        $display("[+] Simulation finished. Processed %0d transactions.", access_idx);
        $finish;
    end

endmodule
