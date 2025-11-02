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

import common_pkg::*;

module mock_ram (
    input  logic                      clock_i,
    input  logic [RAM_ADDR_WIDTH-1:0] ram_addr_i,
    input  logic     [DATA_WIDTH-1:0] ram_data_i,
    output logic     [DATA_WIDTH-1:0] ram_data_o,
    input  logic                      ram_we_n_i,
    input  logic                      ram_oe_n_i
);
    logic [DATA_WIDTH - 1:0] mem[(2 ** RAM_ADDR_WIDTH)];

    task automatic load_rom(
        input bit [RAM_ADDR_WIDTH-1:0] address,
        input string filename
    );
        int file, status;

    `ifndef ECONOPET_ROMS_DIR
        $fatal(1, "ECONOPET_ROMS_DIR not defined. Specify the ROM directory with -DECONOPET_ROMS_DIR=\"/path/to/roms\" on the Verilog compiler command line.");
    `endif
        file = $fopen({ `ECONOPET_ROMS_DIR, "/", filename }, "rb");
        if (file == 0) begin
            $fatal(1, "Unable to open ROM '%s' in '%s'. Verify ECONOPET_ROMS_DIR is set and points to the ROM directory.", filename, `ECONOPET_ROMS_DIR);
        end

        status = $fread(mem, file, 32'(address));
        if (status < 1) begin
            $fatal(1, "Error reading file '%s' (status=%0d).", filename, status);
        end

        $display("[%t] Loaded ROM '%s' ($%x-%x, %0d bytes)", $time, filename, address, address + RAM_ADDR_WIDTH'(status - 1), status);
        $fclose(file);
    endtask

    task automatic fill(
        input bit [RAM_ADDR_WIDTH-1:0] start_addr,
        input bit [RAM_ADDR_WIDTH-1:0] stop_addr,
        input bit [DATA_WIDTH-1:0] data
    );
        bit [RAM_ADDR_WIDTH-1:0] addr = start_addr;

        while (addr <= stop_addr) begin
            mem[addr] = data;
            addr = addr + 1;
        end
        
        $display("[%t] Filled RAM $[%x-$%x] with $%h", $time, start_addr, stop_addr, data);
    endtask

    always @(posedge clock_i) begin
        if (!ram_we_n_i) begin
            if (mem[ram_addr_i] !== ram_data_i) begin
                $display("[%t]        RAM[%h] <- %h", $time, ram_addr_i, ram_data_i);
            end
            mem[ram_addr_i] <= ram_data_i;
        end else if (!ram_oe_n_i) begin
            if (ram_data_o !== mem[ram_addr_i]) begin
                $display("[%t]        RAM[%h] -> %h", $time, ram_addr_i, mem[ram_addr_i]);
            end
            ram_data_o <= mem[ram_addr_i];
        end
    end
endmodule
