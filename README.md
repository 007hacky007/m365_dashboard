Fork notes:  
Display changed from SPI to I2C in defines.h  
Removed RuSSian language, because fuck RuSSia 
The pressed brake flag threshold was set too high and could not enter the menu because of this - fixed  
I used Nano instead of Pro Mini, but i had to cut the RX/TX legs of the CH340, otherwise it didn't work. Nano has 3.3V output, you can power the OLED from there  
If you don't want to install old version of Arduino IDE, download the hex files from releases (no bootloader included). You can burn the hex files to Arduino using Avrdudess and e.g.USBtinyISP. If it's not working, in Avrdudess' options you can set the exe and the conf to the provided files  
A nice 3D printed case here https://www.thingiverse.com/thing:3646680

# Xiaomi M365 Display [This project is no longer maintained, because I don't have a platform to test it.]

I will be pushing pull requests, if you improve this in any way possible feel free to create a push request.

<img src="https://user-images.githubusercontent.com/5514002/56957966-22138500-6b49-11e9-8e42-26d0758b6d00.jpg" width="200" height="264" />

# Products Used  
Arduino Pro Mini    
I2C OLED 0.96" or 1.3" Screen
FTD1232 Usb Programmer   
3d Printed Bracket  
1N4148 Diode  
0.25w 120ohm Resistor       

Estimated price is around 20$ (Inluding Printed Parts).

Knowing the price is around 20$ you can get the Xiaomi M365 Pro top panel from aliexpress for around the same price and a better look, unless you need additional features just go for the pro display.

# Flashing  
![alt text](https://i.imgur.com/DpPkvJz.jpg)  
Please install the libraries I provided in the files, install them to you arduino library folder, usually              
  C:\Users\\%username%\Documents\Arduino\libraries  
I'd recommend you to use Arduino 1.6.6  
https://www.arduino.cc/en/Main/OldSoftwareReleases  

## What you need to install/configure in Arduino IDE 2.x

If you prefer using a current IDE instead of the old 1.6.6, this sketch builds fine on Arduino IDE 2.x (ATmega328P and ESP32) with the following setup:

- Boards
  - Install “Arduino AVR Boards” from Boards Manager.
  - Select the board you actually use:
    - Arduino Pro Mini (ATmega328P, pick 3.3V/8 MHz or 5V/16 MHz as appropriate), or
    - Arduino Nano (ATmega328P). Many Nanos need “Processor: ATmega328P (Old Bootloader)”.
  - Optional: ESP32 (install the "esp32" core by Espressif from Boards Manager). Any common ESP32 devkit should work.

- Libraries
  - SSD1306Ascii (by Bill Greiman) via Library Manager, or use the provided ZIP under `libraries/SSD1306Ascii.zip` (Sketch > Include Library > Add .ZIP Library...).
  - WatchDog 1.2.0 via the provided `libraries/WatchDog-1.2.0.zip` (Add .ZIP Library...).
  - EEPROM is part of the AVR core; no separate install needed.

- Open `M365/M365.ino`, select the correct Port, then Verify/Upload.

### ESP32 notes

- UART pins
  - By default the sketch uses UART1 with RX=GPIO16 and TX=GPIO17 for the scooter BUS.
  - To change pins, define these macros before including `defines.h` or add them to the build:
    - `M365_UART_NUM` (default 1), `M365_UART_RX_PIN`, `M365_UART_TX_PIN`.

- EEPROM emulation
  - The sketch handles `EEPROM.begin()`/`commit()` internally; no extra steps are needed.

- Display
  - I2C works on ESP32’s default SDA/SCL (GPIO21/22 on many boards). If you wired differently, set pins before `Wire.begin()` or rewire to defaults.

## Notes for common pitfalls

- OLED type/address
  - Default is I2C SSD1306 at address 0x3C (`DISPLAY_I2C` + `OLED_I2C_ADDRESS 0x3C` in `M365/defines.h`).
  - For 1.3" SH1106 modules, switch the `display.begin` line in code (already present as a commented alternative) and make sure the I2C address matches your module.

- SPI vs I2C
  - The fork defaults to I2C. If you want SPI, uncomment `DISPLAY_SPI` and wire the pins defined in `defines.h` (`PIN_CS/PIN_DC/PIN_RST/...`). Only enable one of I2C or SPI at a time.

- Nano “Old Bootloader”
  - Many classic Nanos need “ATmega328P (Old Bootloader)” selected; otherwise uploads fail or time out.

- I2C stability
  - The sketch sets Wire timeouts (when supported) and auto-recovers the bus if it gets stuck. If you still see freezes, keep I2C at 100 kHz and avoid long cables.

- Serial BUS half‑duplex
  - On ATmega328P boards the code briefly disables RX during writes to reduce bus noise. On other MCUs those macros are ignored; prefer 328P-based boards for best results.

- Powering the OLED
  - A 0.96" SSD1306 typically runs fine from the Nano’s 3.3V pin. Some 1.3" modules may draw more; use 5V with the module’s onboard regulator if required (check your module specs).

# Physical Connections  

![dashboard](https://github.com/user-attachments/assets/e0b65522-345c-487b-b7f3-a1857144189d)

# Updating M365 firmware / Disabling the Dashboard
Turn on the scooter and immediately engage and hold the throttle and brake before the logo disappears from the dashboard LCD. You will enter on dashboard disabled mode.
The Arduino TX/RX pins will go to hi impedance state leaving the communication BUS free.

By this way you can update de M365 firmware without disconnecting the dashboard or any cable.

A new power cycle will reset the dashboard to normal mode.

# Known Issues  
Sometimes the Arduino Freezes, a watchdog is in place but doesn't always trigger.  

# Screen caps
# Soldering, soldered directly to the cable coming from the MCU
5V To Red    
GND To Black  
BUS To Yellow  
![alt text](https://i.imgur.com/3ZwcrIJ.jpg)  
A video on how everything is soldered may come soon.

Meanwhile you can enable subtitles in English in this YouTube video produced in Spanish language
https://www.youtube.com/watch?v=JQUNXCyj2Fs

# UI
UI pictures from version 0.2  


![alt text](https://i.imgur.com/8ekMdIo.jpg)  
![alt text](https://i.imgur.com/AHLVTcu.jpg)  

More pictures are coming soon.

---

## CI builds

This repo includes a GitHub Actions workflow that compiles:
- Arduino Pro Mini (ATmega328P) 16 MHz and 8 MHz .hex
- ESP32 Dev Module .bin

Artifacts are attached to each workflow run (see the Actions tab > latest run > Artifacts).

## Using CI build artifacts (precompiled binaries)

Pick files from the artifact that matches your target:

- ATmega328P (Arduino Pro Mini/Nano)
  - M365.ino.hex — sketch only (upload via serial bootloader).
  - M365.ino.with_bootloader.hex — bootloader + sketch (program via ISP).
  - M365.ino.elf — symbols/debug only; not for flashing.

- ESP32
  - M365.ino.bin — app image (flash at 0x10000).
  - M365.ino.partitions.bin — partition table (flash at 0x8000).
  - M365.ino.bootloader.bin — bootloader (flash at 0x1000).
  - M365.ino.merged.bin — combined full image (flash at 0x0, if provided).
  - M365.ino.elf — symbols/debug only; not for flashing.

### Flashing ATmega328P from macOS

Serial bootloader (FTDI/CH340; bootloader already present):
1) Find the port:
   ls /dev/tty.usb* /dev/cu.usb* 2>/dev/null
2) Upload the sketch:
   - Old bootloader (common on Pro Mini): 57600 baud
     avrdude -p m328p -c arduino -P /dev/tty.usbserial-XXXX -b 57600 -D -U flash:w:M365.ino.hex:i
   - Optiboot (some boards): 115200 baud
     avrdude -p m328p -c arduino -P /dev/tty.usbserial-XXXX -b 115200 -D -U flash:w:M365.ino.hex:i

ISP programmer (USBasp/USBTinyISP; writes bootloader + sketch):
avrdude -p m328p -c usbasp -U flash:w:M365.ino.with_bootloader.hex:i

Notes:
- If fuses/clock are wrong, use Arduino IDE: Tools > Board “Arduino Pro Mini” (correct 3.3V/8 MHz or 5V/16 MHz), then Tools > Burn Bootloader to set fuses. After that, upload M365.ino.hex via serial.

### Flashing ESP32 from macOS

App-only update (keeps current bootloader/partitions):
esptool.py --chip esp32 --port /dev/tty.usbserial-XXXX --baud 921600 write_flash 0x10000 M365.ino.bin

Full flash:
esptool.py --chip esp32 --port /dev/tty.usbserial-XXXX --baud 921600 write_flash \
  0x1000  M365.ino.bootloader.bin \
  0x8000  M365.ino.partitions.bin \
  0x10000 M365.ino.bin

Single-image (if merged.bin is present):
esptool.py --chip esp32 --port /dev/tty.usbserial-XXXX --baud 921600 write_flash 0x0 M365.ino.merged.bin

Tips:
- Port may be /dev/cu.usbserial-XXXX or /dev/cu.SLAB_USBtoUART depending on your USB-UART chip.
- For ESP32, hold BOOT while tapping EN (RST) to enter download mode on many dev boards.

## Testing without hardware (Wokwi simulator)

This repo includes two ready-to-run Wokwi setups that display the UI with animated, synthetic data. The sketch has a compile-time flag (SIM_MODE) that feeds fake frames so it doesn’t wait for the scooter bus.

- ESP32 DevKit: open the `wokwi-esp32` folder in Wokwi. The SSD1306 is wired to SDA=21, SCL=22 at I2C address 0x3C. The `wokwi.toml` passes `-DSIM_MODE`.
- Arduino Nano: open the `wokwi-nano` folder. The OLED uses A4 (SDA) and A5 (SCL), also with `-DSIM_MODE` enabled.

Notes:
- In SIM mode the sketch skips the hibernation wait and “no data” error, and animates speed/throttle/brake, battery voltage/percent, temps, and timers.
- Production builds are unaffected because SIM_MODE is commented out by default in `M365/defines.h`.
- If you prefer, you can define SIM_MODE via the build flags instead of editing the file.
