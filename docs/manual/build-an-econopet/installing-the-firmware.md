# Installing the firmware

Normally, firmware upgrades are done by placing the firmware files on a microSD card. However, the initial install (or recovering from a corrupted bootloader) requires restoring the EconoPET bootloader over USB.

The flow is:

1. Initialize a microSD card with the firmware files.
2. Restore the bootloader via USB (one time, or as-needed if it gets overwritten).
3. After the bootloader is restored, the EconoPET will automatically finish the firmware update from the microSD card.

## Initialize the microSD card

The EconoPET uses a microSD card to configure the system at power on and reset. To prepare a microSD card:

1. Download and unzip the latest firmware from the EconoPET site: <https://dlehenbauer.github.io/econopet/40-8096-A.html>
2. Format the microSD card with the FAT32 or exFAT filesystem.
3. Copy the unzipped contents of the firmware archive to the root of the microSD card.
4. Insert the microSD card into the EconoPET

If you have trouble formatting the card, try the SD Memory Card Formatter tool from the SD Association: <https://www.sdcard.org/downloads/>

> Note: the microSD card socket uses a push-to-eject mechanism. Do not remove the card by pulling.

## Install the bootloader

To avoid powering the CBM/PET without firmware installed (which can cause a "bright spot" on the CRT), do the initial bootloader restore with the board outside the machine using an external 9V supply.

1. Remove the EconoPET from the CBM/PET (if installed).
2. Insert the prepared microSD card.
3. Connect a USB cable from the EconoPET USB-C port to your computer.
4. Enter flash mode:
   - Press and hold the FLASH button on the EconoPET.
   - While holding FLASH, apply 9V power to the EconoPET (use the DC jack or the 2-pin power breakout).
   - After a couple of seconds, release the FLASH button.
5. The EconoPET should appear as a USB mass storage device named RPI-RP2.
6. Copy the bootloader:
   - bootloader.uf2 is included in the same firmware .zip used to initialize the microSD card.
   - Drag and drop bootloader.uf2 into the RPI-RP2 drive.
7. After the file is copied, the EconoPET will automatically reboot and finish the firmware update from the microSD card.
   - The green LED will blink rapidly during this process.
   - When complete, you should hear a short 4-note "success" beep from the speaker.

After this one-time step, future firmware updates are normally done by updating the files on the microSD card.
