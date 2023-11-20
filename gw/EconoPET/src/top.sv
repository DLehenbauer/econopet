/**
 * PET Clone - Open hardware implementation of the Commodore PET
 * by Daniel Lehenbauer and contributors.
 * 
 * https://github.com/DLehenbauer/commodore-pet-clone
 *
 * To the extent possible under law, I, Daniel Lehenbauer, have waived all
 * copyright and related or neighboring rights to this project. This work is
 * published from the United States.
 *
 * @copyright CC0 http://creativecommons.org/publicdomain/zero/1.0/
 * @author Daniel Lehenbauer <DLehenbauer@users.noreply.github.com> and contributors
 */
 
 module top(
    // FPGA
    input  logic         clk_i,             // 64 MHz clock (from PLL)
    output logic         status_no,         // NSTATUS LED (0 = On, 1 = Off)
    
    // Config
    input  logic         config_crt_i,      // (0 = 12", 1 = 9")
    input  logic         config_kbd_i,      // (0 = Business, 1 = Graphics)
    
    // Spare pins
    output logic  [9:0]  spare_o
);
    initial begin
        spare_o = '0;
    end

    always_ff @(posedge clk_i) begin
        spare_o <= spare_o + 1'b1;
    end
endmodule
