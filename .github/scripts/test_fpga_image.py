#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet

"""Test FPGA image boundaries and SWD programming without hardware."""

import importlib.util
import json
import os
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FPGA_ADDRESS = 0x10F3C000
SPEC = importlib.util.spec_from_file_location("fpga_image", ROOT / "tools/fpga_image.py")
IMAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(IMAGE)


class FpgaImageTests(unittest.TestCase):
    """Check the shared image generator and OpenOCD retry path."""

    def test_partition_limits(self):
        """Allow the exact maximum payload and reject empty or oversized inputs."""
        self.assertEqual(len(IMAGE.build_image(b"x" * 48, 64)), 64)
        with self.assertRaises(ValueError):
            IMAGE.build_image(b"x" * 49, 64)
        with self.assertRaises(ValueError):
            IMAGE.build_image(b"", 64)

    def prepare_probe(self, root):
        """Run generated Tcl in OpenOCD with fake flash and no hardware initialization."""
        real_openocd = shutil.which("openocd")
        self.assertIsNotNone(real_openocd, "OpenOCD is required to execute the Tcl fixture")
        (root / "tools").mkdir()
        shutil.copyfile(ROOT / "tools/fpga_image.py", root / "tools/fpga_image.py")
        shutil.copytree(ROOT / "cmake", root / "cmake")
        bindir = root / "bin"
        bindir.mkdir()
        log = root / "openocd.log"
        flash = root / "flash"
        flash.mkdir()
        (root / "probe.tcl").write_text(textwrap.dedent("""\
            # SPDX-License-Identifier: CC0-1.0
            # Read fixture images without changing their binary representation.
            proc image_bytes {filename} {
                set channel [open $filename rb]
                set data [read $channel]
                close $channel
                return $data
            }
            # Record observable checks, writes and resets across probe sessions.
            proc record {event} {
                set channel [open $::env(OPENOCD_EVENTS) a]
                puts $channel $event
                close $channel
            }
            # Compare the image with persistent simulated board contents.
            proc verify_image_checksum {filename args} {
                record "verify [file tail $filename]"
                set snapshot [file join $::env(OPENOCD_FLASH) [file tail $filename]]
                if {![file exists $snapshot] || [image_bytes $filename] ne [image_bytes $snapshot]} {
                    error "checksum mismatch"
                }
            }
            # Model writes and the bootloader's first-sector firmware-vector erasure.
            proc program {filename args} {
                set name [file tail $filename]
                record "program $name"
                set channel [open [file join $::env(OPENOCD_FLASH) $name] wb]
                puts -nonewline $channel [image_bytes $filename]
                close $channel
                if {$name eq "bootloader.elf"} {
                    file delete -force [file join $::env(OPENOCD_FLASH) firmware.elf]
                    set marker [file join $::env(OPENOCD_FLASH) failed]
                    if {[info exists ::env(OPENOCD_FAIL)] &&
                        $::env(OPENOCD_FAIL) eq "after-boot-once" && ![file exists $marker]} {
                        close [open $marker w]
                        error "probe disconnected after bootloader write"
                    }
                }
            }
            # Observe resets without executing any application code.
            proc reset {mode} { record "reset $mode" }
            # Ignore initialization because the fixture has no target or probe.
            proc init {} {}
            # Ignore adapter setup because the fixture has no physical adapter.
            proc adapter {args} {}
            # Ignore recovery setup because the fixture has no physical adapter.
            proc cmsis-dap {args} {}
            # Ignore polling because the fixture has no target.
            proc poll {args} {}
            """))
        mock = bindir / "openocd"
        mock.write_text(
            '#!/usr/bin/env python3\n'
            '# SPDX-License-Identifier: CC0-1.0\n'
            'import json, os, subprocess, sys\n'
            'from pathlib import Path\n'
            'log = Path(os.environ["OPENOCD_LOG"])\n'
            'previous = log.read_text().splitlines() if log.exists() else []\n'
            'with log.open("a") as output: output.write(json.dumps(sys.argv[1:]) + "\\n")\n'
            'failure = os.environ.get("OPENOCD_FAIL", "")\n'
            'print("OpenOCD programming output", file=sys.stderr)\n'
            'if failure == "always" or (failure == "once" and not previous): sys.exit(1)\n'
            'commands = [sys.argv[i + 1] for i, value in enumerate(sys.argv) if value == "-c"]\n'
            'args = [os.environ["REAL_OPENOCD"], "-c", "noinit",\n'
            '        "-c", "gdb_port disabled", "-c", "tcl_port disabled",\n'
            '        "-c", "telnet_port disabled", "-f", str(log.parent / "probe.tcl")]\n'
            'for command in commands: args.extend(["-c", command])\n'
            'sys.exit(subprocess.run(args).returncode)\n'
        )
        mock.chmod(0o755)
        return dict(os.environ, PATH=f"{bindir}:{os.environ['PATH']}", OPENOCD_LOG=str(log),
                    REAL_OPENOCD=real_openocd, OPENOCD_FLASH=str(flash),
                    OPENOCD_EVENTS=str(root / "probe.events"))

    def run_command(self, command, root, env):
        """Run a fixture command and retain its output for useful failure messages."""
        result = subprocess.run(command, cwd=root, env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def program_commands(self, call):
        """Extract the actual Tcl commands passed as individual OpenOCD arguments."""
        return [call[index + 1] for index, argument in enumerate(call) if argument == "-c"]

    def probe_programs(self, root):
        """Return actual simulated writes, excluding checks that skipped programming."""
        return [event.removeprefix("program ")
                for event in (root / "probe.events").read_text().splitlines()
                if event.startswith("program ")]

    def test_program_swd_and_retry(self):
        """Verify both writes precede the final reset and a retry repeats both writes."""
        with tempfile.TemporaryDirectory(prefix="program test ") as directory:
            root = Path(directory)
            env = self.prepare_probe(root)
            bitstream = root / "gateware.bin"
            bitstream.write_bytes(b"test bitstream")
            firmware = root / "firmware.elf"
            firmware.write_bytes(b"test firmware")
            image = root / "fpga.flash.bin"
            command = ["cmake", f"-DFIRMWARE_ELF={firmware}", f"-DFPGA_BITSTREAM={bitstream}",
                       f"-DFPGA_IMAGE={image}",
                       "-P", str(root / "cmake/Program.cmake")]
            result = self.run_command(command, root, dict(env, OPENOCD_FAIL="once"))
            warning = "Note: If the SD-card contains 'firmware.uf2', the bootloader will replace SWD-programmed images on reset."
            self.assertTrue(result.stderr.rstrip().endswith(warning))
            self.assertLess(result.stderr.index("recovery succeeded"), result.stderr.index(warning))
            self.assertLess(result.stderr.rindex("OpenOCD programming output"), result.stderr.index(warning))
            log = root / "openocd.log"
            calls = [json.loads(line) for line in log.read_text().splitlines()]
            self.assertEqual(len(calls), 2)
            address = hex(FPGA_ADDRESS)
            for call in calls:
                commands = self.program_commands(call)
                self.assertIn(f"verify_image_checksum {{{image}}} {address} bin", commands[-5])
                self.assertIn(f"program {{{image}}} {address} verify", commands[-5])
                self.assertIn(f"verify_image_checksum {{{firmware}}}", commands[-4])
                self.assertEqual(commands[-3:], ["poll off", "reset run", "shutdown"])
            self.assertEqual((root / "probe.events").read_text().splitlines(),
                             ["reset init", "verify fpga.flash.bin", "program fpga.flash.bin",
                              "verify firmware.elf", "program firmware.elf", "reset run"])
            self.assertIn("adapter speed 20833", calls[0])
            self.assertIn("adapter speed 5000", calls[1])
            self.assertIn("cmsis-dap quirk enable", calls[1])
            self.assertEqual(image.read_bytes(), IMAGE.build_image(b"test bitstream", 737280))

            log.unlink()
            failed = subprocess.run(command, env=dict(env, OPENOCD_FAIL="always"), capture_output=True)
            self.assertNotEqual(failed.returncode, 0)
            self.assertNotIn(warning.encode(), failed.stderr)
            self.assertEqual(len(log.read_text().splitlines()), 2)

            # No selected input failure may reach the probe, even if another input is valid.
            for path, content in ((bitstream, b""), (bitstream, b"x" * (16 * 1024 * 1024)),
                                  (firmware, b"")):
                with self.subTest(path=path, size=len(content)):
                    bitstream.write_bytes(b"test bitstream")
                    firmware.write_bytes(b"test firmware")
                    path.write_bytes(content)
                    log.unlink(missing_ok=True)
                    failed = subprocess.run(command, env=env, capture_output=True)
                    self.assertNotEqual(failed.returncode, 0)
                    self.assertFalse(log.exists(), "invalid input must not reach OpenOCD")
            firmware.unlink()
            failed = subprocess.run(command, env=env, capture_output=True)
            self.assertNotEqual(failed.returncode, 0)
            self.assertFalse(log.exists())

    def prepare_subprojects(self, root, env):
        """Use the real root graph with small artifact builders instead of hardware toolchains."""
        for name in ("CMakeLists.txt", "CMakePresets.json"):
            shutil.copyfile(ROOT / name, root / name)
        (root / "build-artifact.cmake").write_text(
            '# SPDX-License-Identifier: CC0-1.0\n'
            'if(DEFINED ROM AND NOT EXISTS "${ROM}")\n'
            '  message(FATAL_ERROR "Firmware built before its menu ROM")\n'
            'endif()\n'
            'get_filename_component(directory "${OUTPUT}" DIRECTORY)\n'
            'file(MAKE_DIRECTORY "${directory}")\n'
            'file(COPY_FILE "${INPUT}" "${OUTPUT}")\n'
            'file(APPEND "${CMAKE_CURRENT_LIST_DIR}/build.log" "${KIND}\\n")\n'
        )
        for name, target, output in (("rom", "menu", "bin/menu.rom"),
                                     ("fw", "firmware.elf", "src/firmware.elf"),
                                     ("gw", "fpga_bitstream", "outflow/EconoPET.hex.bin")):
            source = root / name
            source.mkdir()
            (source / "input.bin").write_bytes(name.encode())
            rom_argument = '"-DROM=${ECONOPET_ROM_BIN_DIR}/menu.rom"' if name == "fw" else ""
            (source / "CMakeLists.txt").write_text(
                '# SPDX-License-Identifier: CC0-1.0\n'
                'cmake_minimum_required(VERSION 3.21)\n'
                f'project(fake_{name} NONE)\n'
                f'add_custom_target({target} ALL\n'
                '  COMMAND "${CMAKE_COMMAND}" '
                f'"-DKIND={name}" "-DOUTPUT=${{CMAKE_BINARY_DIR}}/{output}" '
                f'"-DINPUT=${{CMAKE_SOURCE_DIR}}/input.bin" {rom_argument} '
                '-P "${CMAKE_SOURCE_DIR}/../build-artifact.cmake")\n'
            )
        simulator = root / "gw/simulator"
        simulator.mkdir()
        (simulator / "CMakeLists.txt").write_text(
            '# SPDX-License-Identifier: CC0-1.0\n'
            'cmake_minimum_required(VERSION 3.21)\n'
            'project(fake_models NONE)\n'
            'add_custom_target(gateware_models)\n'
        )
        bootloader = root / "fw/external/pico-sdcard-bootloader"
        bootloader.mkdir(parents=True)
        (bootloader / "input.bin").write_bytes(b"boot")
        (bootloader / "CMakeLists.txt").write_text(
            '# SPDX-License-Identifier: CC0-1.0\n'
            'cmake_minimum_required(VERSION 3.21)\n'
            'project(fake_boot NONE)\n'
            'add_custom_target(bootloader ALL\n'
            '  COMMAND "${CMAKE_COMMAND}" "-DKIND=boot" '
            '"-DOUTPUT=${CMAKE_BINARY_DIR}/src/boot3/bootloader.uf2" '
            '"-DINPUT=${CMAKE_SOURCE_DIR}/input.bin" '
            '-P "${CMAKE_SOURCE_DIR}/../../../build-artifact.cmake"\n'
            '  COMMAND "${CMAKE_COMMAND}" -E copy '
            '"${CMAKE_BINARY_DIR}/src/boot3/bootloader.uf2" '
            '"${CMAKE_BINARY_DIR}/src/boot3/bootloader.elf")\n'
        )
        toolchain = root / "sdk/cmake/preload/toolchains/pico_arm_cortex_m0plus_gcc.cmake"
        toolchain.parent.mkdir(parents=True)
        toolchain.write_text("# SPDX-License-Identifier: CC0-1.0\n")
        env["PICO_SDK_PATH"] = str(root / "sdk")
        self.run_command(
            ["cmake", "--preset", "default", f"-DECONOPET_MEDIA_DIR={root / 'media'}",
             "-DBUILD_TESTS=OFF", "-DBUILD_SYSTEM_TESTS=OFF", "-DBUILD_SDCARD=OFF",
             "-DBUILD_PRGS=OFF"], root, env)

    def test_boot_preset_and_all_refresh_bootloader(self):
        """Build only the bootloader with boot, and include it in repeated all builds."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            env = self.prepare_probe(root)
            self.prepare_subprojects(root, env)
            command = ["cmake", "--build", "--preset", "boot"]
            self.run_command(command, root, env)
            self.assertEqual((root / "build.log").read_text().splitlines(), ["boot"])
            image = root / "build/boot/src/boot3/bootloader.uf2"
            self.assertEqual(image.read_bytes(), b"boot")

            # An incremental all build must refresh the bootloader without reconfiguration.
            source = root / "fw/external/pico-sdcard-bootloader/input.bin"
            source.write_bytes(b"updated boot")
            self.run_command(["cmake", "--build", "--preset", "all"], root, env)
            self.assertEqual(image.read_bytes(), b"updated boot")
            self.assertFalse((root / "openocd.log").exists())

    def test_program_presets_build_selected_prerequisites(self):
        """Exercise fresh builds, ROM ordering, image selection, and repeated programming."""
        for preset, selected in (("prog-boot", {"boot"}), ("prog-fw", {"rom", "fw"}),
                                  ("prog-gw", {"gw"}),
                                  ("prog", {"boot", "rom", "fw", "gw"})):
            with self.subTest(preset=preset), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                env = self.prepare_probe(root)
                self.prepare_subprojects(root, env)
                command = ["cmake", "--build", "--preset", preset, "--parallel", "4"]
                result = self.run_command(command, root, env)
                warning = "Note: If the SD-card contains 'firmware.uf2', the bootloader will replace SWD-programmed images on reset."
                self.assertTrue(result.stderr.rstrip().endswith(warning), result.stdout + result.stderr)
                self.assertLess(result.stderr.rindex("OpenOCD programming output"), result.stderr.index(warning))
                built = (root / "build.log").read_text().splitlines()
                self.assertEqual(set(built), selected)
                if "fw" in selected:
                    self.assertLess(built.index("rom"), built.index("fw"))
                log = root / "openocd.log"
                calls = [json.loads(line) for line in log.read_text().splitlines()]
                self.assertEqual(len(calls), 1)
                commands = self.program_commands(calls[0])
                writes = self.probe_programs(root)
                self.assertEqual(len(writes), len(selected - {"rom"}))
                self.assertEqual(commands[-2:], ["reset run", "shutdown"])
                self.assertEqual(any("firmware.elf" in item for item in writes), "fw" in selected)
                self.assertEqual(any("fpga.flash.bin" in item for item in writes), "gw" in selected)
                self.assertEqual(any("bootloader.elf" in item for item in writes), "boot" in selected)
                if preset == "prog":
                    self.assertEqual(writes, ["fpga.flash.bin", "bootloader.elf", "firmware.elf"])

                # New source bytes must reach the second invocation without reconfiguration.
                for name in selected:
                    source = root / ("fw/external/pico-sdcard-bootloader" if name == "boot" else name)
                    (source / "input.bin").write_bytes(b"updated " + name.encode())
                self.run_command(command, root, env)
                self.assertEqual(len(log.read_text().splitlines()), 2)
                if "fw" in selected:
                    self.assertEqual((root / "build/fw/src/firmware.elf").read_bytes(), b"updated fw")
                if "gw" in selected:
                    self.assertEqual((root / "build/gw/fpga.flash.bin").read_bytes(),
                                     IMAGE.build_image(b"updated gw", 737280))
                if "boot" in selected:
                    self.assertEqual((root / "build/boot/src/boot3/bootloader.elf").read_bytes(),
                                     b"updated boot")

                # Build-only and default targets must never touch the probe.
                log.unlink()
                self.run_command(["cmake", "--build", "--preset", "all"], root, env)
                self.run_command(["cmake", "--build", "build"], root, env)
                self.assertFalse(log.exists())

                # A prerequisite failure must stop before even attempting programming.
                source = ("fw/external/pico-sdcard-bootloader" if "boot" in selected
                          else "fw" if "fw" in selected else "gw")
                (root / source / "input.bin").unlink()
                failed = subprocess.run(command, cwd=root, env=env, capture_output=True)
                self.assertNotEqual(failed.returncode, 0)
                self.assertFalse(log.exists())

                # Combined programming requires all three image builders to be enabled.
                if preset == "prog":
                    self.run_command(["cmake", "--preset", "default", "-DBUILD_BOOTLOADER=OFF"],
                                     root, env)
                    failed = subprocess.run(command, cwd=root, env=env, capture_output=True)
                    self.assertNotEqual(failed.returncode, 0)
                    self.assertFalse(log.exists())

    def test_skip_matching_flash_and_restore_erased_firmware(self):
        """Skip matching board contents, detect corruption and recover bootloader erasure."""
        with tempfile.TemporaryDirectory(prefix="program test ") as directory:
            root = Path(directory)
            env = self.prepare_probe(root)
            self.prepare_subprojects(root, env)
            command = ["cmake", "--build", "--preset", "prog"]
            self.run_command(command, root, env)
            events = root / "probe.events"
            self.assertEqual(self.probe_programs(root),
                             ["fpga.flash.bin", "bootloader.elf", "firmware.elf"])

            # An unchanged board skips every write but still resets and runs.
            events.write_text("")
            self.run_command(command, root, env)
            self.assertEqual(events.read_text().splitlines(),
                             ["reset init", "verify fpga.flash.bin", "verify bootloader.elf",
                              "verify firmware.elf", "reset run"])

            # Board-side corruption must trigger a write even when the input is unchanged.
            events.write_text("")
            (root / "flash/fpga.flash.bin").write_bytes(b"corrupted")
            self.run_command(command, root, env)
            self.assertEqual(self.probe_programs(root), ["fpga.flash.bin"])

            # A partial bootloader write erases vectors, so retry must restore unchanged firmware.
            events.write_text("")
            firmware = (root / "build/fw/src/firmware.elf").read_bytes()
            (root / "fw/external/pico-sdcard-bootloader/input.bin").write_bytes(b"new boot")
            self.run_command(command, root, dict(env, OPENOCD_FAIL="after-boot-once"))
            self.assertEqual(self.probe_programs(root), ["bootloader.elf", "firmware.elf"])
            self.assertEqual((root / "flash/firmware.elf").read_bytes(), firmware)
            self.assertEqual(events.read_text().splitlines().count("reset init"), 2)
            self.assertEqual(events.read_text().splitlines().count("reset run"), 1)


if __name__ == "__main__":
    unittest.main()
