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

module spi1_driver #(
    parameter SCK_MHZ = 24,         // SPI baud rate
    parameter WB_DATA_WIDTH = 8,
    parameter WB_ADDR_WIDTH = 20
) (
    input  logic                     clock_i,

    output logic                     spi_sck_o,
    output logic                     spi_cs_no,
    output logic                     spi_pico_o,
    input  logic                     spi_poci_i,

    input  logic                     spi_stall_i,
    output logic [WB_DATA_WIDTH-1:0] spi_data_o
);
    logic [WB_DATA_WIDTH-1:0] spi_rx_data;

    spi_driver #(SCK_MHZ) spi_driver(
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

    function [7:0] cmd(input bit we, input bit set_addr, input logic [WB_ADDR_WIDTH-1:0] addr = 20'hx_xxxx);
        return { we, set_addr, 2'bxx, addr[WB_ADDR_WIDTH-1:16] };
    endfunction

    function [7:0] addr_hi(input logic [WB_ADDR_WIDTH-1:0] addr);
        return addr[15:8];
    endfunction

    function [7:0] addr_lo(input logic [WB_ADDR_WIDTH-1:0] addr);
        return addr[7:0];
    endfunction

    task send(
        input logic unsigned [WB_DATA_WIDTH-1:0] tx[]
    );
        string s;
        s = "";
        foreach (tx[i]) begin
            if (i == '0) s = { s, $sformatf("%8b ", tx[i]) };
            else s = { s, $sformatf("%2h ", tx[i]) };
        end
        $display("[%t]      SPI1 Send: [ %s]", $time, s);

        spi_driver.send(tx, /* complete: */ '0);
        
        @(negedge spi_stall_i);

        $display("[%t]      SPI1 Received: [ %2h ]", $time, spi_rx_data);
        spi_data_o <= spi_rx_data;

        spi_driver.complete;
    endtask

    task write_at(
        input [WB_ADDR_WIDTH-1:0] addr_i,
        input [WB_DATA_WIDTH-1:0] data_i
    );
        logic [7:0] c;
        logic [7:0] ah;
        logic [7:0] al;

        $display("[%t]    spi1.write_at(%x, %x)", $time, addr_i, data_i);

        c = cmd(/* we: */ 1'b1, /* set_addr: */ 1'b1, addr_i);
        ah = addr_hi(addr_i);
        al = addr_lo(addr_i);

        send('{ c, ah, al, data_i });
    endtask

    task read_at(
        input [WB_ADDR_WIDTH-1:0] addr_i
    );
        logic [7:0] c;
        logic [7:0] ah;
        logic [7:0] al;

        $display("[%t]    spi1.read_at(%x)", $time, addr_i);

        c = cmd(/* we: */ '0, /* set_addr: */ 1'b1, addr_i);
        ah = addr_hi(addr_i);
        al = addr_lo(addr_i);

        send('{ c, ah, al });
    endtask

    task read_next();
        logic [7:0] c;

        $display("[%t]    spi1.read_next()", $time);

        c = cmd(/* we: */ '0, /* set_addr: */ '0, 6'bxxxxxx);

        send('{ c });
    endtask
endmodule
