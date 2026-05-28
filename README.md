# Bad Apple on Makeblocks CyberPi

Bad Apple running on the CyberPi (ESP32-based board by Makeblock). Why? Because i was bored.

## Flashing

1. Clone the repo
2. Flash the `.bin` file:

**Linux:**
`esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 write_flash 0x0 bad-cyberpi.bin`

**Bindoj:**
`esptool.py --chip esp32 --port COM3 --baud 921600 write_flash 0x0 bad-cyberpi.bin`

-> Adjust port (`/dev/ttyUSB0` or `COM3`) and path to the `.bin` file accordingly.

3. To see the lyrics via Serial Monitor:
   - **Linux:** `screen /dev/ttyUSB0 115200`
   - **Bindoj:** PuTTY → Serial → COM3 → 115200 baud
  
-> Adjust port (`/dev/ttyUSB0` or `COM3`) accordingly.

## Editing

To build/edit yourself you'll need:

- **LittleFS** plugin in your Arduino plugins folder
- **PSRAM** enabled
- **Storage** set to 8MB
- CyberPi Arduino Library: https://github.com/Makeblock-official/CyberPi-Library-for-Arduino




## Demo:

[Bad_cyberpi.webm](https://github.com/user-attachments/assets/a7e859ea-23c8-431c-ac7a-cb4a3f91fcfd)
