// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

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
