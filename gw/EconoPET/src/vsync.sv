// Simple v-sync generator @60 Hz.
module vsync(
    input  logic cpu_clock_i,
    output logic v_sync_o
);
    logic [14:0] count = 0;

    initial begin
        v_sync_o = 1'b0;
    end

    always_ff @(posedge cpu_clock_i) begin
        if (count == 8333) begin
            v_sync_o <= ~v_sync_o;
        end else begin
            count <= count + 1'd1;
        end
    end
endmodule
