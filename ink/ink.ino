#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>                 // Library for SH1106 1.3" OLED
#include <SparkFun_BMI270_Arduino_Library.h> // SparkFun BMI270 Library
#include <nrf_gpio.h>                      // Native nRF low-power wake-up sense
#include <math.h>

// --- Pin Assignments & Constants ---
#define BUTTON_PIN            D1              // TTP223 Touch Input (Active-HIGH)
#define SCREEN_WIDTH          128
#define SCREEN_HEIGHT         64
#define HOLD_TIME_MS          1500            // Hold duration (ms) for power off
#define INACTIVITY_TIMEOUT_MS (10 * 60 * 1000UL) // 10 minutes auto-sleep timeout
#define DISPLAY_UPDATE_MS     500             // 2 Hz non-blocking UI update rate
#define MOTION_ANGLE_RESET    5.0f            // Reset 10-min countdown on 5° tilt change

// Master 7x8 Crisp Solid Triangle Bitmap
const uint8_t PROGMEM triangle_bmp[8] = {
  0x10, // ...#...
  0x10, // ...#...
  0x38, // ..###..
  0x38, // ..###..
  0x7C, // .#####.
  0x7C, // .#####.
  0xFE, // #######
  0xFE  // #######
};

// Initialize 1.3" SH1106 Display & SparkFun BMI270 Sensor
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
BMI270 imu;

// Baseline Normalized Gravity Unit Vector at Boot/Tare (RAM only)
float u0x = 0.0f;
float u0y = 1.0f;
float u0z = 0.0f;

// Raw Accelerometer readings
float axRaw = 0.0f;
float ayRaw = 0.0f;
float azRaw = 1.0f;

// Button state & Activity tracking
unsigned long pressStartTime   = 0;
unsigned long lastActivityTime  = 0;
unsigned long lastDisplayUpdate = 0;
bool isPressed                 = false;

// Function prototypes
void enterPowerOff();
void handleChargingOnlyMode();
void calibrateBootZero();

// --- USB Power / Charging Detect ---
bool isCharging() {
#if defined(NRF_POWER) && defined(POWER_USBREGSTATUS_VBUSDETECT_Msk)
  return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
#else
  return false;
#endif
}

// --- Smooth Battery Monitor ---
float getRawBatteryVoltage() {
  pinMode(VBAT_ENABLE, OUTPUT);
  digitalWrite(VBAT_ENABLE, LOW);
  delayMicroseconds(500);

  uint32_t sumADC = 0;
  for (int i = 0; i < 16; i++) {
    sumADC += analogRead(PIN_VBAT);
    delayMicroseconds(50);
  }
  digitalWrite(VBAT_ENABLE, HIGH);

  float avgADC = (float)sumADC / 16.0f;
  return avgADC * (3.6f / 1024.0f) * (1510.0f / 510.0f);
}

float getSmoothBatteryVoltage() {
  static float filteredVbat = -1.0f;
  float currentVbat = getRawBatteryVoltage();

  if (filteredVbat < 0.0f) {
    filteredVbat = currentVbat;
  } else {
    filteredVbat = (filteredVbat * 0.95f) + (currentVbat * 0.05f);
  }
  return filteredVbat;
}

int getBatteryPercentage(float voltage) {
  int pct = map((int)(voltage * 100.0f), 330, 415, 0, 100);
  return constrain(pct, 0, 100);
}

void drawBatteryIcon(int percentage, bool charging, float vbat) {
  int batX = 104;
  int batY = 1;
  int batW = 20;
  int batH = 9;

  // Outer Battery Shell
  display.drawRect(batX, batY, batW, batH, SH110X_WHITE);
  display.drawRect(batX + batW, batY + 2, 2, 5, SH110X_WHITE);

  // Render Percentage Text directly left of battery icon
  char pctBuf[8];
  snprintf(pctBuf, sizeof(pctBuf), "%d%%", percentage);
  int pctW = strlen(pctBuf) * 6;
  int pctX = batX - 3 - pctW;

  display.setTextSize(1);
  display.setCursor(pctX, batY);
  display.print(pctBuf);

  int iconX = pctX - 8;
  bool fullyCharged = charging && (vbat >= 4.16f || percentage >= 98);

  if (fullyCharged) {
    display.fillRect(batX + 2, batY + 2, batW - 4, batH - 4, SH110X_WHITE);

    // Draw '✓' Checkmark Icon
    display.drawLine(iconX, batY + 4, iconX + 2, batY + 6, SH110X_WHITE);
    display.drawLine(iconX + 2, batY + 6, iconX + 5, batY + 1, SH110X_WHITE);
  } else if (charging) {
    static int animFrame = 0;
    animFrame = (animFrame + 1) % 4;
    int animWidth = map(animFrame + 1, 1, 4, 3, batW - 4);

    display.fillRect(batX + 2, batY + 2, animWidth, batH - 4, SH110X_WHITE);

    // Charge '+' Icon
    display.fillRect(iconX, batY + 3, 5, 1, SH110X_WHITE);
    display.fillRect(iconX + 2, batY + 1, 1, 5, SH110X_WHITE);
  } else {
    int barWidth = map(percentage, 0, 100, 0, batW - 4);
    if (barWidth > 0) {
      display.fillRect(batX + 2, batY + 2, barWidth, batH - 4, SH110X_WHITE);
    }
  }
}

// Render perfectly symmetric 7x8 triangles (UP = normal, DOWN = vertically inverted)
void drawTriangleIcon(int x, int y, int dir) {
  if (dir == 0) return;

  for (int row = 0; row < 8; row++) {
    uint8_t rowByte = pgm_read_byte(&triangle_bmp[dir > 0 ? row : (7 - row)]);
    for (int col = 0; col < 7; col++) {
      if (rowByte & (0x80 >> col)) {
        display.drawPixel(x + col, y + row, SH110X_WHITE);
      }
    }
  }
}

// Calibrates boot position in 3D unit vector space (0.00 degree zero reference)
void calibrateBootZero() {
  delay(100);

  float sumX = 0.0f, sumY = 0.0f, sumZ = 0.0f;
  const int numSamples = 20;

  for (int i = 0; i < numSamples; i++) {
    imu.getSensorData();
    sumX += imu.data.accelX;
    sumY += imu.data.accelY;
    sumZ += imu.data.accelZ;
    delay(10);
  }

  float norm = sqrt(sumX * sumX + sumY * sumY + sumZ * sumZ);
  if (norm > 0.0001f) {
    u0x = sumX / norm;
    u0y = sumY / norm;
    u0z = sumZ / norm;
  }
}

// Dedicated screen loop when USB power is active while device is turned off
void handleChargingOnlyMode() {
  if (!isCharging()) return;

  display.setRotation(0);
  display.oled_command(SH110X_DISPLAYON);
  display.oled_command(0x81);
  display.oled_command(0x20);

  while (isCharging()) {
    if (digitalRead(BUTTON_PIN) == HIGH) {
      while (digitalRead(BUTTON_PIN) == HIGH) delay(10);
      delay(100);
      display.oled_command(0x81);
      display.oled_command(0x80);
      return;
    }

    float vbat = getSmoothBatteryVoltage();
    int batPct = getBatteryPercentage(vbat);
    bool full = (vbat >= 4.16f || batPct >= 98);

    display.clearDisplay();

    int bw = 50, bh = 24, tw = 4, th = 10;
    int bx = (SCREEN_WIDTH - (bw + tw)) / 2;
    int by = 12;

    display.drawRect(bx, by, bw, bh, SH110X_WHITE);
    display.drawRect(bx + bw, by + (bh - th) / 2, tw, th, SH110X_WHITE);

    if (full) {
      display.fillRect(bx + 3, by + 3, bw - 6, bh - 6, SH110X_WHITE);
    } else {
      static int anim = 0;
      anim = (anim + 1) % 5;
      int fillW = map(anim, 0, 4, 0, bw - 6);
      if (fillW > 0) {
        display.fillRect(bx + 3, by + 3, fillW, bh - 6, SH110X_WHITE);
      }
    }

    char chgText[24];
    if (full) {
      snprintf(chgText, sizeof(chgText), "FULLY CHARGED 100%%");
    } else {
      snprintf(chgText, sizeof(chgText), "CHARGING %d%%", batPct);
    }

    int textW = strlen(chgText) * 6;
    int textX = (SCREEN_WIDTH - textW) / 2;

    display.setTextSize(1);
    display.setCursor(textX, 44);
    display.print(chgText);

    display.display();
    delay(500);
  }

  display.clearDisplay();
  display.display();
  display.oled_command(SH110X_DISPLAYOFF);

#if defined(ARDUINO_ARCH_NRF52840) || defined(NRF52_SERIES)
  #if defined(digitalPinToPinName)
    nrf_gpio_cfg_sense_input(digitalPinToPinName(BUTTON_PIN), NRF_GPIO_PIN_PULLDOWN, NRF_GPIO_PIN_SENSE_HIGH);
  #elif defined(g_ADigitalPinMap)
    nrf_gpio_cfg_sense_input(g_ADigitalPinMap[BUTTON_PIN], NRF_GPIO_PIN_PULLDOWN, NRF_GPIO_PIN_SENSE_HIGH);
  #else
    nrf_gpio_cfg_sense_input(BUTTON_PIN, NRF_GPIO_PIN_PULLDOWN, NRF_GPIO_PIN_SENSE_HIGH);
  #endif
#endif

  NRF_POWER->SYSTEMOFF = 1;
}

void enterPowerOff() {
  display.setRotation(0);
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(10, 24);
  display.print("POWER OFF");
  display.display();

  while (digitalRead(BUTTON_PIN) == HIGH) {
    delay(10);
  }
  delay(100);

  if (isCharging()) {
    handleChargingOnlyMode();
    return;
  }

  display.clearDisplay();
  display.display();
  display.oled_command(SH110X_DISPLAYOFF);

#if defined(ARDUINO_ARCH_NRF52840) || defined(NRF52_SERIES)
  #if defined(digitalPinToPinName)
    nrf_gpio_cfg_sense_input(digitalPinToPinName(BUTTON_PIN), NRF_GPIO_PIN_PULLDOWN, NRF_GPIO_PIN_SENSE_HIGH);
  #elif defined(g_ADigitalPinMap)
    nrf_gpio_cfg_sense_input(g_ADigitalPinMap[BUTTON_PIN], NRF_GPIO_PIN_PULLDOWN, NRF_GPIO_PIN_SENSE_HIGH);
  #else
    nrf_gpio_cfg_sense_input(BUTTON_PIN, NRF_GPIO_PIN_PULLDOWN, NRF_GPIO_PIN_SENSE_HIGH);
  #endif
#endif

  NRF_POWER->SYSTEMOFF = 1;
}

// Continuous non-blocking touch input processing
void handleButton() {
  bool currentReading = (digitalRead(BUTTON_PIN) == HIGH);

  if (currentReading) {
    lastActivityTime = millis(); // Resets timer on touch tap/hold
  }

  if (currentReading && !isPressed) {
    isPressed = true;
    pressStartTime = millis();
  }
  else if (currentReading && isPressed) {
    if (millis() - pressStartTime >= HOLD_TIME_MS) {
      enterPowerOff();
      isPressed = false;
    }
  }
  else if (!currentReading && isPressed) {
    unsigned long pressDuration = millis() - pressStartTime;
    isPressed = false;

    // Tap (>20ms, <1.5s): Zero baseline unit vector in RAM
    if (pressDuration >= 20 && pressDuration < HOLD_TIME_MS) {
      float sumX = 0.0f, sumY = 0.0f, sumZ = 0.0f;
      for (int i = 0; i < 10; i++) {
        imu.getSensorData();
        sumX += imu.data.accelX;
        sumY += imu.data.accelY;
        sumZ += imu.data.accelZ;
        delayMicroseconds(200);
      }
      float norm = sqrt(sumX * sumX + sumY * sumY + sumZ * sumZ);
      if (norm > 0.0001f) {
        u0x = sumX / norm;
        u0y = sumY / norm;
        u0z = sumZ / norm;
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLDOWN);

  Wire.begin();

  if (!display.begin(0x3C, true)) {
    Serial.println("OLED Init Failed!");
  }

  uint8_t bmiStatus = imu.beginI2C(0x68, Wire);
  if (bmiStatus != BMI2_OK) {
    bmiStatus = imu.beginI2C(0x69, Wire);
  }

  if (bmiStatus != BMI2_OK) {
    Serial.println("BMI270 Init Failed!");
  }

  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);

  // If USB is connected on boot, show charging screen first
  handleChargingOnlyMode();

  // Calibrate current resting position as 0.00 degree baseline
  calibrateBootZero();

  lastActivityTime = millis();
}

void loop() {
  handleButton();

  unsigned long currentMillis = millis();
  if (currentMillis - lastDisplayUpdate >= DISPLAY_UPDATE_MS) {
    lastDisplayUpdate = currentMillis;

    // 10x Over-sampling to eliminate electrical noise
    float sumX = 0.0f, sumY = 0.0f, sumZ = 0.0f;
    for (int i = 0; i < 10; i++) {
      imu.getSensorData();
      sumX += imu.data.accelX;
      sumY += imu.data.accelY;
      sumZ += imu.data.accelZ;
      delayMicroseconds(200);
    }

    axRaw = sumX / 10.0f;
    ayRaw = sumY / 10.0f;
    azRaw = sumZ / 10.0f;

    // Current normalized 3D gravity unit vector
    float norm = sqrt(axRaw * axRaw + ayRaw * ayRaw + azRaw * azRaw);
    float rawAngleDeg = 0.0f;
    float crossZ = 0.0f;

    if (norm > 0.0001f) {
      float ux = axRaw / norm;
      float uy = ayRaw / norm;
      float uz = azRaw / norm;

      float dot = ux * u0x + uy * u0y + uz * u0z;
      float crossX = u0y * uz - u0z * uy;
      float crossY = u0z * ux - u0x * uz;
      crossZ       = u0x * uy - u0y * ux;
      float crossMag = sqrt(crossX * crossX + crossY * crossY + crossZ * crossZ);

      rawAngleDeg = atan2(crossMag, dot) * 180.0f / M_PI;
    }

    // Motion Detection for Inactivity Timeout (5.0° change threshold)
    static float lastActivityAngle = -999.0f;
    if (lastActivityAngle < -900.0f) {
      lastActivityAngle = rawAngleDeg;
    } else if (fabs(rawAngleDeg - lastActivityAngle) >= MOTION_ANGLE_RESET) {
      lastActivityTime = currentMillis;
      lastActivityAngle = rawAngleDeg;
    }

    if (currentMillis - lastActivityTime >= INACTIVITY_TIMEOUT_MS) {
      enterPowerOff();
    }

    // Deadband Hysteresis Filter: Holds 0.01° solid when still, snaps instantly on motion
    static float stableAngle = -999.0f;
    if (stableAngle < -900.0f) {
      stableAngle = rawAngleDeg;
    } else {
      float diff = rawAngleDeg - stableAngle;
      if (fabs(diff) > 0.05f) {
        stableAngle = rawAngleDeg; // Instant snap on deliberate tilt
      } else if (fabs(diff) > 0.015f) {
        stableAngle += diff * 0.4f; // Smooth transition on gentle movement
      }
    }

    float displayAngle = stableAngle;
    bool flipDisplay = false;

    // Strict 0.00° to 90.00° range mapping + 180° screen rotation past 90°
    if (displayAngle > 90.0f) {
      displayAngle = 180.0f - displayAngle;
      flipDisplay = true;
    }

    display.setRotation(flipDisplay ? 2 : 0);

    // Roll direction triangles
    int leftDir = 0, rightDir = 0;
    if (crossZ > 0.01f) {
      leftDir = 1;    // Clockwise: Left UP
      rightDir = -1;  // Right DOWN
    } else if (crossZ < -0.01f) {
      leftDir = -1;   // Counter-clockwise: Left DOWN
      rightDir = 1;   // Right UP
    }

    bool chargingNow = isCharging();
    float vbat = getSmoothBatteryVoltage();
    int batPct = getBatteryPercentage(vbat);

    display.clearDisplay();

    drawBatteryIcon(batPct, chargingNow, vbat);

    // Left triangle at x=2, Right triangle at x=119
    drawTriangleIcon(2, 30, leftDir);
    drawTriangleIcon(119, 30, rightDir);

    // Formatted to two decimal places (1/100th degree)
    char tiltBuf[10];
    snprintf(tiltBuf, sizeof(tiltBuf), "%.2f", displayAngle);

    int textWidth = strlen(tiltBuf) * 18; // 18px per char in GFX size 3
    int textStartX = 108 - textWidth;    // Right-aligned anchor at x=108

    display.setTextSize(3);
    display.setCursor(textStartX, 22);
    display.print(tiltBuf);

    // Degree symbol fixed at x=112
    display.drawCircle(112, 24, 2, SH110X_WHITE);

    display.display();
  }

  delay(5);
}
