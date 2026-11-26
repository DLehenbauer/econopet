// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "framework/firmware_test.h"

#include <fstream>
#include <optional>
#include <vector>

#include "scheduler.h"

extern "C" {
#include "display/display.h"
#include "driver.h"
#include "ieee/ieee_drive.h"
#include "input/keyboard_core.h"
#include "input/pet_keyboard.h"
#include "roms/roms.h"
#include "system/state.h"
#include "system/system.h"
}

/** Run both assembled ROM entries and observe CPU-published completion through SPI. */
TEST_F(FirmwareTest, AssembledDiagnosticRomCompletesBeforeBootDeadline) {
    constexpr size_t RomBytes = 0x100;
    constexpr unsigned JumpEntryBytes = 3;
    constexpr econopet::Cycles BootBudget{128'000'000};
    std::array<uint8_t, RomBytes> rom;
    std::ifstream file(ECONOPET_MENU_BOOT_ROM, std::ios::binary);
    ASSERT_TRUE(file.read(reinterpret_cast<char*>(rom.data()), rom.size()));
    ASSERT_EQ(file.peek(), std::ifstream::traits_type::eof());

    for (const auto reason : {MENU_ROM_BOOT_NORMAL, MENU_ROM_BOOT_ERROR}) {
        SCOPED_TRACE(reason);
        system.cpu(CPU_SOFT_6502).prepare();
        spi_write(MENU_ROM_START_ADDRESS, rom.data(), rom.size());
        spi_write_at(MENU_ROM_COMPLETION_ADDRESS, 0);
        system.cpu(CPU_SOFT_6502).start(econopet::CpuAddress{
            MENU_ROM_START_ADDRESS + JumpEntryBytes * static_cast<unsigned>(reason)});
        uint8_t observed = 0;
        const auto start = system.time();
        system.execution().within(BootBudget)
            .service([&](System&, econopet::Cycles) {
                observed = spi_read_once(MENU_ROM_COMPLETION_ADDRESS);
            })
            .until([&](const System&) { return observed == MENU_ROM_COMPLETION_VALUE; })
            .run("assembled diagnostic completion");
        EXPECT_LT(system.time() - start, BootBudget);
        EXPECT_EQ(system.peek(econopet::SramAddress{MENU_ROM_COMPLETION_ADDRESS}),
            MENU_ROM_COMPLETION_VALUE);
        EXPECT_NE(spi_read_at(ECONOPET_WB_STATUS_ADDR) & ECONOPET_REG_STATUS_TEXT_MODE_MASK, 0);
    }
}

/** Run real session/scanner SPI against production top, SRAM, and physical chip models. */
TEST_F(FirmwareTest, BackRestoresBorrowedBytesAndResumesAfterPhysicalScanning) {
    for (const auto selection : {CPU_SOFT_6502, CPU_SOFT_6809}) {
        const econopet::CpuAddress entry{0x0300};
        const econopet::SramAddress marker{0x0200};
        const std::array<uint8_t, 8> code6502{0x58, 0xee, 0x00, 0x02, 0x4c, 0x01, 0x03, 0xea};
        const std::array<uint8_t, 8> code6809{0x7c, 0x02, 0x00, 0x20, 0xfb, 0x12, 0x12, 0x12};
        const auto& code = selection == CPU_SOFT_6502 ? code6502 : code6809;
        for (size_t index = 0; index < code.size(); ++index) {
            system.poke(econopet::SramAddress{entry.value() + index}, code[index]);
        }
        system.cpu(selection).prepare();
        system.poke(econopet::SramAddress{0x0201}, 0);
        const std::array<uint8_t, 7> handler{0xee, 0x01, 0x02, 0xad, 0x44, 0xe8, 0x40};
        for (size_t index = 0; index < handler.size(); ++index) {
            system.poke(econopet::SramAddress{0x0500 + index}, handler[index]);
        }
        system.poke(econopet::SramAddress{0xfffe}, 0x00);
        system.poke(econopet::SramAddress{0xffff}, 0x05);
        std::array<uint8_t, PET_MAX_VIDEO_RAM_BYTES> screen;
        std::array<uint8_t, 1u << ECONOPET_BRAM_ADDR_WIDTH> font;
        for (size_t index = 0; index < screen.size(); ++index) {
            screen[index] = index * 17 + 3;
        }
        for (size_t index = 0; index < font.size(); ++index) {
            font[index] = index * 7 + 1;
        }
        spi_write(VIDEO_RAM_ADDRESS, screen.data(), screen.size());
        spi_write(ECONOPET_WB_BRAM_BASE_ADDR, font.data(), font.size());
        system_state.video_source = video_source_pet;
        system.cpu(selection).start(entry);
        system.tick(1024);
        set_cpu(CPU_HALT);
        pet_keyboard_begin_boot();
        pet_keyboard_end();
        pet_io_write(0xe813, 0x3c);
        pet_io_write(0xe84c, 0xcc);
        set_cpu(CPU_READY);
        const auto control = get_cpu();
        ASSERT_TRUE(system_suspend());
        const auto saved_marker = system.peek(marker);
        if (selection == CPU_SOFT_6502) {
            pet_io_write(0xe84e, 0xc0);
            pet_io_write(0xe844, 0x20);
            pet_io_write(0xe845, 0x00);
        }
        const auto via_writes = system.io().via().writes().count();
        const auto pia2_writes = system.io().pia2().writes().count();
        for (unsigned step = 0; step < 3 * ECONOPET_KBD_COL_COUNT; ++step) {
            pet_keyboard_task();
            sync_state();
            system.tick(6400);
            EXPECT_EQ(system.peek(marker), saved_marker);
            EXPECT_FALSE(system.snapshot().cpu_be_o);
        }
        ASSERT_TRUE(pet_keyboard_ready());
        if (selection == CPU_SOFT_6502) {
            EXPECT_NE(pet_io_read(0xe84d) & 0x40, 0);
            EXPECT_FALSE(system.snapshot().cpu_irq_n_o);
            EXPECT_EQ(system.peek(econopet::SramAddress{0x0201}), 0);
        }
        EXPECT_EQ(system.io().via().writes().count(), via_writes);
        EXPECT_EQ(system.io().pia2().writes().count(), pia2_writes);
        std::array<uint8_t, VIDEO_PLANE_CAPACITY> menu;
        menu.fill(0x20);
        spi_write(VIDEO_RAM_ADDRESS, menu.data(), menu.size());
        system_restore(true);
        EXPECT_EQ(get_cpu(), control);
        set_cpu(CPU_HALT);
        std::array<uint8_t, PET_MAX_VIDEO_RAM_BYTES> restored;
        std::array<uint8_t, 1u << ECONOPET_BRAM_ADDR_WIDTH> restored_font;
        spi_read(VIDEO_RAM_ADDRESS, restored.size(), restored.data());
        spi_read(ECONOPET_WB_BRAM_BASE_ADDR, restored_font.size(), restored_font.data());
        EXPECT_EQ(restored, screen);
        EXPECT_EQ(restored_font, font);
        EXPECT_EQ(pet_io_read(0xe84c), 0xcc);
        set_cpu(CPU_READY);
        const auto resumed = system.peek(marker);
        system.tick(4096);
        EXPECT_NE(system.peek(marker), resumed);
        if (selection == CPU_SOFT_6502) {
            EXPECT_EQ(system.peek(econopet::SramAddress{0x0201}), 1);
        }
    }
}

/** Coordinate physical VIA/PIA2 IEEE directions across menu mounts on soft and physical CPUs. */
TEST_F(FirmwareTest, MenuMountAndUnmountSynchronizePhysicalIeeeDirections) {
    const econopet::CpuAddress entry{0x0300};
    const auto code6502 = std::to_array<uint8_t>({
        0xa9, 0x1e, 0x8d, 0x42, 0xe8, // LDA #$1E : STA $E842
        0xa9, 0x00, 0x8d, 0x23, 0xe8, // LDA #$00 : STA $E823
        0xa9, 0xff, 0x8d, 0x22, 0xe8, // LDA #$FF : STA $E822 (DDRB)
        0xa9, 0x3c, 0x8d, 0x21, 0xe8, // LDA #$3C : STA $E821
        0x8d, 0x23, 0xe8,             // STA $E823
        0xa9, 0x1a, 0x8d, 0x40, 0xe8, // LDA #$1A : STA $E840 (ATN low, cassette high)
        0x4c, 0x00, 0x03,             // JMP $0300
    });
    const auto code6809 = std::to_array<uint8_t>({
        0x86, 0x06, 0xb7, 0xe8, 0x42, // LDA #$06 : STA $E842 (Waterloo leaves PB4:3 as inputs)
        0x86, 0x00, 0xb7, 0xe8, 0x23, // LDA #$00 : STA $E823
        0x86, 0xff, 0xb7, 0xe8, 0x22, // LDA #$FF : STA $E822 (DDRB)
        0x86, 0x3c, 0xb7, 0xe8, 0x21, // LDA #$3C : STA $E821
        0xb7, 0xe8, 0x23,             // STA $E823
        0x86, 0x02, 0xb7, 0xe8, 0x40, // LDA #$02 : STA $E840 (ATN low, PB4:3 zeroed like Waterloo)
        0x7e, 0x03, 0x00,             // JMP $0300
    });
    firmware.mount(disk_fixture::D64::empty(), "menu.d64");
    ASSERT_TRUE(ieee_drive_unmount_all());

    for (const auto selection : {CPU_SOFT_6502, CPU_SOFT_6809}) {
        SCOPED_TRACE(static_cast<int>(selection));
        const auto& code = selection == CPU_SOFT_6502 ? code6502 : code6809;
        for (size_t index = 0; index < code.size(); ++index) {
            system.poke(econopet::SramAddress{entry.value() + index}, code[index]);
        }

        ASSERT_TRUE(ieee_drive_unmount_all());
        system.cpu(selection).prepare();
        system_state.video_source = video_source_pet;
        system.cpu(selection).start(entry);
        system.tick(4096);
        // Without virtual media, physical VIA/PIA2 enable IEEE outputs and pull ATN low.
        EXPECT_EQ(system.io().via().state().pb.ddr & 0x06u, 0x06u);
        EXPECT_EQ(system.io().via().state().pb.outr & 0x04u, 0x00u);
        EXPECT_EQ(system.io().pia2().state().pb.ddr, 0xffu);

        // Suspend into menu, simulate an external 4040/8050 peripheral stuck mid-transfer on the
        // physical IEEE connector (holding DAV and NDAC low until ATN + UNLISTEN + UNTALK), mount
        // a virtual disk, and resume: firmware retires the external session, waits for the bus to
        // release, and then parks VIA PB2:1 and PIA2 as inputs. On the second iteration, verify
        // the bounded timeout path when an external line is held low with no responsive device.
        ASSERT_TRUE(system_suspend());
        ASSERT_EQ(ieee_drive_try_mount(0, "menu.d64", nullptr), DOS_MEDIA_OK);
        if (selection == CPU_SOFT_6502) {
            bool ext_atn_prev = false;
            bool ext_dav_prev = true;
            bool ext_listening = true;
            bool ext_talking = true;
            std::optional<econopet::CycleTime> acceptor_ready;
            constexpr econopet::Cycles AdapterResponseDelay{50u * 64u};
            std::vector<uint8_t> atn_commands;
            system.io().via().inputs([](econopet::io::Inputs& in) {
                in.port_b = static_cast<uint8_t>(econopet::io::PortAllHigh & ~(0x80u | 0x01u));
            });
            {
                Scheduler scheduler(system);
                auto step_ext_peer = [&](System& board) {
                    const auto& via = board.io().via().state();
                    const auto& pia2 = board.io().pia2();
                    const bool atn_n = ((via.pb.outr | ~via.pb.ddr) & 0x04u) != 0;
                    const bool dav_n = pia2.cb2();
                    const uint8_t dio_n = pia2.state().pb.outr | ~pia2.state().pb.ddr;

                    // Model an adapter releasing DAV before its delayed ATN interrupt arms NDAC.
                    if (!atn_n && ext_atn_prev) {
                        board.io().via().inputs([](econopet::io::Inputs& in) {
                            in.port_b = econopet::io::PortAllHigh;
                        });
                        acceptor_ready = board.time() + AdapterResponseDelay;
                    }
                    if (!atn_n && acceptor_ready && !(board.time() < *acceptor_ready)) {
                        board.io().via().inputs([](econopet::io::Inputs& in) {
                            in.port_b = static_cast<uint8_t>(econopet::io::PortAllHigh & ~0x01u);
                        });
                        acceptor_ready.reset();
                    }
                    // ATN30..ATN40: accept command byte on DAV low (releasing NDAC) and re-arm NDAC on DAV high.
                    if (!atn_n && !dav_n && ext_dav_prev) {
                        const uint8_t cmd = static_cast<uint8_t>(~dio_n);
                        atn_commands.push_back(cmd);
                        if (cmd == 0x3fu) ext_listening = false;
                        if (cmd == 0x5fu) ext_talking = false;
                        board.io().via().inputs([](econopet::io::Inputs& in) {
                            in.port_b = static_cast<uint8_t>(econopet::io::PortAllHigh & ~0x40u);
                        });
                    } else if (!atn_n && dav_n && !ext_dav_prev) {
                        board.io().via().inputs([](econopet::io::Inputs& in) {
                            in.port_b = static_cast<uint8_t>(econopet::io::PortAllHigh & ~0x01u);
                        });
                    }
                    // ATN50..ATN60: releasing ATN returns unaddressed peripherals to IDLE (all lines high).
                    if (atn_n && !ext_atn_prev) {
                        acceptor_ready.reset();
                        board.io().via().inputs([&](econopet::io::Inputs& in) {
                            in.port_b = (ext_listening || ext_talking)
                                            ? static_cast<uint8_t>(econopet::io::PortAllHigh & ~0x01u)
                                            : econopet::io::PortAllHigh;
                        });
                    }
                    ext_atn_prev = atn_n;
                    ext_dav_prev = dav_n;
                };
                for (uint32_t step = 1; step <= 4096; ++step) {
                    scheduler.at(system.time() + econopet::Cycles{step * 256u}, step_ext_peer);
                }
                system_restore(true);
            }
            EXPECT_EQ(atn_commands, (std::vector<uint8_t>{0x3fu, 0x5fu}));
            EXPECT_FALSE(ext_listening);
            EXPECT_FALSE(ext_talking);
            EXPECT_EQ(system.io().via().peek_inputs().port_b, econopet::io::PortAllHigh);
        } else {
            // Unpowered / stuck external peripheral holding NDAC low must bound-timeout cleanly.
            system.io().via().inputs([](econopet::io::Inputs& in) {
                in.port_b = static_cast<uint8_t>(econopet::io::PortAllHigh & ~0x01u);
            });
            system_restore(true);
            system.io().via().inputs([](econopet::io::Inputs& in) {
                in.port_b = econopet::io::PortAllHigh;
            });
        }
        system.tick(4096);
        EXPECT_EQ(system.io().via().state().pb.ddr, selection == CPU_SOFT_6502 ? 0x18u : 0x00u);
        EXPECT_NE((system.io().via().state().pb.outr | ~system.io().via().state().pb.ddr) & 0x10u, 0u);
        EXPECT_EQ(system.io().pia2().state().pb.ddr, 0x00u);
        EXPECT_TRUE(system.io().pia2().ca2());
        EXPECT_TRUE(system.io().pia2().cb2());

        // Suspend again, unmount all virtual disks, and resume: physical IEEE outputs return.
        ASSERT_TRUE(system_suspend());
        ASSERT_TRUE(ieee_drive_unmount_all());
        system_restore(true);
        system.tick(4096);
        EXPECT_EQ(system.io().via().state().pb.ddr & 0x06u, 0x06u);
        EXPECT_EQ(system.io().pia2().state().pb.ddr, 0xffu);
    }

    // Also verify socketed physical-6502 bus writes obey suppression/pass-through and lazy VIA DDRB sync.
    ASSERT_EQ(ieee_drive_try_mount(0, "menu.d64", nullptr), DOS_MEDIA_OK);
    system.cpu(CPU_PHYS_6502).prepare();
    set_cpu(CPU_HALT);
    system_park_ieee_io();
    system.cpu(CPU_PHYS_6502).release_reset();
    system.drive_physical_cpu({pet::ViaPortB, 0x1eu, false});
    system.tick(128);
    system.drive_physical_cpu({pet::ViaDdrB, 0x1eu, false});
    system.tick(128);
    system.drive_physical_cpu({pet::Pia2ControlB, 0x3cu, false});
    system.tick(128);
    system.drive_physical_cpu({pet::ViaPortB, 0x06u, false});
    system.tick(128);
    system.drive_physical_cpu({});
    EXPECT_EQ(system.io().via().state().pb.ddr, 0x00u);
    system_sync_ieee_io();
    EXPECT_EQ(system.io().via().state().pb.ddr, 0x18u);
    EXPECT_EQ(system.io().via().state().pb.outr & 0x18u, 0x00u);
    EXPECT_EQ(system.io().pia2().state().pb.ddr, 0x00u);
    EXPECT_TRUE(system.io().pia2().cb2());
}
