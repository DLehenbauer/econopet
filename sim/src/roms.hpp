#include "pch.hpp"
#pragma once

struct RomEntry {
    const char* file;
    uint16_t addr;
    uint16_t byteLength;
};

struct RomSet {
    RomEntry roms[15];
};

const RomSet romSets[] = {
    // VICE: BASIC 4, CRTC, 40c (i.e. 4032, 4016)
    RomSet {{
        RomEntry { /* name: */ "basic-4.901465-23-20-21.bin",    /* addr: */ 0xb000, /* byteLength: */ 0x3000 },
        RomEntry { /* name: */ "edit-4-40-n-50Hz.901498-01.bin", /* addr: */ 0xe000, /* byteLength: */ 0x0800 },
        RomEntry { /* name: */ "kernal-4.901465-22.bin",         /* addr: */ 0xf000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ nullptr,                          /* addr: */ 0x0000, /* byteLength: */ 0x0800 }
    }},

    // BASIC 1, PET 2001-8
    RomSet {{
        RomEntry { /* name: */ "rom-1-c000.901439-01.bin",       /* addr: */ 0xc000, /* byteLength: */ 0x0800 },
        RomEntry { /* name: */ "rom-1-c800.901439-05.bin",       /* addr: */ 0xc800, /* byteLength: */ 0x0800 },
        RomEntry { /* name: */ "rom-1-d000.901439-02.bin",       /* addr: */ 0xd000, /* byteLength: */ 0x0800 },
        RomEntry { /* name: */ "rom-1-d800.901439-06.bin",       /* addr: */ 0xd800, /* byteLength: */ 0x0800 },
        RomEntry { /* name: */ "edit-1-n.901439-03.bin",         /* addr: */ 0xe000, /* byteLength: */ 0x0800 },
        RomEntry { /* name: */ "rom-1-f000.901439-04.bin",       /* addr: */ 0xf000, /* byteLength: */ 0x0800 },
        RomEntry { /* name: */ "rom-1-f800.901439-07.bin",       /* addr: */ 0xf800, /* byteLength: */ 0x0800 },
        RomEntry { /* name: */ nullptr,                          /* addr: */ 0x0000, /* byteLength: */ 0x0800 }
    }},

    // BASIC 2, NON-CRTC, Normal Kbd (i.e. 2001)
    RomSet {{
        RomEntry { /* name: */ "basic-2-c000.901465-01.bin",     /* addr: */ 0xc000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ "basic-2-d000.901465-02.bin",     /* addr: */ 0xd000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ "edit-2-n.901447-24.bin",         /* addr: */ 0xe000, /* byteLength: */ 0x0800 },
        RomEntry { /* name: */ "kernal-2.901465-03.bin",         /* addr: */ 0xf000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ nullptr,                          /* addr: */ 0x0000, /* byteLength: */ 0x0800 }
    }},

    // BASIC 2, NON-CRTC, Business Kbd (i.e.2001)
    RomSet {{
        RomEntry { /* name: */ "basic-2-c000.901465-01.bin",     /* addr: */ 0xc000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ "basic-2-d000.901465-02.bin",     /* addr: */ 0xd000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ "edit-2-b.901474-01.bin",         /* addr: */ 0xe000, /* byteLength: */ 0x0800 },
        RomEntry { /* name: */ "kernal-2.901465-03.bin",         /* addr: */ 0xf000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ nullptr,                          /* addr: */ 0x0000, /* byteLength: */ 0x0800 }
    }},

    // BASIC 4, NON-CRTC (i.e. 2001)
    RomSet {{
        RomEntry { /* name: */ "basic-4-b000.901465-23.bin",     /* addr: */ 0xb000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ "basic-4-c000.901465-20.bin",     /* addr: */ 0xc000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ "basic-4-d000.901465-21.bin",     /* addr: */ 0xd000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ "edit-4-n.901447-29.bin",         /* addr: */ 0xe000, /* byteLength: */ 0x0800 },
        RomEntry { /* name: */ "kernal-4.901465-22.bin",         /* addr: */ 0xf000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ nullptr,                          /* addr: */ 0x0000, /* byteLength: */ 0x0800 }
    }},

    // BASIC 4, CRTC, 80c (i.e. 8032)
    RomSet {{
        RomEntry { /* name: */ "basic-4-b000.901465-23.bin",     /* addr: */ 0xb000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ "basic-4-c000.901465-20.bin",     /* addr: */ 0xc000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ "basic-4-d000.901465-21.bin",     /* addr: */ 0xd000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ "edit-4-80-b-60Hz.901474-03.bin", /* addr: */ 0xe000, /* byteLength: */ 0x0800 },
        RomEntry { /* name: */ "kernal-4.901465-22.bin",         /* addr: */ 0xf000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ nullptr,                          /* addr: */ 0x0000, /* byteLength: */ 0x0800 }
    }},
 
    // BASIC 4, CRTC, 40c (i.e. 4032, 4016)
    RomSet {{
        RomEntry { /* name: */ "basic-4-b000.901465-23.bin",     /* addr: */ 0xb000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ "basic-4-c000.901465-20.bin",     /* addr: */ 0xc000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ "basic-4-d000.901465-21.bin",     /* addr: */ 0xd000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ "edit-4-40-n-60Hz.901499-01.bin", /* addr: */ 0xe000, /* byteLength: */ 0x0800 },
        RomEntry { /* name: */ "kernal-4.901465-22.bin",         /* addr: */ 0xf000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ nullptr,                          /* addr: */ 0x0000, /* byteLength: */ 0x0800 }
    }},

    // BASIC 4, PET 4032 60Hz
    RomSet {{
        RomEntry { /* name: */ "basic-4-b000.901465-23.bin",     /* addr: */ 0xb000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ "basic-4-c000.901465-20.bin",     /* addr: */ 0xc000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ "basic-4-d000.901465-21.bin",     /* addr: */ 0xd000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ "edit-4-n.901447-29.bin",         /* addr: */ 0xe000, /* byteLength: */ 0x0800 },
        RomEntry { /* name: */ "kernal-4.901465-22.bin",         /* addr: */ 0xf000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ nullptr,                          /* addr: */ 0x0000, /* byteLength: */ 0x0800 }
    }},

    // BASIC 4, 4032 50 Hz
    RomSet {{
        RomEntry { /* name: */ "basic-4-b000.901465-23.bin",     /* addr: */ 0xb000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ "basic-4-c000.901465-20.bin",     /* addr: */ 0xc000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ "basic-4-d000.901465-21.bin",     /* addr: */ 0xd000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ "edit-4-40-n-50Hz.901498-01.bin", /* addr: */ 0xe000, /* byteLength: */ 0x0800 },
        RomEntry { /* name: */ "kernal-4.901465-22.bin",         /* addr: */ 0xf000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ nullptr,                          /* addr: */ 0x0000, /* byteLength: */ 0x0800 }
    }},

    // BASIC 4, PET 4032 60 Hz
    RomSet {{
        RomEntry { /* name: */ "basic-4-b000.901465-23.bin",     /* addr: */ 0xb000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ "basic-4-c000.901465-20.bin",     /* addr: */ 0xc000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ "basic-4-d000.901465-21.bin",     /* addr: */ 0xd000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ "edit-4-n.901447-29.bin",         /* addr: */ 0xe000, /* byteLength: */ 0x0800 },
        RomEntry { /* name: */ "kernal-4.901465-22.bin",         /* addr: */ 0xf000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ nullptr,                          /* addr: */ 0x0000, /* byteLength: */ 0x0800 }
    }},

    // PETTEST ROM
    RomSet {{
        RomEntry { /* name: */ "PETTESTE2KV04.bin",              /* addr: */ 0xe000, /* byteLength: */ 0x0800 },
        RomEntry { /* name: */ "kernal-4.901465-22.bin",         /* addr: */ 0xf000, /* byteLength: */ 0x1000 },
        RomEntry { /* name: */ nullptr,                          /* addr: */ 0x0000, /* byteLength: */ 0x0800 }
    }},
};
