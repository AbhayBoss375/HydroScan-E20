/*
  HydroScan-E20 — integrated student prototype firmware
  ESP32 DevKit V1 (30-pin)

  What this sketch does:
    - Reads FDC2214 CH0 raw signal over I2C
    - Reads DS18B20 temperature (non-blocking conversion)
    - Reads turbidity module AOUT as raw ESP32 ADC counts and calibrated mV
    - Uses preliminary air-vs-tap-water voltage readings for a demo alert threshold
    - Displays a clearly marked demo alert on the OLED and a crossed-circle matrix icon
    - Sounds the buzzer only when voltage is in a provisional tap-water-like band
      that is distinct from the recorded air and E20 readings
    - Hosts the dashboard over the ESP32's own Wi-Fi AP
    - Serves /data JSON from the same ESP32
    - Logs a single 32-valid-sample FDC moving average per second to Serial Monitor
      with manual condition labels for baseline and cumulative water additions

  IMPORTANT:
    - Shows a DEMO nominal added-water estimate by interpolating the E20-only and
      1.0 mL-added endpoint means. This is NOT a validated water concentration.
    - Intermediate calibration means are non-monotonic, so intermediate percentages
      are provisional and must not be treated as quantitatively accurate.
    - Values outside the recorded endpoint sample range show OUT OF RANGE.
    - Calibration stage labels record nominal water added, not independently verified
      total water concentration; initial water content may be unknown.
    - Turbidity ADC counts are NOT NTU and do not prove the probe is connected.
    - Air-vs-tap-water calibration only creates a DEMO trigger. It does not prove
      that a water-rich phase has separated in E20 fuel.
    - Do not put ordinary, un-rated electronics/probes into petrol or flammable vapour.
    - Confirm the turbidity module AOUT is <= 3.3 V before connecting to ESP32 GPIO34.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Wire.h>
#include <Protocentral_FDC2214.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------------- Wi-Fi dashboard ----------------
const char *AP_SSID = "HydroScan-E20";
const char *AP_PASS = "hydroscan123";
WebServer server(80);
bool filesystemReady = false;

// ---------------- Pin map ----------------
constexpr int I2C_SDA_PIN = 21;
constexpr int I2C_SCL_PIN = 22;
constexpr int ONE_WIRE_PIN = 4;
constexpr int TURBIDITY_PIN = 34;  // ADC1 input-only pin; works while Wi-Fi is on
constexpr int MATRIX_DIN_PIN = 23;
constexpr int MATRIX_CLK_PIN = 18;
constexpr int MATRIX_CS_PIN  = 5;
constexpr int BUZZER_PIN = 27;     // drives 2N2222 base through 4.7k resistor

// ---------------- OLED (SSD1306, 128 x 64, I2C address 0x3C) ----------------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool oledOK = false;

// ---------------- MAX7219 8x8 matrix ----------------
// Drive the MAX7219 directly so this ESP32 project does not depend on the
// AVR-only LedControl library/header. Wiring remains DIN=23, CLK=18, CS=5.
bool matrixOK = false;

// ---------------- FDC2214 + calibration averaging ----------------
#define FDC_CHANNEL FDC2214_CH0
FDC2214 fdc;
bool fdcReady = false;
bool fdcValueValid = false;
uint32_t fdcRaw = 0;
unsigned long lastFdcReadMs = 0;
constexpr unsigned long FDC_SAMPLE_INTERVAL_MS = 50;  // collect about 20 samples/s
constexpr size_t FDC_AVERAGE_WINDOW = 32;
uint32_t fdcSamples[FDC_AVERAGE_WINDOW] = {};
size_t fdcSampleIndex = 0;
size_t fdcSampleCount = 0;
uint64_t fdcSampleSum = 0;

// Recorded mean FDC_avg32 calibration points from the user's files.
// Units are FDC2214 raw counts (32-sample average), not pF.
// Added water was cumulative into 100 mL starting E20; these are nominal additions.
// 0.0 mL: 12239751.42   0.1 mL: 12252021.27   0.2 mL: 12232220.07
// 0.3 mL: 12222311.34   0.4 mL: 12222351.53   0.5 mL: 12222776.56
// 0.6 mL: 12225179.62   0.7 mL: 12221553.93   0.8 mL: 12223311.95
// 0.9 mL: 12216577.66   1.0 mL: 12215497.90
// Crucial: intermediate points are non-monotonic/overlapping. Thus only the dry
// and 1.0 mL endpoints are used as anchors for a DEMO estimate; this is not a
// validated true water % calibration and must not be used for vehicle-fuel safety.
constexpr double FDC_CAL_DRY_MEAN = 12239751.42;  // E20 only (nominal 0.0% added water)
constexpr double FDC_CAL_1ML_MEAN = 12215497.90;  // 1.0 mL added to 100 mL E20 (nominal ~1.0%)
// Actual min/max of the recorded 32-average values in the two endpoint files.
constexpr double FDC_DRY_OBS_MIN = 12236587.19;
constexpr double FDC_DRY_OBS_MAX = 12243777.50;
constexpr double FDC_1ML_OBS_MIN = 12211612.47;
constexpr double FDC_1ML_OBS_MAX = 12219725.41;
constexpr double FDC_ACCEPT_MIN = FDC_1ML_OBS_MIN;
constexpr double FDC_ACCEPT_MAX = FDC_DRY_OBS_MAX;
bool fdcAverageReady();
double fdcAverage32();
bool fdcWithinObservedRange() {
  if (!fdcAverageReady() || !fdcValueValid) return false;
  const double v = fdcAverage32();
  return v >= FDC_ACCEPT_MIN && v <= FDC_ACCEPT_MAX;
}

// Gives a nominal endpoint label only. The endpoint bands reflect the min/max
// FDC_avg32 values observed in the dry-E20 and 1.0 mL-added files. The intermediate
// data is non-monotonic, so this deliberately DOES NOT invent a percentage between
// endpoints. Values between the two endpoint bands remain uncalibrated (null).
bool getNominalWaterEstimatePercent(double &percent) {
  if (!fdcAverageReady() || !fdcValueValid || !fdcWithinObservedRange()) return false;
  const double v = fdcAverage32();
  if (v >= FDC_DRY_OBS_MIN && v <= FDC_DRY_OBS_MAX) {
    percent = 0.0;
    return true;
  }
  if (v >= FDC_1ML_OBS_MIN && v <= FDC_1ML_OBS_MAX) {
    percent = 1.0;
    return true;
  }
  return false;  // between endpoint bands: not enough evidence for a % estimate
}

// Calibration logging is deliberately manual: select a condition only after the
// operator has prepared that sample and it has settled. This labels data; it does
// not certify the actual total water concentration of the fuel.
int calibrationStage = -1;  // -1 = not started; 0 = E20 only; 1..10 = cumulative 0.1..1.0 mL additions
float calibrationWaterAddedMl = 0.0f;
uint32_t calibrationRowNumber = 0;
unsigned long lastCalibrationPrintMs = 0;
constexpr unsigned long CALIBRATION_PRINT_INTERVAL_MS = 1000;

// ---------------- DS18B20 ----------------
OneWire oneWire(ONE_WIRE_PIN);
DallasTemperature tempSensors(&oneWire);
bool temperatureOK = false;
float temperatureC = 0.0f;
bool temperatureConversionPending = false;
unsigned long lastTemperatureRequestMs = 0;
constexpr unsigned long TEMP_CONVERSION_WAIT_MS = 800;
constexpr unsigned long TEMP_REQUEST_INTERVAL_MS = 2000;

// ---------------- Turbidity analog input ----------------
int turbidityRaw = 0;
int turbidityMilliVolts = 0;
bool turbidityReadStarted = false;
unsigned long lastTurbidityReadMs = 0;

// Calibration evidence from the user's files/screenshots:
// - E20 only: voltage mean ~2.1277 V, observed range 2.116-2.144 V
// - E20 + cumulative water additions up to 1.0 mL: readings stayed ~2.11-2.14 V
// - Tap-water screenshot: ~1.800 V
// - Air screenshot: ~1.514 V (an older air text log measured ~1.555-1.569 V)
// A single midpoint threshold was WRONG because clean E20 is also above 1.657 V.
// Use a provisional water-like WINDOW instead. These limits support a demo only:
// they do NOT prove that a real separated water/ethanol layer has this signature.
constexpr int TURBIDITY_WATER_MIN_MV = 1680;  // activate at/above 1.680 V
constexpr int TURBIDITY_WATER_MAX_MV = 1950;  // and at/below 1.950 V
constexpr int TURBIDITY_CLEAR_LOW_MV = 1620;  // clear when back air-like
constexpr int TURBIDITY_CLEAR_HIGH_MV = 2000; // clear when back fuel-like
bool sensorWindowAlert = false; // provisional turbidity-window alert
// FDC reference-threshold screening flag. This is a preliminary indicator, not validated detection.
bool fdcWaterSuspectedAlert = false;
uint8_t fdcLowStreak = 0;
uint8_t fdcDryStreak = 0;
constexpr uint8_t FDC_ALERT_CONFIRM_SAMPLES = 3;
// Upper edge of the recorded 1.0 mL endpoint band: 12,219,725.41 counts.
// If the 32-sample FDC average stays at/below this level, flag a possible water-related change.
// Intermediate calibration points are non-monotonic, so this threshold is NOT selective for 1.0 mL.
constexpr double FDC_WATER_SUSPECT_THRESHOLD = FDC_1ML_OBS_MAX;
uint8_t waterLikeStreak = 0;
uint8_t clearStreak = 0;
unsigned long lastAlarmBeepMs = 0;
unsigned long buzzerTurnOffAtMs = 0;
bool alarmBuzzerOn = false;
constexpr unsigned long ALARM_BEEP_INTERVAL_MS = 1200;
constexpr unsigned long ALARM_BEEP_DURATION_MS = 100;

bool anyAlertActive() { return sensorWindowAlert || fdcWaterSuspectedAlert; }

// Evaluate once per new FDC sample. Three consecutive averages are required to
// avoid reacting to a single transient. Clear only after three averages return
// to the observed dry-E20 band. This remains an experimental screening rule.
void updateFdcWaterAlert() {
  if (!fdcAverageReady() || !fdcValueValid) return;
  const double v = fdcAverage32();
  if (!fdcWaterSuspectedAlert) {
    fdcDryStreak = 0;
    if (v <= FDC_WATER_SUSPECT_THRESHOLD) {
      if (fdcLowStreak < FDC_ALERT_CONFIRM_SAMPLES) ++fdcLowStreak;
      if (fdcLowStreak >= FDC_ALERT_CONFIRM_SAMPLES) {
        fdcWaterSuspectedAlert = true;
        lastAlarmBeepMs = millis() - ALARM_BEEP_INTERVAL_MS;
        Serial.println(F("FDC ALERT: reading is at/below recorded 1.0 mL endpoint band. Possible water-related change; threshold is preliminary and not validated."));
      }
    } else {
      fdcLowStreak = 0;
    }
  } else {
    fdcLowStreak = 0;
    if (v >= FDC_DRY_OBS_MIN && v <= FDC_DRY_OBS_MAX) {
      if (fdcDryStreak < FDC_ALERT_CONFIRM_SAMPLES) ++fdcDryStreak;
      if (fdcDryStreak >= FDC_ALERT_CONFIRM_SAMPLES) {
        fdcWaterSuspectedAlert = false;
        fdcDryStreak = 0;
        Serial.println(F("FDC INFO: reading returned to observed dry-E20 reference band; FDC alert cleared."));
      }
    } else {
      fdcDryStreak = 0;
    }
  }
}



// ---------------- UI timing ----------------
unsigned long lastOledUpdateMs = 0;
unsigned long lastMatrixUpdateMs = 0;
unsigned long lastHistoryUpdateMs = 0;

// ---------------- DSA requirements for the project evaluation ----------------
// Classes + inheritance: all sensor modules share a common interface. The derived
// classes below call the real hardware-reading functions used by the firmware.
class SensorModule {
public:
  virtual void read() = 0;
  virtual const char* name() const = 0;
  virtual ~SensorModule() {}
};

// Linked list: keeps a short rolling history of capacitive raw readings for the
// dashboard API. Each new sample is appended; the oldest is removed at capacity.
struct ReadingNode {
  uint32_t value;
  unsigned long timeMs;
  ReadingNode* next;
  ReadingNode(uint32_t v, unsigned long t) : value(v), timeMs(t), next(nullptr) {}
};
ReadingNode* historyHead = nullptr;
ReadingNode* historyTail = nullptr;
size_t historyCount = 0;
const size_t HISTORY_LIMIT = 12;

void removeOldestHistoryNode() {
  if (!historyHead) return;
  ReadingNode* old = historyHead;
  historyHead = historyHead->next;
  if (!historyHead) historyTail = nullptr;
  delete old;
  historyCount--;
}

void appendHistory(uint32_t value) {
  ReadingNode* node = new ReadingNode(value, millis());
  if (!node) return;
  if (historyTail) historyTail->next = node;
  else historyHead = node;
  historyTail = node;
  historyCount++;
  while (historyCount > HISTORY_LIMIT) removeOldestHistoryNode();
}

// Recursion: takes eight ADC samples recursively and returns their sum. This is
// used by the actual turbidity-reading path, not just a demonstration function.
uint32_t readAdcSumRecursive(int remaining) {
  if (remaining <= 0) return 0;  // recursion base case
  uint32_t current = (uint32_t)analogRead(TURBIDITY_PIN);
  delayMicroseconds(150);
  return current + readAdcSumRecursive(remaining - 1);
}

// 8x8 bitmap rows, bit 7 is the left-most LED on typical MAX7219 modules.
const byte ICON_CALIBRATION[8] = {
  B00011000,
  B00100100,
  B01000010,
  B01011010,
  B01011010,
  B01000010,
  B00100100,
  B00011000
};
const byte ICON_WARNING[8] = {
  B00011000,
  B00011000,
  B00011000,
  B00011000,
  B00011000,
  B00000000,
  B00011000,
  B00011000
};
const byte ICON_OFFLINE[8] = {
  B10000001,
  B01000010,
  B00100100,
  B00011000,
  B00011000,
  B00100100,
  B01000010,
  B10000001
};
// Crossed-circle icon = invalid/demo-alert state. Displayed steadily during the alert.
const byte ICON_INVALID[8] = {
  B00111100,
  B01000010,
  B10011001,
  B10100101,
  B10100101,
  B10011001,
  B01000010,
  B00111100
};

void matrixWriteRegister(byte reg, byte value) {
  digitalWrite(MATRIX_CS_PIN, LOW);
  shiftOut(MATRIX_DIN_PIN, MATRIX_CLK_PIN, MSBFIRST, reg);
  shiftOut(MATRIX_DIN_PIN, MATRIX_CLK_PIN, MSBFIRST, value);
  digitalWrite(MATRIX_CS_PIN, HIGH);
}

void initMatrix() {
  pinMode(MATRIX_DIN_PIN, OUTPUT);
  pinMode(MATRIX_CLK_PIN, OUTPUT);
  pinMode(MATRIX_CS_PIN, OUTPUT);
  digitalWrite(MATRIX_CS_PIN, HIGH);
  matrixWriteRegister(0x0F, 0x00); // Display test off
  matrixWriteRegister(0x0C, 0x00); // Shutdown while configuring
  matrixWriteRegister(0x0B, 0x07); // Scan all eight rows
  matrixWriteRegister(0x09, 0x00); // No BCD decode
  matrixWriteRegister(0x0A, 0x03); // Low brightness
  for (byte row = 1; row <= 8; row++) matrixWriteRegister(row, 0x00);
  matrixWriteRegister(0x0C, 0x01); // Normal operation
  matrixOK = true;
}

void showMatrixIcon(const byte icon[8]) {
  if (!matrixOK) return;
  for (byte row = 0; row < 8; row++) {
    matrixWriteRegister(row + 1, icon[row]);
  }
}

void buzzerBeep(unsigned int durationMs) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(durationMs);
  digitalWrite(BUZZER_PIN, LOW);
}

// The OLED intentionally shows only the four requested readings, all at once.
// Water percentage remains blank (--) until calibration is performed. FDC2214
// output is shown as raw sensor code, not falsely labelled as pF capacitance.
void drawOledReadings() {
  if (!oledOK) return;

  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);

  oled.setCursor(0, 0);
  oled.print(F("HydroScan-E20"));

  oled.setCursor(0, 8);
  double nominalWaterPct = 0.0;
  if (getNominalWaterEstimatePercent(nominalWaterPct)) {
    oled.print(F("Water est: "));
    oled.print(nominalWaterPct, 1);
    oled.print(F("%*"));
  } else if (!fdcWithinObservedRange()) {
    oled.print(F("Water est: OUT OF RNG"));
  } else {
    oled.print(F("Water est: N/A*"));
  }

  oled.setCursor(0, 16);
  if (fdcWaterSuspectedAlert) oled.print(F("WATER SUSPECTED*"));
  else if (sensorWindowAlert) oled.print(F("ALERT: TURB WINDOW*"));
  else oled.print(F("Alert: Not active"));

  oled.setCursor(0, 24);
  if (fdcWaterSuspectedAlert) oled.print(F("Status: FDC threshold"));
  else if (sensorWindowAlert) oled.print(F("Status: Turb window"));
  else oled.print(F("Status: Monitoring"));

  oled.setCursor(0, 32);
  oled.print(F("FDC32: "));
  if (fdcAverageReady() && fdcValueValid) oled.print((unsigned long)(fdcAverage32() + 0.5));
  else oled.print(F("--"));

  oled.setCursor(0, 40);
  oled.print(F("Turb: "));
  if (turbidityReadStarted) {
    oled.print(turbidityMilliVolts / 1000.0f, 3);
    oled.print(F(" V"));
  } else oled.print(F("-- V"));

  oled.setCursor(0, 48);
  oled.print(F("Temp: "));
  if (temperatureOK) {
    oled.print(temperatureC, 1);
    oled.print(F(" C"));
  } else {
    oled.print(F("-- C"));
  }

  oled.setCursor(0, 56);
  oled.print(F("*Endpoint demo only"));

  oled.display();
}

void resetFdcAverageWindow() {
  fdcSampleIndex = 0;
  fdcSampleCount = 0;
  fdcSampleSum = 0;
  for (size_t i = 0; i < FDC_AVERAGE_WINDOW; ++i) fdcSamples[i] = 0;
}

bool fdcAverageReady() {
  return fdcSampleCount == FDC_AVERAGE_WINDOW;
}

double fdcAverage32() {
  if (fdcSampleCount == 0) return 0.0;
  return static_cast<double>(fdcSampleSum) / static_cast<double>(fdcSampleCount);
}

const char* calibrationConditionName(int stage) {
  switch (stage) {
    case 0: return "E20_BASELINE";
    case 1: return "ADD_WATER_0.1ML";
    case 2: return "ADD_WATER_0.2ML";
    case 3: return "ADD_WATER_0.3ML";
    case 4: return "ADD_WATER_0.4ML";
    case 5: return "ADD_WATER_0.5ML";
    case 6: return "ADD_WATER_0.6ML";
    case 7: return "ADD_WATER_0.7ML";
    case 8: return "ADD_WATER_0.8ML";
    case 9: return "ADD_WATER_0.9ML";
    case 10: return "ADD_WATER_1.0ML";
    default: return "NOT_SET";
  }
}

void selectCalibrationStage(int stage) {
  if (stage < 0 || stage > 10) return;
  calibrationStage = stage;
  calibrationWaterAddedMl = stage * 0.1f;
  calibrationRowNumber = 0;
  lastCalibrationPrintMs = millis();
  resetFdcAverageWindow();  // don't mix readings from the previous sample condition
  Serial.print(F("# Calibration condition: "));
  Serial.print(calibrationConditionName(calibrationStage));
  Serial.print(F("; cumulative water added = "));
  Serial.print(calibrationWaterAddedMl, 1);
  Serial.println(F(" mL"));
  Serial.println(F("# Waiting for 32 new valid FDC readings before logging rows."));
}

void handleCalibrationSerialCommands() {
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
    if (c == 'b' || c == 'B') {
      selectCalibrationStage(0);
    } else if (c >= '1' && c <= '9') {
      selectCalibrationStage(c - '0');
    } else if (c == '0') {
      selectCalibrationStage(10);  // convenient single-key label for 1.0 mL
    } else if (c == 'h' || c == 'H') {
      Serial.println(F("condition,water_added_ml,sample_no,t_ms,fdc_avg32,temp_C"));
    } else {
      Serial.println(F("# Command: b=E20 baseline, 1..9=0.1..0.9 mL, 0=1.0 mL cumulative added water, h=CSV header"));
    }
  }
}

void printCalibrationHeader() {
  Serial.println(F("condition,water_added_ml,sample_no,t_ms,fdc_avg32,temp_C"));
}

void printCalibrationRow() {
  const unsigned long now = millis();
  if (calibrationStage < 0 || !fdcReady || !fdcValueValid || !fdcAverageReady()) return;
  if (now - lastCalibrationPrintMs < CALIBRATION_PRINT_INTERVAL_MS) return;
  lastCalibrationPrintMs = now;
  ++calibrationRowNumber;

  Serial.print(calibrationConditionName(calibrationStage));
  Serial.print(',');
  Serial.print(calibrationWaterAddedMl, 1);
  Serial.print(',');
  Serial.print(calibrationRowNumber);
  Serial.print(',');
  Serial.print(now);
  Serial.print(',');
  Serial.print(fdcAverage32(), 1);
  Serial.print(',');
  if (temperatureOK) Serial.println(temperatureC, 2); else Serial.println(F("NaN"));
}

void printLiveSerialRow() {
  static unsigned long lastLivePrint = 0;
  const unsigned long now = millis();
  if (now - lastLivePrint < 1000) return;
  lastLivePrint = now;

  Serial.print(F("LIVE,"));
  if (fdcValueValid) Serial.print(fdcRaw); else Serial.print(F("NaN"));
  Serial.print(',');
  if (fdcAverageReady()) Serial.print(fdcAverage32(), 2); else Serial.print(F("NaN"));
  Serial.print(',');
  if (fdcWithinObservedRange()) Serial.print(F("IN_OBSERVED_RANGE"));
  else Serial.print(F("OUT_OF_RANGE_OR_SAMPLING"));
  Serial.print(',');
  if (turbidityReadStarted) Serial.print(turbidityRaw); else Serial.print(F("NaN"));
  Serial.print(',');
  if (turbidityReadStarted) Serial.print(turbidityMilliVolts); else Serial.print(F("NaN"));
  Serial.print(',');
  if (turbidityReadStarted) Serial.print(turbidityMilliVolts / 1000.0f, 3); else Serial.print(F("NaN"));
  Serial.print(',');
  Serial.print(fdcWaterSuspectedAlert ? F("FDC_WATER_SUSPECTED") : (sensorWindowAlert ? F("TURBIDITY_WINDOW_ALERT") : F("NORMAL")));
  Serial.print(',');
  if (temperatureOK) Serial.print(temperatureC, 2); else Serial.print(F("NaN"));
  Serial.print(',');
  Serial.println(now);
}

void updateFDC() {
  const unsigned long now = millis();
  if (now - lastFdcReadMs < FDC_SAMPLE_INTERVAL_MS) return;
  lastFdcReadMs = now;

  if (!fdcReady) {
    fdcValueValid = false;
    return;
  }

  uint32_t readValue = fdc.readRawChannel(FDC_CHANNEL);
  fdcRaw = readValue;
  // The driver has no separate read-error flag; zero is treated as invalid.
  fdcValueValid = (readValue != 0);
  if (!fdcValueValid) return;

  // Maintain the moving average of the most recent 32 valid sensor readings.
  if (fdcSampleCount == FDC_AVERAGE_WINDOW) {
    fdcSampleSum -= fdcSamples[fdcSampleIndex];
  } else {
    ++fdcSampleCount;
  }
  fdcSamples[fdcSampleIndex] = readValue;
  fdcSampleSum += readValue;
  fdcSampleIndex = (fdcSampleIndex + 1) % FDC_AVERAGE_WINDOW;
  updateFdcWaterAlert();
}

void updateTemperature() {
  const unsigned long now = millis();

  if (temperatureConversionPending && now - lastTemperatureRequestMs >= TEMP_CONVERSION_WAIT_MS) {
    float reading = tempSensors.getTempCByIndex(0);
    if (reading != DEVICE_DISCONNECTED_C && reading > -55.0f && reading < 125.0f) {
      temperatureC = reading;
      temperatureOK = true;
    } else {
      temperatureOK = false;
    }
    temperatureConversionPending = false;
  }

  if (!temperatureConversionPending && now - lastTemperatureRequestMs >= TEMP_REQUEST_INTERVAL_MS) {
    tempSensors.requestTemperatures();
    lastTemperatureRequestMs = now;
    temperatureConversionPending = true;
  }
}

void updateTurbidity() {
  const unsigned long now = millis();
  if (now - lastTurbidityReadMs < 250) return;
  lastTurbidityReadMs = now;

  // Average raw ADC counts and ESP32-calibrated millivolts over 8 samples.
  uint32_t rawSum = readAdcSumRecursive(8);
  uint32_t milliVoltSum = 0;
  for (int i = 0; i < 8; ++i) {
    milliVoltSum += (uint32_t)analogReadMilliVolts(TURBIDITY_PIN);
    delayMicroseconds(150);
  }
  turbidityRaw = (int)(rawSum / 8);
  turbidityMilliVolts = (int)(milliVoltSum / 8);
  turbidityReadStarted = true;

  // Require three consecutive readings before changing alert state.
  if (!sensorWindowAlert) {
    clearStreak = 0;
    const bool isWaterLike =
        turbidityMilliVolts >= TURBIDITY_WATER_MIN_MV &&
        turbidityMilliVolts <= TURBIDITY_WATER_MAX_MV;
    if (isWaterLike) {
      if (waterLikeStreak < 3) ++waterLikeStreak;
      if (waterLikeStreak >= 3) {
        sensorWindowAlert = true;
        lastAlarmBeepMs = now - ALARM_BEEP_INTERVAL_MS;  // start first beep promptly
        Serial.println(F("ALERT: voltage is in provisional tap-water-like window (DEMO ONLY; not proof of fuel phase separation)."));
      }
    } else {
      waterLikeStreak = 0;
    }
  } else {
    waterLikeStreak = 0;
    const bool returnedToAirOrFuel =
        turbidityMilliVolts <= TURBIDITY_CLEAR_LOW_MV ||
        turbidityMilliVolts >= TURBIDITY_CLEAR_HIGH_MV;
    if (returnedToAirOrFuel) {
      if (clearStreak < 3) ++clearStreak;
      if (clearStreak >= 3) {
        sensorWindowAlert = false;
        clearStreak = 0;
        Serial.println(F("INFO: turbidity voltage returned to air-like or fuel-like range; demo alert cleared."));
      }
    } else {
      clearStreak = 0;
    }
  }
}

void updateAlarmBuzzer() {
  const unsigned long now = millis();
  if (alarmBuzzerOn && (long)(now - buzzerTurnOffAtMs) >= 0) {
    digitalWrite(BUZZER_PIN, LOW);
    alarmBuzzerOn = false;
  }

  if (anyAlertActive() && !alarmBuzzerOn &&
      now - lastAlarmBeepMs >= ALARM_BEEP_INTERVAL_MS) {
    digitalWrite(BUZZER_PIN, HIGH);
    alarmBuzzerOn = true;
    buzzerTurnOffAtMs = now + ALARM_BEEP_DURATION_MS;
    lastAlarmBeepMs = now;
  }

  if (!anyAlertActive() && alarmBuzzerOn) {
    digitalWrite(BUZZER_PIN, LOW);
    alarmBuzzerOn = false;
  }
}

// Derived classes implement the shared SensorModule interface (inheritance).
class CapacitiveSensorModule : public SensorModule {
public:
  void read() override { updateFDC(); }
  const char* name() const override { return "FDC2214"; }
};
class TemperatureSensorModule : public SensorModule {
public:
  void read() override { updateTemperature(); }
  const char* name() const override { return "DS18B20"; }
};
class TurbiditySensorModule : public SensorModule {
public:
  void read() override { updateTurbidity(); }
  const char* name() const override { return "Turbidity ADC"; }
};
CapacitiveSensorModule capacitiveModule;
TemperatureSensorModule temperatureModule;
TurbiditySensorModule turbidityModule;
SensorModule* sensorModules[] = { &capacitiveModule, &temperatureModule, &turbidityModule };
const size_t SENSOR_MODULE_COUNT = sizeof(sensorModules) / sizeof(sensorModules[0]);

String jsonNumberOrNull(bool valid, float value, int decimals) {
  if (!valid) return String("null");
  return String(value, decimals);
}

void handleData() {
  String json;
  json.reserve(400);
  json += "{";
  json += "\"device\":\"HydroScan-E20\",";
  json += "\"demoMode\":true,";
  double nominalWaterPct = 0.0;
  const bool pctAvailable = getNominalWaterEstimatePercent(nominalWaterPct);
  json += "\"waterPercent\":";
  json += pctAvailable ? String(nominalWaterPct, 2) : String("null");
  json += ",";
  json += "\"nominalAddedWaterEstimatePercent\":";
  json += pctAvailable ? String(nominalWaterPct, 2) : String("null");
  json += ",";
  json += "\"measurementNote\":\"Endpoint demonstration only: 0.0% is the observed E20-only band and 1.0% is the observed 1.0 mL-added band; readings between those bands are uncalibrated because intermediate points are non-monotonic; not validated true water concentration\",";
  json += "\"fdcAvg32\":";
  json += fdcAverageReady() ? String(fdcAverage32(), 2) : String("null");
  json += ",";
  json += "\"fdcWithinObservedRange\":";
  json += fdcWithinObservedRange() ? String("true") : String("false");
  json += ",";
  json += "\"temperatureC\":" + jsonNumberOrNull(temperatureOK, temperatureC, 2) + ",";
  json += "\"turbidity\":" + String(turbidityReadStarted ? turbidityRaw : -1) + ",";
  json += "\"turbidityRaw\":" + String(turbidityReadStarted ? turbidityRaw : -1) + ",";
  json += "\"turbidityUnit\":\"raw ADC counts (0-4095), not NTU\",";
  json += "\"turbidityVoltageV\":" + String(turbidityReadStarted ? (turbidityMilliVolts / 1000.0f) : 0.0f, 3) + ",";
  json += "\"turbidityAlertThresholdV\":1.680,";  // legacy field: lower edge of the water-like window
  json += "\"turbidityWaterLikeMinV\":1.680,";
  json += "\"turbidityWaterLikeMaxV\":1.950,";
  json += "\"turbidityClearLowV\":1.620,";
  json += "\"turbidityClearHighV\":2.000,";
  json += "\"sensorWindowAlert\":" + String(sensorWindowAlert ? "true" : "false") + ",";
  json += "\"fdcWaterSuspectedAlert\":" + String(fdcWaterSuspectedAlert ? "true" : "false") + ",";
  json += "\"fdcWaterSuspectThreshold\":" + String(FDC_WATER_SUSPECT_THRESHOLD, 2) + ",";
  json += "\"alertSource\":" + String(fdcWaterSuspectedAlert ? "\"FDC_REFERENCE_THRESHOLD_PRELIMINARY\"" : (sensorWindowAlert ? "\"PROVISIONAL_TURBIDITY_WINDOW\"" : "\"NONE\"")) + ",";
  json += "\"phaseAlertNote\":\"Provisional water-like voltage window based on air, tap-water and E20 baseline readings; not proof of actual fuel phase separation\",";
  json += "\"capacitiveRaw\":";
  json += fdcValueValid ? String(fdcRaw) : String("null");
  json += ",";
  json += "\"status\":" + String(fdcWaterSuspectedAlert ? "\"WATER_CONTAMINATION_SUSPECTED\"" : (sensorWindowAlert ? "\"PROVISIONAL_TURBIDITY_WINDOW\"" : "\"MONITORING\"")) + ",";
  json += "\"fdcOK\":" + String(fdcReady && fdcValueValid ? "true" : "false") + ",";
  json += "\"temperatureOK\":" + String(temperatureOK ? "true" : "false") + ",";
  // ADC readings always return a number; this flag means the ADC has been sampled,
  // not that the turbidity probe is physically connected or correctly calibrated.
  json += "\"turbidityOK\":" + String(turbidityReadStarted ? "true" : "false") + ",";
  json += "\"uptimeSeconds\":" + String(millis() / 1000) + ",";
  json += "\"oledOK\":" + String(oledOK ? "true" : "false") + ",";
  json += "\"matrixOK\":" + String(matrixOK ? "true" : "false") + ",";
  json += "\"buzzerMode\":" + String(anyAlertActive() ? "\"alert active\"" : "\"standby\"") + ",";
  json += "\"capacitiveHistory\":[";
  ReadingNode* node = historyHead;
  bool first = true;
  while (node) {
    if (!first) json += ",";
    json += String(node->value);
    first = false;
    node = node->next;
  }
  json += "]";
  json += "}";

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.send(200, "application/json; charset=utf-8", json);
}

void sendFileOrFallback(const char *path, const char *contentType, const String &fallback = "") {
  if (filesystemReady && LittleFS.exists(path)) {
    File file = LittleFS.open(path, "r");
    if (file) {
      server.streamFile(file, contentType);
      file.close();
      return;
    }
  }
  if (fallback.length() > 0) {
    server.send(200, contentType, fallback);
  } else {
    server.send(404, "text/plain; charset=utf-8", String("File not found: ") + path + ". Upload the /data folder using Upload LittleFS.");
  }
}

void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    const String fallback = F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>HydroScan-E20</title></head><body style='font-family:Arial;padding:24px'><h1>HydroScan-E20</h1><p>Firmware is running, but dashboard files are missing.</p><p>Upload the sketch's data folder with the LittleFS upload command, then refresh.</p><p><a href='/data'>View live JSON</a></p></body></html>");
    sendFileOrFallback("/index.html", "text/html; charset=utf-8", fallback);
  });
  server.on("/style.css", HTTP_GET, []() {
    sendFileOrFallback("/style.css", "text/css; charset=utf-8");
  });
  server.on("/app.js", HTTP_GET, []() {
    sendFileOrFallback("/app.js", "application/javascript; charset=utf-8");
  });
  server.on("/data", HTTP_GET, handleData);
  server.onNotFound([]() {
    server.send(404, "text/plain; charset=utf-8", "Not found. Open / for dashboard or /data for JSON.");
  });
  server.begin();
}

void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.println();
  Serial.println(F("HydroScan-E20 integrated firmware starting..."));

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  buzzerBeep(70);  // quick hardware check only, not a contamination alarm
  delay(70);
  buzzerBeep(70);

  analogReadResolution(12);
  analogSetPinAttenuation(TURBIDITY_PIN, ADC_11db);
  pinMode(TURBIDITY_PIN, INPUT);

  // Initialize the shared I2C bus before starting either I2C device.
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  oledOK = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (oledOK) {
    oled.clearDisplay();
    oled.display();
    drawOledReadings();
    Serial.println(F("OLED: initialized at 0x3C"));
  } else {
    Serial.println(F("OLED: not found at 0x3C (check address/wiring/library)"));
  }

  initMatrix();
  showMatrixIcon(ICON_CALIBRATION);

  fdcReady = fdc.begin();
  if (fdcReady) {
    fdc.configureDefaults();
    fdc.setAutoscan(true, FDC2214_RR_SEQ_ALL);
    fdc.start();
    Serial.println(F("FDC2214: initialized"));
  } else {
    Serial.println(F("FDC2214: NOT FOUND; check 3.3V, GND, SDA 21, SCL 22"));
  }

  tempSensors.begin();
  tempSensors.setWaitForConversion(false);
  tempSensors.requestTemperatures();
  lastTemperatureRequestMs = millis();
  temperatureConversionPending = true;
  if (tempSensors.getDeviceCount() > 0) {
    Serial.print(F("DS18B20 device count: "));
    Serial.println(tempSensors.getDeviceCount());
  } else {
    Serial.println(F("DS18B20: no device detected yet; will retry readings"));
  }

  filesystemReady = LittleFS.begin(false);
  Serial.println(filesystemReady ? F("LittleFS: mounted") : F("LittleFS: mount failed; upload data files / check partition"));

  WiFi.mode(WIFI_AP);
  IPAddress localIP(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(localIP, gateway, subnet);
  bool apStarted = WiFi.softAP(AP_SSID, AP_PASS);
  Serial.println(apStarted ? F("Wi-Fi AP: started") : F("Wi-Fi AP: FAILED"));
  Serial.print(F("SSID: ")); Serial.println(AP_SSID);
  Serial.print(F("Password: ")); Serial.println(AP_PASS);
  Serial.print(F("Dashboard: http://")); Serial.println(WiFi.softAPIP());

  setupWebServer();
  drawOledReadings();
  Serial.println(F("HTTP server ready. Open /data for JSON or / for dashboard."));
  Serial.println(F("Dashboard includes clearly labelled manual presentation demo trigger; never describe it as sensor detection."));
  Serial.println(F("Water estimate: endpoint DEMO classification only (recorded dry E20 band=0.0%; recorded 1.0mL-added band=1.0% nominal added-water estimate)."));
  Serial.println(F("Recorded intermediate FDC calibration means are non-monotonic; do not interpret intermediate display values as validated true water concentration."));
  Serial.println(F("Turbidity DEMO window: 1.680-1.950 V water-like; clears <=1.620 V or >=2.000 V."));
  Serial.println(F("E20 files show ~2.1277 V clean-fuel mean and ~2.107-2.144 V across recorded added-water conditions."));
  Serial.println(F("OLED/matrix/buzzer alert is a tap-water-like demo trigger, NOT confirmation of E20 phase separation."));
  Serial.println(F("Accepted recorded endpoint FDC_avg32 range: 12211612.47 to 12243777.50; outside this range shows OUT OF RANGE."));
  Serial.println(F("Live CSV: LIVE,fdc_raw,fdc_avg32,range_status,turbidity_adc,turbidity_mV,turbidity_V,phase_demo_alert,temp_C,t_ms"));
  Serial.println(F("Calibration commands: b=E20-only baseline; 1..9=cumulative 0.1..0.9 mL; 0=1.0 mL; h=CSV header."));
  Serial.println(F("Select a condition only after preparing it and allowing it to settle under the approved procedure."));
  printCalibrationHeader();
}

void loop() {
  server.handleClient();
  handleCalibrationSerialCommands();
  // Polymorphic calls through the common base-class interface.
  for (size_t i = 0; i < SENSOR_MODULE_COUNT; i++) sensorModules[i]->read();
  printCalibrationRow();
  printLiveSerialRow();

  const unsigned long now = millis();
  if (now - lastHistoryUpdateMs >= 1000) {
    lastHistoryUpdateMs = now;
    if (fdcValueValid) appendHistory(fdcRaw);
  }
  if (now - lastOledUpdateMs >= 500) {
    lastOledUpdateMs = now;
    drawOledReadings();
  }
  updateAlarmBuzzer();

  if (now - lastMatrixUpdateMs >= 1000) {
    lastMatrixUpdateMs = now;
    // Alert icon has priority and remains steady; otherwise blink the normal status icon.
    static bool matrixVisible = true;
    matrixVisible = !matrixVisible;
    if (anyAlertActive()) {
      showMatrixIcon(ICON_INVALID);
    } else if (!matrixVisible) {
      const byte blank[8] = {0,0,0,0,0,0,0,0};
      showMatrixIcon(blank);
    } else if (!fdcReady) showMatrixIcon(ICON_OFFLINE);
    else if (!fdcValueValid || !fdcAverageReady() || !fdcWithinObservedRange()) showMatrixIcon(ICON_WARNING);
    else showMatrixIcon(ICON_CALIBRATION);
  }
}
