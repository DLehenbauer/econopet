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
    parameter integer unsigned ADDR_WIDTH = 17,
    parameter integer unsigned DATA_WIDTH = 8
) (
    input  logic [ADDR_WIDTH-1:0] ram_addr_i,
    input  logic [DATA_WIDTH-1:0] ram_data_i,
    output logic [DATA_WIDTH-1:0] ram_data_o,
    input  logic                  ram_we_n_i,
    input  logic                  ram_oe_n_i
);
    logic [DATA_WIDTH - 1:0] mem[(2 ** ADDR_WIDTH) - 1];

    task automatic load_rom(
        input bit [ADDR_WIDTH-1:0] address,
        input string filename
    );
        int file, status;

        file = $fopen({ "../../build/roms/bin/roms/", filename }, "rb");
        if (file == 0) begin
            $fatal(1, "Error opening file '%s'.  Did you forget to build '/roms'?", filename);
        end

        status = $fread(mem, file, 32'(address));
        if (status < 1) begin
            $fatal(1, "Error reading file '%s' (status=%0d).", filename, status);
        end

        $display("[%t] Loaded ROM '%s' ($%x-%x, %0d bytes)", $time, filename, address, address + ADDR_WIDTH'(status - 1), status);
        $fclose(file);
    endtask

    always @(negedge ram_we_n_i or negedge ram_oe_n_i) begin
        if (!ram_we_n_i) begin
            mem[ram_addr_i] <= ram_data_i;
            $display("[%t]        RAM[%h] <- %h", $time, ram_addr_i, ram_data_i);
        end else if (!ram_oe_n_i) begin
            ram_data_o <= mem[ram_addr_i];
            $display("[%t]        RAM[%h] -> %h", $time, ram_addr_i, mem[ram_addr_i]);
        end
    end
endmodule
