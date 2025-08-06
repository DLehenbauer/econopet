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

`include "./src/common_pkg.svh"

import common_pkg::*;

module spi1_driver(
    input  logic clock_i,

    output logic spi_sck_o,
    output logic spi_cs_no,
    output logic spi_pico_o,
    input  logic spi_poci_i,

    input  logic spi_stall_i,
    output logic [DATA_WIDTH-1:0] spi_data_o
);
    logic [DATA_WIDTH-1:0] spi_rx_data;

    spi_driver spi_driver (
        .clock_i(clock_i),
        .spi_sck_o(spi_sck_o),
        .spi_cs_no(spi_cs_no),
        .spi_sd_i(spi_poci_i),
        .spi_sd_o(spi_pico_o),
        .spi_data_o(spi_rx_data)
    );

    task reset;
        $display("[%t]    spi1.reset()", $time);
        spi_driver.reset();
    endtask

    localparam bit [1:0] ADDR_MODE_SAME = 2'b00,
                         ADDR_MODE_INC  = 2'b01,
                         ADDR_MODE_SEEK = 2'b10,
                         ADDR_MODE_DEC  = 2'b11;

    function [7:0] cmd(input bit we, input bit [1:0] addr_mode, input logic [WB_ADDR_WIDTH-1:0] addr = 20'bx);
        return { we, addr_mode, 1'bx, addr[WB_ADDR_WIDTH-1:16] };
    endfunction

    function [7:0] addr_hi(input logic [WB_ADDR_WIDTH-1:0] addr);
        return addr[15:8];
    endfunction

    function [7:0] addr_lo(input logic [WB_ADDR_WIDTH-1:0] addr);
        return addr[7:0];
    endfunction

    task send(input logic unsigned [DATA_WIDTH-1:0] tx[]);
        string s;
        s = "";
        foreach (tx[i]) begin
            if (i == '0) s = {s, $sformatf("%8b ", tx[i])};
            else s = {s, $sformatf("%2h ", tx[i])};
        end
        $display("[%t]      SPI1 Send: [ %s]", $time, s);

        spi_driver.send(tx,  /* complete: */ '0);

        @(negedge spi_stall_i);

        $display("[%t]      SPI1 Received: [ %2h ]", $time, spi_rx_data);
        spi_data_o <= spi_rx_data;

        spi_driver.complete;
    endtask

    task write_at(
        input [WB_ADDR_WIDTH-1:0] addr_i,
        input [   DATA_WIDTH-1:0] data_i
    );
        logic [7:0] c;
        logic [7:0] ah;
        logic [7:0] al;

        $display("[%t]    spi1.write_at(%x, %x)", $time, addr_i, data_i);

        c  = cmd(/* we: */ 1'b1, ADDR_MODE_SEEK, addr_i);
        ah = addr_hi(addr_i);
        al = addr_lo(addr_i);

        send('{c, ah, al, data_i});
    endtask

    task read_at(input [WB_ADDR_WIDTH-1:0] addr_i);
        logic [7:0] c;
        logic [7:0] ah;
        logic [7:0] al;

        $display("[%t]    spi1.read_at(%x)", $time, addr_i);

        c  = cmd(/* we: */ '0, ADDR_MODE_SEEK, addr_i);
        ah = addr_hi(addr_i);
        al = addr_lo(addr_i);

        send('{c, ah, al});
    endtask

    task read_next();
        logic [7:0] c;

        $display("[%t]    spi1.read_next()", $time);

        c = cmd(/* we: */ '0, ADDR_MODE_INC, 20'bx);

        send('{c});
    endtask

    task read_prev();
        logic [7:0] c;

        $display("[%t]    spi1.read_prev()", $time);

        c = cmd(/* we: */ '0, ADDR_MODE_DEC, 20'bx);

        send('{c});
    endtask

    task read_same();
        logic [7:0] c;

        $display("[%t]    spi1.read_same()", $time);

        c = cmd(/* we: */ '0, ADDR_MODE_SAME, 20'bx);

        send('{c});
    endtask

    task write_next(input [DATA_WIDTH-1:0] data_i);
        logic [7:0] c;

        $display("[%t]    spi1.write_next(%x)", $time, data_i);

        c = cmd(/* we: */ 1'b1, ADDR_MODE_INC, 20'bx);

        send('{c, data_i});
    endtask

    task write_prev(input [DATA_WIDTH-1:0] data_i);
        logic [7:0] c;

        $display("[%t]    spi1.write_prev(%x)", $time, data_i);

        c = cmd(/* we: */ 1'b1, ADDR_MODE_DEC, 20'bx);

        send('{c, data_i});
    endtask

    task write_same(input [DATA_WIDTH-1:0] data_i);
        logic [7:0] c;

        $display("[%t]    spi1.write_same(%x)", $time, data_i);

        c = cmd(/* we: */ 1'b1, ADDR_MODE_SAME, 20'bx);

        send('{c, data_i});
    endtask
endmodule
