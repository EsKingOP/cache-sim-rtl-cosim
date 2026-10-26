// ============================================================================
// lru_unit.v
// Pseudo-LRU Tree Tracking Unit
//
// Implements a binary tree of "direction bits" (one per internal node) that
// always points toward the LRU leaf. For N ways, we need N-1 internal nodes.
//
// Supports up to 16 ways (15 internal nodes).
//
// Parameters:
//   NUM_SETS  - Number of cache sets
//   NUM_WAYS  - Associativity (must be power of 2; 1, 2, 4, 8, or 16)
//
// Ports:
//   update     - Pulse high for 1 cycle when a way is accessed/filled
//   set_index  - Which set to update / query
//   used_way   - Way that was just accessed (valid when update=1)
//   victim_way - LRU victim (combinational, always valid)
// ============================================================================

`timescale 1ns/1ps

module lru_unit #(
    parameter NUM_SETS = 64,
    parameter NUM_WAYS = 4
)(
    input  wire                        clk,
    input  wire                        rst_n,

    input  wire                        update,
    input  wire [$clog2(NUM_SETS)-1:0] set_index,
    input  wire [$clog2(NUM_WAYS)-1:0] used_way,

    output wire [$clog2(NUM_WAYS)-1:0] victim_way
);

// ── Number of internal nodes in pseudo-LRU tree ───────────────────────────────
// For N ways: N-1 internal nodes (binary tree)
localparam NUM_NODES = NUM_WAYS - 1;

// ── Tree storage: one bit per internal node per set ───────────────────────────
// Bit = 0: LRU pointer points left; 1: points right
reg [NUM_NODES-1:0] tree [0:NUM_SETS-1];

// ─────────────────────────────────────────────────────────────────────────────
//  Victim way — combinational traversal from root to leaf
//  Root = node 0; left child of node i = 2i+1; right child = 2i+2
// ─────────────────────────────────────────────────────────────────────────────

reg [$clog2(NUM_WAYS)-1:0] victim_reg;

always @(*) begin : VICTIM_CALC
    integer node;
    integer way_out;
    way_out = 0;
    node    = 0;

    // Traverse NUM_WAYS-1 levels (log2(NUM_WAYS) levels)
    // Level 0: root (node 0), Level 1: nodes 1-2, Level 2: nodes 3-6 ...
    // At each node: if bit=0 go left (lower ways), if bit=1 go right

    if (NUM_WAYS == 2) begin
        // 1 level: root bit → victim is way 0 or 1
        way_out = tree[set_index][0] ? 1 : 0;
    end else if (NUM_WAYS == 4) begin
        // 2 levels: root → left/right subtree → leaf
        if (tree[set_index][0] == 0) begin
            // Go left: choose between way 0 and way 1 (node 1)
            way_out = tree[set_index][1] ? 1 : 0;
        end else begin
            // Go right: choose between way 2 and way 3 (node 2)
            way_out = tree[set_index][2] ? 3 : 2;
        end
    end else if (NUM_WAYS == 8) begin
        // 3 levels
        if (tree[set_index][0] == 0) begin
            if (tree[set_index][1] == 0) begin
                way_out = tree[set_index][3] ? 1 : 0;
            end else begin
                way_out = tree[set_index][4] ? 3 : 2;
            end
        end else begin
            if (tree[set_index][2] == 0) begin
                way_out = tree[set_index][5] ? 5 : 4;
            end else begin
                way_out = tree[set_index][6] ? 7 : 6;
            end
        end
    end else if (NUM_WAYS == 16) begin
        // 4 levels — fully unrolled for synthesis
        if (tree[set_index][0] == 0) begin
            if (tree[set_index][1] == 0) begin
                if (tree[set_index][3] == 0) begin
                    way_out = tree[set_index][7]  ? 1  : 0;
                end else begin
                    way_out = tree[set_index][8]  ? 3  : 2;
                end
            end else begin
                if (tree[set_index][4] == 0) begin
                    way_out = tree[set_index][9]  ? 5  : 4;
                end else begin
                    way_out = tree[set_index][10] ? 7  : 6;
                end
            end
        end else begin
            if (tree[set_index][2] == 0) begin
                if (tree[set_index][5] == 0) begin
                    way_out = tree[set_index][11] ? 9  : 8;
                end else begin
                    way_out = tree[set_index][12] ? 11 : 10;
                end
            end else begin
                if (tree[set_index][6] == 0) begin
                    way_out = tree[set_index][13] ? 13 : 12;
                end else begin
                    way_out = tree[set_index][14] ? 15 : 14;
                end
            end
        end
    end else begin
        // Direct-mapped (NUM_WAYS==1): always evict way 0
        way_out = 0;
    end

    victim_reg = way_out[$clog2(NUM_WAYS)-1:0];
end

assign victim_way = victim_reg;

// ─────────────────────────────────────────────────────────────────────────────
//  Update — sequential: flip bits along the path from used_way to root
//  after an access (so the MRU path points away from the recently-used way)
// ─────────────────────────────────────────────────────────────────────────────

integer i;

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        for (i = 0; i < NUM_SETS; i = i + 1)
            tree[i] <= {NUM_NODES{1'b0}};
    end else if (update) begin
        // Update pseudo-LRU bits along the path from leaf to root
        // For each tree level, the direction bit at the parent is set
        // to point *away* from the just-used way.

        if (NUM_WAYS == 2) begin
            // Level 1: root points away from used_way
            tree[set_index][0] <= used_way[0] ? 1'b0 : 1'b1;
        end

        else if (NUM_WAYS == 4) begin
            // Level 1 (root): bit[0] → 0 if used_way < 2, 1 if used_way >= 2
            tree[set_index][0] <= (used_way[1]) ? 1'b0 : 1'b1;
            // Level 2: update the relevant subtree node
            if (used_way[1] == 0) begin
                // Left subtree (node 1): bit points away from used_way[0]
                tree[set_index][1] <= used_way[0] ? 1'b0 : 1'b1;
            end else begin
                // Right subtree (node 2)
                tree[set_index][2] <= used_way[0] ? 1'b0 : 1'b1;
            end
        end

        else if (NUM_WAYS == 8) begin
            tree[set_index][0] <= used_way[2] ? 1'b0 : 1'b1;
            if (used_way[2] == 0) begin
                tree[set_index][1] <= used_way[1] ? 1'b0 : 1'b1;
                if (used_way[1] == 0)
                    tree[set_index][3] <= used_way[0] ? 1'b0 : 1'b1;
                else
                    tree[set_index][4] <= used_way[0] ? 1'b0 : 1'b1;
            end else begin
                tree[set_index][2] <= used_way[1] ? 1'b0 : 1'b1;
                if (used_way[1] == 0)
                    tree[set_index][5] <= used_way[0] ? 1'b0 : 1'b1;
                else
                    tree[set_index][6] <= used_way[0] ? 1'b0 : 1'b1;
            end
        end

        else if (NUM_WAYS == 16) begin
            tree[set_index][0] <= used_way[3] ? 1'b0 : 1'b1;
            if (used_way[3] == 0) begin
                tree[set_index][1] <= used_way[2] ? 1'b0 : 1'b1;
                if (used_way[2] == 0) begin
                    tree[set_index][3] <= used_way[1] ? 1'b0 : 1'b1;
                    if (used_way[1] == 0)
                        tree[set_index][7]  <= used_way[0] ? 1'b0 : 1'b1;
                    else
                        tree[set_index][8]  <= used_way[0] ? 1'b0 : 1'b1;
                end else begin
                    tree[set_index][4] <= used_way[1] ? 1'b0 : 1'b1;
                    if (used_way[1] == 0)
                        tree[set_index][9]  <= used_way[0] ? 1'b0 : 1'b1;
                    else
                        tree[set_index][10] <= used_way[0] ? 1'b0 : 1'b1;
                end
            end else begin
                tree[set_index][2] <= used_way[2] ? 1'b0 : 1'b1;
                if (used_way[2] == 0) begin
                    tree[set_index][5] <= used_way[1] ? 1'b0 : 1'b1;
                    if (used_way[1] == 0)
                        tree[set_index][11] <= used_way[0] ? 1'b0 : 1'b1;
                    else
                        tree[set_index][12] <= used_way[0] ? 1'b0 : 1'b1;
                end else begin
                    tree[set_index][6] <= used_way[1] ? 1'b0 : 1'b1;
                    if (used_way[1] == 0)
                        tree[set_index][13] <= used_way[0] ? 1'b0 : 1'b1;
                    else
                        tree[set_index][14] <= used_way[0] ? 1'b0 : 1'b1;
                end
            end
        end
    end
end

endmodule
