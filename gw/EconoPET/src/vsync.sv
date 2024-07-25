// Simple v-sync generator @60 Hz.
module vsync(
    input  logic cpu_clock_i,
    output logic v_sync_o
);
    logic [14:0] count = 0;

    assign v_sync_o = count[14];

    always_ff @(posedge cpu_clock_i) begin
        if (count == 16667) begin
            count <= 0;
        end else begin
            count <= count + 1'd1;
        end
    end
endmodule
