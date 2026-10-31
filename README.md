Fujitsu AirConditioner Controller for HomeKit
----------
Flash to an [M5Atom](https://shop.m5stack.com/collections/atom-series/products/atom-lite-esp32-development-kit), and add your Fujitsu AirConditioner (Heatpump) to your HomeKit home. Should work with most other ESP32 kits as well.

Changes in this fork (0.3)
--------------------------

Tested on a plain ESP32 DevKit (38-pin) with an MCP2021A transceiver and a Fujitsu ARYG18LLTB ducted unit.

* No HomeKit notification flood: characteristics are only updated when their value actually changes
* No slider bounce: a change made in Home.app is held until the unit confirms it on the bus (or 4 s pass)
* Stale or late writes are dropped instead of being sent to the unit seconds later
* Current state shows Inactive when the unit is off; Fan/Dry show Idle
* Wi-Fi updates (HomeSpan OTA, password protected, `safeLoad`) with an automatic rollback: a new image is only marked good after 2 minutes of uptime with Wi-Fi, and 3 crash reboots in a row switch back to the previous slot
* Web log on port 8080, behind HTTP digest auth (user `admin`, password set with the `@W <password>` serial command)
* FujiHeatPump is vendored in `lib/` (with the fixes above) instead of pulled from git
* Board pins for a DevKit: control button on GPIO0 (BOOT), status LED on GPIO2

Wi-Fi update: set the OTA password once with the `O` serial command, set `upload_port` in `platformio.ini` to the board's IP, then run `FUJI_OTA_PWD=<password> pio run -e ota -t upload`.

Do not erase the whole flash when re-flashing over USB: NVS holds the HomeKit pairing and Wi-Fi settings.

Quick start
-----------

* Download the latest release
* Download [NodeMCU PyFlasher](https://github.com/marcelstoer/nodemcu-pyflasher)
* Plug in your [M5Atom](https://shop.m5stack.com/collections/atom-series/products/atom-lite-esp32-development-kit)
* Launch NodeMCU PyFlasher
  * Set your serial port
  * Select the downloaded .bin file as your NodeMCU firmware
  * Select 'Dual I/O (DIO)' as Flash mode
  * Erase flash to 'yes'
* Click 'Flash NodeMCU'

You can either use the built in SoftAP mode to configure the WiFi details and pairing key:
* Hold down the button for 3 seconds until the LED flashes cyan quickly
* Press the button 3 times, the LED should triple-blink
* Hold the button for 3 seconds again to enter AP configuration mode
* Connect to the 'Fujitsu Airconditioner XXXXXX' AP that should now be available and enter your WiFi details and setup code

Or the serial monitor:
* Open Serial Monitor
* Enter 'S <HOMEKIT PIN>' to define the HomeKit pairing pin (eg 'S 11122333')
* Enter 'W' to enter WiFi connection details

Connect the unit:
* Connect the M5Atom to the AirConditioner via the 3-wire interface (see example circuit) or via the com connector on the indoor unit (will need a logic level converter)
* Power-Cycle your air conditioner. 
  * You may need to toggle the dip switch on the Primary controller so that it 'forgets' its setup.


Secondary Mode
--------------

By default the controller binds to the indoor unit as a secondary controller. If you do not have an existing wall remote controller, you will need to edit FujiHeatPump initialisation to bind as a primary controller.

QR Code
-------

You can generate a QR code to scan with Home.app by using the [QR code generator tool](https://github.com/maximkulkin/esp-homekit/tree/master/tools)

```#  python3 gen_qrcode 21 111-22-333 FUJI output.png```

Alternatively, you can use an online QR code generator such as [the-qrcode-generator.com](https://www.the-qrcode-generator.com/), just enter the generated setup payload which is printed after you enter your custom pairing pin (eg `X-HM://00KUG4GWTFUJI`)

Example Circuit
---------------
<img src="https://github.com/unreality/FujiHK/blob/master/example-circuit.png"/>

Other ESP Boards
----------------

This controller should work fine on other ESP boards, with the following setup:
- GPIO 19 - LIN RX
- GPIO 22 - LIN TX 
- GPIO 27 - NeoPixel Compatible LED (SK6812/WS2812C/etc)

Parts Required
--------------
- LIN Tranceiver
  - MCP-2025-330 (or similar)
  - http://ww1.microchip.com/downloads/en/DeviceDoc/20002306B.pdf
- 10uF Capacitor
- 10k Resistor
- 12v -> 3.3v (500mA+) voltage regulator
  - https://www.pololu.com/product/2842

