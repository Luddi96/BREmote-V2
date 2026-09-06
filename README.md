# BREmote V2 - Open Source eFoil and Esk8 remote 
![Banner](https://github.com/Luddi96/BREmote-V2/blob/main/img/banner.png)

## Features:
* All mechanical parts 3D printed (even the springs)
* Symmetric design
* Sustainable - All external parts can be replaced
* Open Source: 3D Models, Electronics and Software are GPL3.0
* 868/915MHz Link (range dependant on antenna type, position)
* Communication with VESC via UART / CAN
* Low battery alarm (vibration)
* Water ingress alarm integrated in receiver
* Gears / Power Levels / Cruise Control
* Charging and Programming via USB

## Links:

### Videos:

<details>
<summary>Build Logs (<-open)</summary>

* [#1: Frankenstein](https://youtube.com/shorts/WTDl78fUoms)
* [#2: First Demo of V2](https://youtu.be/LEpEsWbisg0)
* [#3: First Working Prototype](https://youtu.be/mQs5-Zjtvak)
* [#4: Videos of V2 in Action](https://youtu.be/PwP42KR0XCg)
* [#5: GPS Demo](https://youtube.com/shorts/FHg6_6AkiIw)
* [#6: NANO Tx](https://youtu.be/5pOXrJwp_Hw)
* [#7: NANO Rx](https://youtu.be/QyAsgHsLu8w)

</details>

* [#9.0: Build Video](https://youtu.be/Fw4YdQWvs6I)
* [#9.3: NANO Build Video](https://youtu.be/5MdmIF84HN0)
* [#9.1: Usage and Updates](https://youtu.be/r6JIZEq3aTU)
* [#9.4: Config Update](https://youtu.be/K6lIZkDvQfY)
* [#9.5: Improved Battery Measurement (LUT)](https://youtu.be/Mp6ousHFNtM)
* [#9.6: GPS Integration](https://youtu.be/24d-lHl9csI)

### Tools:

* [Serial Terminal](https://lbre.de/BREmote/sertest.html)
* [Config Tool](https://lbre.de/BREmote/struct.html)

* [Flash Download Tool](https://dl.espressif.com/public/flash_download_tool.zip)

* [Premade LUTs](https://lbre.de/BREmote/LUT.html)
* [LUT creation tool](https://lbre.de/BREmote/bat_conf.html)
* [LUT decode tool](https://lbre.de/BREmote/bat_decode.html)
* [Per Cell Current Calculator](https://lbre.de/BREmote/per_cell_calc.html)
* [Plot digitizer](https://apps.automeris.io/downloads/WebPlotDigitizer-4.7-win32-x64.zip)

* [Expo Tool](https://lbre.de/BREmote/expo.html)
* [Calibration Factor Tool](https://lbre.de/BREmote/ubat_cal.html)

* [Log viewer (offline)](https://lbre.de/BREmote/logview1-1.html)
* [LogTRC (online)](https://www.logtrc.com/)

## Usage:


## Status/Error Codes:
Tx:
* XX: Remote went into power saver (5 min no connection) -> power off and on again 
* Ex: Remote Errors
* EP: Not paired
* EC: Not Cald
* ESV: Error Config version SPIFFS <> Build
* ESP3: Error init SPIFFS
* ESP4: Error writing SPIFFS
* EHFC: Error HF (LoRa) setting
* EHFI: Error HF (LoRa) init
* EHFP: Error transmit power
* ECH: Error charger

Rx:
* Aux blink:
	* 3x: Error init SPIFFS
	* 2x: Error Config version SPIFFS <> Build
	* 4x: Error writing SPIFFS

* Bind blink:
	* Short Periodic Flash: Not paired
	* Blink: Paired not connected
	* Solid: Connected
	
	* 2: Error transmit power
	* 3: Error HF (LoRa) setting
	* 4: Error HF (LoRa) init

## Inputs at startup:
Tx:
* Left: Calibrate
* Right: Pair
* THR+Left: USB Mode
* THR+Right: Delete Spiffs

Rx:
* Bind pushed: Pair
* Both pushed: Delete config


# Connection Examples:

<details>
<summary>VESC with UART</summary>

![Conn](https://github.com/Luddi96/BREmote-V2/blob/main/img/conn_vesc.PNG)

</details>

<details>
<summary>ESC with BREmote BEC</summary>

![Conn](https://github.com/Luddi96/BREmote-V2/blob/main/img/conn_esc_bbec.PNG)

</details>

<details>
<summary>ESC with own BEC</summary>

![Conn](https://github.com/Luddi96/BREmote-V2/blob/main/img/conn_esc_obec.PNG)

</details>

<details>
<summary>VESC + Servo</summary>

![Conn](https://github.com/Luddi96/BREmote-V2/blob/main/img/conn_vesc_servo.PNG)

</details>

<details>
<summary>ESC + Servo</summary>

![Conn](https://github.com/Luddi96/BREmote-V2/blob/main/img/conn_esc_servo.PNG)

</details>

# Changelog:

## V2.2.x
### 2026-07-04
* Further rf packet collision improvement (RadioLib bug [#1827](https://github.com/jgromes/RadioLib/issues/1827)
### 2026-07-01
* Add rf packet collision detection (up to 5~6 riders close together)
### 2026-07-01
* Add battery % instead of GPS (speed_src 4)
* Add vibration motor
### 2026-06-14
* Merge Nano and "original" SW (Nano with display now)
* Add I2C Mutex to logger software
### 2026-02-12
* Add Nano Mech. and SW files (BETA)
* Add Rx Logger in DEV folder (BETA)
### 2026-01-22
* Fix Serial() pushing voltage to USB magnet pins
* Fix GPS init sequence
### 2025-11-28
* Fix startup buttons not working when no_gear and no_lock active
### 2025-10-23
* Change Wetness Detection Sensitivity and Frequency
### 2025-10-06
* Add emergency battery saver (5min no connection -> Tx goes to low power mode)
### 2025-09-24:
* Changed batconf noload offset to 5x multiplier
### 2025-09-20:
* Release V2.2.: Change SW version and config version (preparation for follow me)
* Add improved battery measurement
* Change display layout (swap battery and signal graph)

## V2.1.x:
### 2025-09-02:
* Move vias next to connector on Bot_Shield
* Add 3D files for Tx-GPS integration
### 2025-08-16:
* Fix Tx: Add timeout to "ECH" message
### 2025-07-18:
* Fix Tx: Incorrect gear/throttle calculation
### 2025-07-10:
* Update Rx: Add ?printBat
### 2025-07-08:
* Update Tx: Add a case where 0 set as tog_block_time disables gearshifiting after initial throttle input
* Fix Rx: Change PWM Pin from open-drain to push-pull 
### 2025-06-05:
* Updated Readme (Add YT link and conn examples)
### 2025-05-26:
* Updated Complete.step (spring assy. missing)
* Added RF settings for US/AU(915MHz) in Tx&Rx
### 2025-05-20: [Release V2.1]
* Initial release


---
# Credits:
Logo uses "watersport" and "Skate" by Adrien Coquet from https://thenounproject.com/. CC BY 3.0.