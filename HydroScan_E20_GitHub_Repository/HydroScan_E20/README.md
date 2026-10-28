# HydroScan-E20 — ESP32 Prototype

HydroScan-E20 is a student prototype exploring capacitive sensing and live telemetry for water-related changes in E20 ethanol-blended petrol. It combines an ESP32, ProtoCentral FDC2214 capacitive sensing board, DS18B20 temperature probe, analog turbidity module, OLED, MAX7219 LED matrix, buzzer, and a Wi-Fi-hosted dashboard.

## Project status and limitations

## Project Status and Limitations

- **Successful capacitive sensing demonstration:** The FDC2214 registered measurable changes during the cumulative water-addition experiment. The recorded average decreased from **12,239,751.42 counts for dry E20 to 12,215,497.90 counts after adding 1.0 mL of water**, demonstrating that the capacitive sensing system responded to changes in the sample conditions.
- **Capacitance response:** The decrease in FDC2214 output is consistent with a change in the effective capacitance of the sensing arrangement as the sample's dielectric properties changed. The exact contribution of bulk capacitance and other experimental factors requires further controlled investigation.
- **Calibration limitations:** Although the readings decreased overall between the dry baseline and the 1.0 mL endpoint, intermediate readings were non-monotonic. The available data do not establish a reliable continuous water-percentage calibration or a validated contamination threshold.
- **Supplementary turbidity sensing:** The recorded turbidity readings for dry E20 and E20 with 1.0 mL of added water overlapped substantially. Consequently, the available measurements did not demonstrate clear discrimination between these two conditions using turbidity alone.
- **Alert functionality:** The firmware includes a provisional sensor-threshold alert intended to flag readings that cross the configured reference boundary. This provides a preliminary indication of a sensor response, but it is not a validated determination of water contamination.
- **Phase separation:** The available experimental records do not establish that visible phase separation occurred after adding 1.0 mL of water. The sensor response should not be interpreted as proof of phase separation.
- **System demonstration:** HydroScan-E20 integrates the FDC2214 capacitive sensor, ESP32-based data acquisition, temperature and turbidity measurements, local display indicators, and a live web dashboard. The project demonstrates the integration of these components into a functional educational prototype.
- **Safety and intended use:** HydroScan-E20 is an educational proof-of-concept, not a certified fuel-quality instrument. Ordinary hobby electronics must not be operated near open petrol/E20 or fuel vapour. Testing with real fuel requires an approved laboratory procedure and appropriate safety controls.


## Repository contents

- `HydroScan_E20.ino` — ESP32 firmware.
- `data/index.html` — dashboard HTML.
- `data/style.css` — dashboard styles.
- `data/app.js` — dashboard behavior and API requests.

## Hardware pin map

- I²C SDA: GPIO 21
- I²C SCL: GPIO 22
- DS18B20 data: GPIO 4
- Turbidity analog output: GPIO 34 (verify the output never exceeds the ESP32 ADC input limit of 3.3 V)
- MAX7219 DIN: GPIO 23
- MAX7219 CLK: GPIO 18
- MAX7219 CS: GPIO 5
- Buzzer driver: GPIO 27 through the transistor driver stage

Verify this mapping against the actual wiring before powering the prototype.

## Arduino IDE setup

1. Install Arduino IDE and ESP32 board support by Espressif Systems.
2. Open `HydroScan_E20.ino` from this folder.
3. Install the libraries used by the sketch: ProtoCentral FDC2214 library, OneWire, DallasTemperature, Adafruit GFX Library, and Adafruit SSD1306. Library names and compatibility can vary; use the library required by the include statements in the source.
4. Select the appropriate ESP32 board and COM/serial port.
5. Compile and upload the sketch.
6. Upload this project's `data` folder to the ESP32 LittleFS filesystem using an uploader compatible with your Arduino IDE and ESP32 core version.
7. Open Serial Monitor at 115200 baud and check startup messages.
8. Connect to the `HydroScan-E20` access point and open `http://192.168.4.1/`.
9. Check the JSON endpoint at `http://192.168.4.1/data`.

## Dashboard and alert behavior

- Guest dashboard: `http://192.168.4.1/`
- Live JSON endpoint: `http://192.168.4.1/data`
- No manual Trigger or Clear alert button is included in this version.
- The firmware can raise a provisional sensor-window alert after the analog voltage remains in the configured window for the required consecutive readings. The current window is exploratory and does not validate detection of water in E20 or confirm phase separation.

## Calibration data

The report contains the recorded cumulative-addition readings and discusses their limitations. Preserve the original measurements. Do not edit measured values to create a smoother-looking graph; collect additional independent trials if a stronger calibration is needed.

## Team contributions

- Abhay Patil — hardware and firmware.
- Arjun Jagadish — firmware and CAD design, (including PETG capacitive-probe holder and enclosure.)
- Sai Satya Sujal Sabbavarapu — dashboard and documentation.
- All team members — calibration and presentation collaboratively.

## Missing project links

- Demonstration video: not recorded at the time this README was prepared.
- Repository code version: this is the initial repository upload; add a release/tag after testing the exact committed version on the hardware.


## Preliminary FDC threshold alert (important limitation)

The firmware now sets `fdcWaterSuspectedAlert` when the rolling 32-sample FDC2214 average is at or below **12,219,725.41 counts** for three consecutive FDC updates. This threshold is the upper edge of the recorded 1.0 mL-added endpoint range. The alert clears after three consecutive readings return to the observed dry-E20 reference band (12,236,587.19–12,243,777.50 counts). The OLED/dashboard label is `WATER CONTAMINATION SUSPECTED*`.

This is a preliminary screening rule only. The recorded intermediate calibration points are non-monotonic, and several intermediate readings also fall below the threshold. It cannot establish a specific water concentration or confirm phase separation. Validate with repeated runs and an independent reference method before making detection-performance claims.
