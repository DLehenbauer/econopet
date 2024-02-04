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

 module mock_ram #(
    parameter DATA_WIDTH = 8,
    parameter ADDR_WIDTH = 17
)(
    input  logic [ADDR_WIDTH-1:0]  ram_addr_i,
    input  logic [DATA_WIDTH-1:0]  ram_data_i,
    output logic [DATA_WIDTH-1:0]  ram_data_o,
    input  logic                   ram_we_n_i,
    input  logic                   ram_oe_n_i
);
    logic [DATA_WIDTH - 1:0] mem[(2 ** ADDR_WIDTH) - 1:0];

    always_ff @(negedge ram_we_n_i or negedge ram_oe_n_i) begin
        if (!ram_we_n_i) begin
            mem[ram_addr_i] <= ram_data_i;
        end else if (!ram_oe_n_i) begin
            ram_data_o <= mem[ram_addr_i];
        end
    end
endmodule
