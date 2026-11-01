// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "firmware.h"
#include "ieee/controller.h"
#include "ieee/trace.h"

extern "C" {
#include "sd/sd.h"
}

namespace {
constexpr uint64_t SystemClockHz = 64'000'000;
constexpr uint64_t AttosecondsPerSecond = 1'000'000'000'000'000'000;
constexpr size_t ActionLimit = 20'000;
constexpr size_t EventLimit = 2'000'000;
constexpr econopet::Cycles ServiceBudget{200'000};

struct Action {
    std::string test_case;
    char kind;
    unsigned byte, atn, eoi;
};

// Validate the entire external script before mounting media or clocking a board.
std::vector<Action> load_actions(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::string line;
    if (!std::getline(input, line) || line != "ieee-replay-v1")
        throw std::invalid_argument("invalid IEEE replay header");
    std::vector<Action> actions;
    while (std::getline(input, line)) {
        Action action;
        std::string extra;
        std::istringstream fields(line);
        if (!(fields >> action.test_case >> action.kind >> action.byte >> action.atn >> action.eoi) ||
            (fields >> extra) || action.byte > 255 || action.atn > 1 || action.eoi > 1 ||
            (action.kind != 'H' && action.kind != 'R' && action.kind != 'A') ||
            action.test_case.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789") != std::string::npos ||
            actions.size() == ActionLimit)
            throw std::invalid_argument("invalid or oversized IEEE replay action");
        actions.push_back(action);
    }
    if (input.bad() || actions.empty())
        throw std::runtime_error("incomplete IEEE replay script");
    return actions;
}

// Own an output stream that fails loudly on partial writes.
std::ofstream output_file(const std::filesystem::path& path) {
    std::ofstream output;
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.open(path, std::ios::binary);
    return output;
}

// Preserve all backing bytes before session teardown unregisters its writable file.
void save_media(const char* name, size_t size, const std::filesystem::path& output) {
    char path[SD_PATH_MAX];
    sd_make_path(path, SD_DIR_DISKS, name);
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(std::fopen(path, "rb"), &std::fclose);
    if (!file)
        throw std::runtime_error("cannot read mounted replay image");
    std::vector<char> bytes(size);
    if (std::fread(bytes.data(), 1, bytes.size(), file.get()) != size || std::fgetc(file.get()) != EOF ||
        std::ferror(file.get()))
        throw std::runtime_error("incomplete mounted replay image");
    auto destination = output_file(output);
    destination.write(bytes.data(), bytes.size());
    destination.close();
}

// Record completed-cycle composed nets without reading or acknowledging a peripheral.
class NetTrace {
  public:
    // Keep storage alive until after the scoped observer disconnects.
    NetTrace(System& board, const std::filesystem::path& path) : output_(output_file(path)), board_(board) {
        output_ << "sequence,seconds,event,source,atn,eoi,dav,nrfd,ndac,ifc,dio\n";
        append("initial");
        subscription_ = board.observe([this](const System&) {
            if (sample() != previous_)
                append("edge");
        });
    }
    // Close coverage before firmware unmount can change the bus.
    void finish() {
        subscription_.reset();
        append("stop");
        output_.close();
    }

  private:
    // Observe the virtual bus consistently, not the separately delayed fitted PIA outputs.
    std::array<unsigned, 7> sample() const {
        const auto pins = board_.snapshot();
        return {unsigned(pins.ieee_bus_atn_no),  unsigned(pins.ieee_bus_eoi_no),  unsigned(pins.ieee_bus_dav_no),
                unsigned(pins.ieee_bus_nrfd_no), unsigned(pins.ieee_bus_ndac_no), 1,
                unsigned(pins.ieee_bus_dio_o)};
    }
    // Retain integer 64 MHz time rather than rounding sub-microsecond phases to floats.
    void append(const char* event) {
        if (++count_ > EventLimit)
            throw std::runtime_error("replay net trace exceeded event limit");
        const auto cycles = board_.time().value();
        output_ << count_ << ',' << cycles / SystemClockHz << '.' << std::setfill('0') << std::setw(18)
                << (cycles % SystemClockHz) * (AttosecondsPerSecond / SystemClockHz) << ',' << event << ",system";
        previous_ = sample();
        for (const auto level : previous_)
            output_ << ',' << level;
        output_ << '\n';
    }
    std::ofstream output_;
    System& board_;
    size_t count_ = 0;
    std::array<unsigned, 7> previous_{};
    System::ObserverSubscription subscription_;
};

// Drain actual RX/ATN-end events through firmware's physical SPI service adapter.
void service_rx(System& board, FirmwareSession& firmware) {
    board.execution()
        .within(ServiceBudget)
        .service(firmware.service_adapter())
        .until([](const System& observed) { return !observed.snapshot().ieee_rx_ready_o; })
        .run("reference replay RX service");
}

// Run identical byte/ATN/EOI stimuli, retaining every replacement DOS difference.
int replay(const std::filesystem::path& image_path, const std::filesystem::path& script,
           const std::filesystem::path& directory, const std::filesystem::path& source_path) {
    const auto actions = load_actions(script);
    std::ifstream input(image_path, std::ios::binary);
    const std::vector<uint8_t> image{std::istreambuf_iterator<char>(input), {}};
    if (input.bad() || image.empty()) {
        throw std::runtime_error("cannot read replay input image");
    }
    if (!std::filesystem::create_directory(directory)) {
        throw std::invalid_argument("replay output directory already exists");
    }
    System board;
    IeeeController controller(board);
    FirmwareSession firmware(board);
    const auto name = "reference" + image_path.extension().string();
    firmware.mount(image, name.c_str());
    std::vector<uint8_t> source;
    const auto source_name = "source" + image_path.extension().string();
    if (!source_path.empty()) {
        std::ifstream source_input(source_path, std::ios::binary);
        source.assign(std::istreambuf_iterator<char>(source_input), {});
        if (source_input.bad() || source.empty()) {
            throw std::runtime_error("cannot read replay source image");
        }
        firmware.mount(source, source_name.c_str(), 1);
    }
    NetTrace trace(board, directory / "ieee-wire.csv");
    auto received = output_file(directory / "received.csv");
    received << "action,case,expected,actual,expected_eoi,actual_eoi\n";
    size_t completed = 0;
    std::string failure;
    try {
        for (const auto& action : actions) {
            if (action.kind == 'A') {
                controller.release_atn();
            } else if (action.kind == 'H') {
                controller.raw_send(ieee_test::raw_record(action.byte, action.atn,
                                                          action.eoi ? ieee_test::Eoi::Last : ieee_test::Eoi::More));
            } else {
                firmware.service();
                const auto value = controller.receive();
                received << completed << ',' << action.test_case << ',' << action.byte << ',' << unsigned(value.data)
                         << ',' << action.eoi << ',' << (value.eoi == ieee_test::Eoi::Last) << '\n';
            }
            service_rx(board, firmware);
            ++completed;
        }
    } catch (const std::exception& error) {
        failure = error.what();
    }
    // Keep complete media and the observed prefix even when a legacy limitation stops replay.
    trace.finish();
    received.close();
    save_media(name.c_str(), image.size(), directory / ("final" + image_path.extension().string()));
    if (!source_path.empty()) {
        save_media(source_name.c_str(), source.size(), directory / ("source-final" + image_path.extension().string()));
    }
    auto coverage = output_file(directory / "coverage.txt");
    coverage << "completed=" << completed << "\nrequested=" << actions.size() << "\nfailure=" << failure << '\n';
    coverage.close();
    firmware.close();
    return failure.empty() ? 0 : 2;
}
} // namespace

// Accept explicit disposable input/output paths, never operator SD-card images by default.
int main(int argc, char** argv) {
    try {
        if (argc != 4 && argc != 5) {
            throw std::invalid_argument(
                "usage: econopet_drive_reference IMAGE REPLAY NEW_OUTPUT_DIRECTORY [SOURCE_IMAGE]");
        }
        return replay(argv[1], argv[2], argv[3], argc == 5 ? std::filesystem::path{argv[4]} : std::filesystem::path{});
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
