#include <Arduino.h>
#include <Keypad.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#define KEYPAD_PIN1 13
#define KEYPAD_PIN2 12
#define KEYPAD_PIN3 14
#define KEYPAD_PIN4 27
#define KEYPAD_PIN5 26
#define KEYPAD_PIN6 25
#define KEYPAD_PIN7 33

const byte ROWS = 4;
const byte COLS = 3;
char keys[ROWS][COLS] = {
  {'1','2','3'},
  {'4','5','6'},
  {'7','8','9'},
  {'*','0','#'}
};
byte rowPins[ROWS] = {KEYPAD_PIN2, KEYPAD_PIN7, KEYPAD_PIN6, KEYPAD_PIN4};
byte colPins[COLS] = {KEYPAD_PIN3, KEYPAD_PIN1, KEYPAD_PIN5};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

#define LCD_ADDR 0x27
#define LCD_COLS 16
#define LCD_ROWS 2
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

#define LED_PIN    2
#define BUZZER_PIN 18

const unsigned long BOMB_TIME_MS   = 40000UL;
const unsigned long DEFUSE_HOLD_MS = 10000UL;

const unsigned long KEYPAD_DEBOUNCE_MS = 45;
const unsigned long KEYPAD_HOLD_MS     = 1000;
const unsigned long KEY_REPEAT_COOLDOWN_MS = 250;

const char DEFUSE_KEY = '#';
const char ERASE_KEY  = '*';
const String ARM_CODE = "7355608";

enum class BombState {
  BOMB_IDLE, BOMB_ARMED, BOMB_DEFUSED, BOMB_EXPLODED,
  MENU, MODE_CLOCK, MODE_TIMER_SET, MODE_TIMER, MODE_STOPWATCH
};
BombState state = BombState::BOMB_IDLE;

unsigned long armedAt = 0;
unsigned long lastBeepAt = 0;

String enteredArmCode = "";

char lastAcceptedKey = 0;
unsigned long lastAcceptedKeyAt = 0;

unsigned long defuseHoldStart = 0;
bool defuseHolding = false;

long clockOffsetSeconds = 0;
unsigned long clockStartedAt = 0;

unsigned long timerDurationMs = 0;
unsigned long timerStartedAt = 0;
bool timerRunning = false;
bool timerFinished = false;

unsigned long stopwatchStartedAt = 0;
unsigned long stopwatchElapsedMs = 0;
bool stopwatchRunning = false;

void showIdleScreen();
void handleIdle(char key);
void armBomb();
void handleArmed();
void updateArmedScreen(unsigned long remainingMs, unsigned long defuseProgressMs);
void defuseBomb();
void handleDefused(char key);
void explode();
void handleExploded(char key);
bool isKeyDown(char target);
bool initDisplay();
void debugPrintPressedKeys();
void lcdPrintLine(uint8_t row, const String &text);
char getFreshPressedKey();
String buildCodeMask();
bool isAllZeros(const String &code);

void openMenu();
void showMenuScreen();
void handleMenu(char key);

void enterClockMode();
void updateClockScreen();
void handleClock(char key);

void enterTimerSetMode();
void showTimerSetScreen();
void handleTimerSet(char key);
void updateTimerScreen(unsigned long remainingMs);
void handleTimer(char key);

void enterStopwatchMode();
void updateStopwatchScreen(bool force);
void handleStopwatch(char key);

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  keypad.setDebounceTime(KEYPAD_DEBOUNCE_MS);
  keypad.setHoldTime(KEYPAD_HOLD_MS);

  bool displayOk = initDisplay();

  if (displayOk) {
    lcdPrintLine(0, "Display OK");
    lcdPrintLine(1, "Init...");
    delay(700);
  }

  showIdleScreen();
}

bool initDisplay() {
  Wire.begin(21, 22);
  Wire.setClock(100000);

  Wire.beginTransmission(LCD_ADDR);
  byte err = Wire.endTransmission();

  if (err != 0) {
    Serial.print("[Display] LCD not found at 0x");
    Serial.print(LCD_ADDR, HEX);
    Serial.println("");
    return false;
  }

  lcd.init();
  lcd.backlight();
  lcd.clear();

  Serial.print("[Display] LCD found at 0x");
  Serial.println(LCD_ADDR, HEX);
  return true;
}

void lcdPrintLine(uint8_t row, const String &text) {
  String padded = text;
  while (padded.length() < LCD_COLS) padded += ' ';
  if (padded.length() > LCD_COLS) padded = padded.substring(0, LCD_COLS);

  lcd.setCursor(0, row);
  lcd.print(padded);
}

void debugPrintPressedKeys() {
  for (int i = 0; i < LIST_MAX; i++) {
    if (keypad.key[i].kchar != NO_KEY && keypad.key[i].kstate == PRESSED) {
      Serial.print("[Keypad] ");
      Serial.println(keypad.key[i].kchar);
    }
  }
}

char getFreshPressedKey() {
  for (int i = 0; i < LIST_MAX; i++) {
    if (keypad.key[i].kchar != NO_KEY && keypad.key[i].kstate == PRESSED) {
      return keypad.key[i].kchar;
    }
  }
  return NO_KEY;
}

void loop() {
  keypad.getKeys();
  debugPrintPressedKeys();

  char freshKey = getFreshPressedKey();

  switch (state) {
    case BombState::BOMB_IDLE:
      handleIdle(freshKey);
      break;
    case BombState::BOMB_ARMED:
      handleArmed();
      break;
    case BombState::BOMB_DEFUSED:
      handleDefused(freshKey);
      break;
    case BombState::BOMB_EXPLODED:
      handleExploded(freshKey);
      break;
    case BombState::MENU:
      handleMenu(freshKey);
      break;
    case BombState::MODE_CLOCK:
      handleClock(freshKey);
      break;
    case BombState::MODE_TIMER_SET:
      handleTimerSet(freshKey);
      break;
    case BombState::MODE_TIMER:
      handleTimer(freshKey);
      break;
    case BombState::MODE_STOPWATCH:
      handleStopwatch(freshKey);
      break;
  }
}

bool isKeyDown(char target) {
  for (int i = 0; i < LIST_MAX; i++) {
    if (keypad.key[i].kchar == target &&
        (keypad.key[i].kstate == PRESSED || keypad.key[i].kstate == HOLD)) {
      return true;
    }
  }
  return false;
}

String buildCodeMask() {
  String mask = "";
  for (int i = 0; i < (int)ARM_CODE.length(); i++) {
    if (i < (int)enteredArmCode.length()) {
      mask += enteredArmCode[i];
    } else {
      mask += '-';
    }
  }
  return mask;
}

bool isAllZeros(const String &code) {
  if (code.length() == 0) return false;
  for (int i = 0; i < (int)code.length(); i++) {
    if (code[i] != '0') return false;
  }
  return true;
}

void showIdleScreen() {
  lcdPrintLine(0, "ENTER CODE:");
  lcdPrintLine(1, buildCodeMask());
}

void handleIdle(char key) {
  if (!key) return;

  unsigned long now = millis();
  if (key == lastAcceptedKey && (now - lastAcceptedKeyAt) < KEY_REPEAT_COOLDOWN_MS) {
    return;
  }
  lastAcceptedKey = key;
  lastAcceptedKeyAt = now;

  if (isDigit(key)) {
    enteredArmCode += key;

    if (enteredArmCode.length() > ARM_CODE.length()) {
      enteredArmCode = String(key);
    }

    tone(BUZZER_PIN, 1000, 40);

    if (enteredArmCode.length() == ARM_CODE.length()) {
      if (enteredArmCode == ARM_CODE) {
        armBomb();
        return;
      } else if (isAllZeros(enteredArmCode)) {
        openMenu();
        return;
      } else {
        enteredArmCode = "";
      }
    }
  } else if (key == ERASE_KEY) {
    if (enteredArmCode.length() > 0) {
      enteredArmCode.remove(enteredArmCode.length() - 1);
      tone(BUZZER_PIN, 600, 40);
    }
  }

  showIdleScreen();
}

void armBomb() {
  enteredArmCode = "";

  lcdPrintLine(0, "BOMB HAS BEEN");
  lcdPrintLine(1, "  PLANTED!");

  tone(BUZZER_PIN, 1500, 250);
  delay(300);

  state = BombState::BOMB_ARMED;
  armedAt = millis();
  defuseHolding = false;
}

void handleArmed() {
  unsigned long elapsed = millis() - armedAt;

  if (elapsed >= BOMB_TIME_MS) {
    explode();
    return;
  }

  unsigned long remaining = BOMB_TIME_MS - elapsed;

  long beepInterval;
  if (remaining > 10000UL) {
    beepInterval = map(remaining, 10000, BOMB_TIME_MS, 400, 900);
  } else {
    beepInterval = map(remaining, 0, 10000, 80, 400);
  }

  if (millis() - lastBeepAt >= (unsigned long)beepInterval) {
    tone(BUZZER_PIN, 1800, 60);
    digitalWrite(LED_PIN, HIGH);
    delay(25);
    digitalWrite(LED_PIN, LOW);
    lastBeepAt = millis();
  }

  bool down = isKeyDown(DEFUSE_KEY);

  if (down && !defuseHolding) {
    defuseHolding = true;
    defuseHoldStart = millis();
    tone(BUZZER_PIN, 900, 100);
  }

  if (!down && defuseHolding) {
    defuseHolding = false;
  }

  unsigned long defuseProgress = 0;
  if (defuseHolding) {
    defuseProgress = millis() - defuseHoldStart;
    if (defuseProgress >= DEFUSE_HOLD_MS) {
      defuseBomb();
      return;
    }
  }

  updateArmedScreen(remaining, defuseProgress);
}

void updateArmedScreen(unsigned long remainingMs, unsigned long defuseProgressMs) {
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw < 200) return;
  lastDraw = millis();

  int secLeft = (remainingMs + 999) / 1000;

  char line0[LCD_COLS + 1];
  snprintf(line0, sizeof(line0), "ARMED   %02d sec", secLeft);
  lcdPrintLine(0, line0);

  char line1[LCD_COLS + 1];
  if (defuseProgressMs > 0) {
    int pct = (defuseProgressMs * 100) / DEFUSE_HOLD_MS;
    if (pct > 100) pct = 100;
    snprintf(line1, sizeof(line1), "Defusing: %3d%%", pct);
  } else {
    snprintf(line1, sizeof(line1), "Hold # to defuse");
  }
  lcdPrintLine(1, line1);
}

void defuseBomb() {
  noTone(BUZZER_PIN);
  digitalWrite(LED_PIN, LOW);
  defuseHolding = false;
  state = BombState::BOMB_DEFUSED;

  lcdPrintLine(0, "BOMB HAS BEEN");
  lcdPrintLine(1, "  DEFUSED");

  tone(BUZZER_PIN, 800, 200);
  delay(250);
  tone(BUZZER_PIN, 1200, 300);
}

void handleDefused(char key) {
  if (key == ERASE_KEY) {
    state = BombState::BOMB_IDLE;
    enteredArmCode = "";
    lastAcceptedKey = 0;
    showIdleScreen();
  }
}

void explode() {
  state = BombState::BOMB_EXPLODED;
  defuseHolding = false;

  lcdPrintLine(0, "     BOOM!");
  lcdPrintLine(1, "");

  unsigned long start = millis();
  while (millis() - start < 3000) {
    digitalWrite(LED_PIN, HIGH);
    tone(BUZZER_PIN, 2000, 80);
    delay(80);
    digitalWrite(LED_PIN, LOW);
    tone(BUZZER_PIN, 1000, 80);
    delay(80);
  }
  noTone(BUZZER_PIN);
}

void handleExploded(char key) {
  if (key == ERASE_KEY) {
    state = BombState::BOMB_IDLE;
    enteredArmCode = "";
    lastAcceptedKey = 0;
    showIdleScreen();
  }
}


void openMenu() {
  enteredArmCode = "";
  state = BombState::MENU;
  tone(BUZZER_PIN, 1200, 100);
  showMenuScreen();
}

void showMenuScreen() {
  lcdPrintLine(0, "1)Clock 2)Timer");
  lcdPrintLine(1, "3)Stopwatch *Ex");
}

void handleMenu(char key) {
  if (!key) return;

  switch (key) {
    case '1':
      enterClockMode();
      break;
    case '2':
      enterTimerSetMode();
      break;
    case '3':
      enterStopwatchMode();
      break;
    case ERASE_KEY:
      state = BombState::BOMB_IDLE;
      enteredArmCode = "";
      showIdleScreen();
      break;
    default:
      break;
  }
}


void enterClockMode() {
  clockStartedAt = millis();
  enteredArmCode = "";
  state = BombState::MODE_CLOCK;
  updateClockScreen();
}

void updateClockScreen() {
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw < 500) return;
  lastDraw = millis();

  unsigned long elapsedSec = (millis() - clockStartedAt) / 1000UL;
  long totalSec = (clockOffsetSeconds + (long)elapsedSec) % 86400L;
  if (totalSec < 0) totalSec += 86400L;

  int hh = totalSec / 3600;
  int mm = (totalSec % 3600) / 60;
  int ss = totalSec % 60;

  char line0[LCD_COLS + 1];
  snprintf(line0, sizeof(line0), "CLOCK  %02d:%02d:%02d", hh, mm, ss);
  lcdPrintLine(0, line0);

  char line1[LCD_COLS + 1];
  if (enteredArmCode.length() > 0) {
    snprintf(line1, sizeof(line1), "Set HHMM:%-6s", enteredArmCode.c_str());
  } else {
    snprintf(line1, sizeof(line1), "#set  *menu");
  }
  lcdPrintLine(1, line1);
}

void handleClock(char key) {
  updateClockScreen();
  if (!key) return;

  if (isDigit(key) && enteredArmCode.length() < 4) {
    enteredArmCode += key;
    tone(BUZZER_PIN, 1000, 30);
    updateClockScreen();
  } else if (key == DEFUSE_KEY) {
    if (enteredArmCode.length() == 4) {
      int hh = enteredArmCode.substring(0, 2).toInt();
      int mm = enteredArmCode.substring(2, 4).toInt();
      if (hh >= 0 && hh < 24 && mm >= 0 && mm < 60) {
        clockOffsetSeconds = (long)hh * 3600L + (long)mm * 60L;
        clockStartedAt = millis();
        tone(BUZZER_PIN, 1500, 150);
      }
    }
    enteredArmCode = "";
    updateClockScreen();
  } else if (key == ERASE_KEY) {
    if (enteredArmCode.length() > 0) {
      enteredArmCode.remove(enteredArmCode.length() - 1);
      updateClockScreen();
    } else {
      state = BombState::MENU;
      showMenuScreen();
    }
  }
}


void enterTimerSetMode() {
  enteredArmCode = "";
  state = BombState::MODE_TIMER_SET;
  showTimerSetScreen();
}

void showTimerSetScreen() {
  lcdPrintLine(0, "SET TIMER MMSS:");
  char line1[LCD_COLS + 1];
  snprintf(line1, sizeof(line1), "%-4s   #ok *back", enteredArmCode.c_str());
  lcdPrintLine(1, line1);
}

void handleTimerSet(char key) {
  if (!key) return;

  if (isDigit(key) && enteredArmCode.length() < 4) {
    enteredArmCode += key;
    tone(BUZZER_PIN, 1000, 30);
    showTimerSetScreen();
  } else if (key == ERASE_KEY) {
    if (enteredArmCode.length() > 0) {
      enteredArmCode.remove(enteredArmCode.length() - 1);
      showTimerSetScreen();
    } else {
      state = BombState::MENU;
      showMenuScreen();
    }
  } else if (key == DEFUSE_KEY) {
    if (enteredArmCode.length() > 0) {
      String padded = enteredArmCode;
      while (padded.length() < 4) padded = "0" + padded;
      int mm = padded.substring(0, 2).toInt();
      int ss = padded.substring(2, 4).toInt();
      unsigned long totalSec = (unsigned long)mm * 60UL + (unsigned long)ss;
      if (totalSec > 0) {
        timerDurationMs = totalSec * 1000UL;
        timerStartedAt = millis();
        timerRunning = true;
        timerFinished = false;
        state = BombState::MODE_TIMER;
        tone(BUZZER_PIN, 1500, 150);
      }
    }
  }
}

void updateTimerScreen(unsigned long remainingMs) {
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw < 200) return;
  lastDraw = millis();

  int secLeft = (remainingMs + 999) / 1000;
  int mm = secLeft / 60;
  int ss = secLeft % 60;

  char line0[LCD_COLS + 1];
  snprintf(line0, sizeof(line0), "TIMER  %02d:%02d", mm, ss);
  lcdPrintLine(0, line0);
  lcdPrintLine(1, timerRunning ? "#pause  *menu" : "#resume *menu");
}

void handleTimer(char key) {
  if (timerRunning && !timerFinished) {
    unsigned long elapsed = millis() - timerStartedAt;
    if (elapsed >= timerDurationMs) {
      timerFinished = true;
      timerRunning = false;
      lcdPrintLine(0, "TIME'S UP!");
      lcdPrintLine(1, "*menu");
      tone(BUZZER_PIN, 2000, 500);
    } else {
      updateTimerScreen(timerDurationMs - elapsed);
    }
  }

  if (!key) return;

  if (key == DEFUSE_KEY && !timerFinished) {
    if (timerRunning) {
      unsigned long elapsed = millis() - timerStartedAt;
      timerDurationMs = (elapsed < timerDurationMs) ? (timerDurationMs - elapsed) : 0;
      timerRunning = false;
    } else {
      timerStartedAt = millis();
      timerRunning = true;
    }
  } else if (key == ERASE_KEY) {
    timerRunning = false;
    state = BombState::MENU;
    showMenuScreen();
  }
}


void enterStopwatchMode() {
  state = BombState::MODE_STOPWATCH;
  stopwatchElapsedMs = 0;
  stopwatchRunning = false;
  updateStopwatchScreen(true);
}

void updateStopwatchScreen(bool force) {
  static unsigned long lastDraw = 0;
  if (!force && millis() - lastDraw < 100) return;
  lastDraw = millis();

  unsigned long totalMs = stopwatchElapsedMs;
  if (stopwatchRunning) totalMs += millis() - stopwatchStartedAt;

  unsigned long totalSec = totalMs / 1000UL;
  int mm = totalSec / 60;
  int ss = totalSec % 60;
  int cs = (totalMs % 1000UL) / 10UL;

  char line0[LCD_COLS + 1];
  snprintf(line0, sizeof(line0), "STOPWATCH %02d:%02d", mm, ss);
  lcdPrintLine(0, line0);

  char line1[LCD_COLS + 1];
  snprintf(line1, sizeof(line1), ".%02d #%s *rst", cs, stopwatchRunning ? "stop" : "run ");
  lcdPrintLine(1, line1);
}

void handleStopwatch(char key) {
  updateStopwatchScreen(false);
  if (!key) return;

  if (key == DEFUSE_KEY) {
    if (stopwatchRunning) {
      stopwatchElapsedMs += millis() - stopwatchStartedAt;
      stopwatchRunning = false;
    } else {
      stopwatchStartedAt = millis();
      stopwatchRunning = true;
    }
    updateStopwatchScreen(true);
  } else if (key == ERASE_KEY) {
    if (stopwatchRunning || stopwatchElapsedMs > 0) {
      stopwatchRunning = false;
      stopwatchElapsedMs = 0;
      updateStopwatchScreen(true);
    } else {
      state = BombState::MENU;
      showMenuScreen();
    }
  }
}
