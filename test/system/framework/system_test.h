// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet
#pragma once
#include <array>
#include <iostream>
#include <string>

#include <gtest/gtest.h>

#include "system.h"

// Check exception categories and live diagnostic evidence without advancing the board.
template<class Exception, class Operation>
void expect_failure(const System& system, const std::string& operation, Operation action,
                    const std::string& detail = "") {
    try {
        action();
        FAIL() << "expected " << operation << " to fail";
    } catch (const Exception& error) {
        const std::string message = error.what();
        for (const auto& field : {operation, detail, std::string("cycle="),
             std::string("half_tick="), std::string("cpu="),
             "seed=" + std::to_string(system.seed()), std::string("recent={")})
            EXPECT_NE(message.find(field), std::string::npos) << message;
    }
}

// Each board test owns a fresh production model without firmware or ROM media.
class SystemTest : public testing::Test {
protected:
    // Include the actual board seed in assertions independently of shuffle seeds.
    SystemTest() : seed_trace_(__FILE__, __LINE__,
        "ECONOPET_SIM_SEED=" + std::to_string(system.seed())) {
        RecordProperty("simulation_seed", std::to_string(system.seed()));
    }
    // Report bounded live evidence for assertion failures outside board APIs.
    void TearDown() override {
        if (HasFailure()) std::cerr << system.diagnostic("test failure", "framework assertion") << '\n';
    }
    // Check actual generated-model pins without duplicating the public snapshot API.
    void expect_stimulus_pins(const System::RawStimulus& expected) const {
        const auto& pins = *system.dut_;
        EXPECT_EQ(pins.cpu_addr_i, expected.cpu.address.value());
        EXPECT_EQ(pins.cpu_we_n_i, !expected.cpu.write_data.has_value());
        EXPECT_EQ(pins.cpu_sync_i, expected.cpu.sync);
        EXPECT_EQ(pins.cpu_reset_n_i, expected.cpu_reset_n_i);
        EXPECT_EQ(pins.cpu_irq_n_i, expected.cpu_irq_n_i);
        EXPECT_EQ(pins.cpu_nmi_n_i, expected.cpu_nmi_n_i);
        EXPECT_EQ(pins.diag_i, expected.diag_i);
        EXPECT_EQ(pins.audio_det_i, expected.audio_det_i);
        EXPECT_EQ(pins.config_hz_i, expected.config_hz_i);
        EXPECT_EQ(pins.spi1_cs_ni, expected.spi1_cs_ni);
        EXPECT_EQ(pins.spi1_sck_i, expected.spi1_sck_i);
        EXPECT_EQ(pins.spi1_sd_i, expected.spi1_sd_i);
        EXPECT_EQ(pins.spi1_sdo_i, expected.spi1_sdo_i);
        EXPECT_EQ(pins.i2c0_scl_i, expected.i2c0_scl_i);
        EXPECT_EQ(pins.i2c0_sda_i, expected.i2c0_sda_i);
        EXPECT_EQ(pins.i2c1_scl_i, expected.i2c1_scl_i);
        EXPECT_EQ(pins.i2c1_sda_i, expected.i2c1_sda_i);
        EXPECT_EQ(pins.mcu_cec_i, expected.mcu_cec_i);
        EXPECT_EQ(pins.pmod1_i, expected.pmod1_i);
        EXPECT_EQ(pins.pmod2_i, expected.pmod2_i);
        const std::array<bool, 6> spare{
            bool(pins.sp1_i), bool(pins.sp2_i), bool(pins.sp3_i),
            bool(pins.sp6_i), bool(pins.sp7_i), bool(pins.sp8_i)};
        EXPECT_EQ(spare, expected.spare);
    }
    System system;
private:
    testing::ScopedTrace seed_trace_;
};
