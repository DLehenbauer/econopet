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

module stopwatch;
    bit running = '0;
    time startTime;
    time stopTime;

    task start;
        startTime = $time;
        running = 1'b1;
    endtask

    task stop;
        stopTime = $time;
        running = 1'b0;
    endtask

    function time elapsed;
        return (running ? $time : stopTime) - startTime;
    endfunction

    function real freq_hz();
        real e, f;

        e = elapsed();
        f = 1.0e9 / e;
        return f;
    endfunction

    function real freq_khz();
        real e, f;

        e = elapsed();
        f = 1.0e6 / e;
        return f;
    endfunction

    function real freq_mhz();
        real e, f;

        e = elapsed();
        f = 1.0e3 / e;
        return f;
    endfunction
endmodule
