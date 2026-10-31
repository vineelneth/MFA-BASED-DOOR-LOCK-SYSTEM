#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <WiFi.h>
#include "time.h"
#include <TOTP.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// ---------------- LCD ----------------
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ---------------- SERIAL FROM CAM ----------------
HardwareSerial CamSerial(2);  // RX=16, TX=17

// ---------------- WIFI (TIME SYNC ONCE) ----------------
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* botToken = TELEGRAM_BOT_TOKEN;
const char* chatID = TELEGRAM_CHATID;
const String masterPin = "739284";  // choose strong one
int masterAttempts = 0;

// ---------------- GOOGLE AUTH SECRET (Decoded "Hello!") ----------------
uint8_t hmacKey[] = {
  0x48, 0x65, 0x6C, 0x6C, 0x6F, 0x21, 0xDE, 0xAD, 0xBE, 0xEF
};
TOTP totp(hmacKey, sizeof(hmacKey));

// ---------------- SYSTEM VARIABLES ----------------
bool doorUnlocked = false;
#define VIB_PIN 34
bool faceVerified = false;
String enteredOTP = "";
int wrongAttempts = 0;
unsigned long vibrationStart = 0;
int vibrationCount = 0;
bool systemLocked = false;
unsigned long lockStartTime = 0;
unsigned long lastVibrationAlert = 0;

// ---------------- RELAY & BUZZER ----------------
#define RELAY_PIN 23
#define BUZZER_PIN 4

// ---------------- KEYPAD ----------------
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  { '1', '2', '3', 'A' },
  { '4', '5', '6', 'B' },
  { '7', '8', '9', 'C' },
  { '*', '0', '#', 'D' }
};
byte rowPins[ROWS] = { 13, 12, 14, 27 };
byte colPins[COLS] = { 26, 25, 33, 32 };
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);
  CamSerial.begin(115200, SERIAL_8N1, 16, 17);

  lcd.init();
  lcd.backlight();

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(VIB_PIN, INPUT);

  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  lcd.print("Syncing Time...");

  // ---------- NTP TIME SYNC ----------
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  // Google Auth requires strict UTC time, offset must remain 0
  configTime(0, 0, "pool.ntp.org");
  while (time(nullptr) < 100000) {
    delay(500);
  }

  Serial.println("Time synced.");
  Serial.print("Current Unix Time: ");
  Serial.println(time(nullptr));

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  lcd.clear();
  lcd.print("Waiting for Face...");
}

// ---------------- LOOP ----------------
void loop() {
  if (!doorUnlocked && digitalRead(VIB_PIN) == LOW) {
    if (vibrationCount == 0) {
      vibrationStart = millis();
    }
    
    vibrationCount++;

    if (millis() - vibrationStart > 2000) {
      if (vibrationCount >= 1 && millis() - lastVibrationAlert > 10000) {
        lastVibrationAlert = millis();
        lcd.clear();
        lcd.print("DOOR BREAKING!");

        for (int i = 0; i < 20; i++) {
          digitalWrite(BUZZER_PIN, HIGH);
          delay(100);
          digitalWrite(BUZZER_PIN, LOW);
          delay(100);
        }
        digitalWrite(BUZZER_PIN, LOW);

        sendAlert("FORCE_ENTRY");
      }
      vibrationCount = 0;
    }
  }

  if (systemLocked) {
    if (millis() - lockStartTime > 120000) {  // 2 minutes
      systemLocked = false;
      wrongAttempts = 0;
      masterAttempts = 0;

      lcd.clear();
      lcd.print("SYSTEM UNLOCKED");
      delay(2000);

      lcd.clear();
      lcd.print("Waiting for Face...");
    }
    return;
  }

  // -------- RECEIVE FACE_OK FROM CAM --------
  static String received = "";
  while (CamSerial.available()) {
    char c = CamSerial.read();

    if (c == '\n') {
      received.trim();

      if (received.startsWith("FACE_OK:")) {
        String personName = received.substring(8);
        personName.trim();

        faceVerified = true;

        lcd.clear();
        lcd.print("Hello");
        lcd.setCursor(0, 1);
        lcd.print(personName);

        delay(2000);

        lcd.clear();
        lcd.print("Enter OTP:");
      }

      received = "";

    } else if (c != '\r') {
      received += c;
    }
  }

  // -------- KEYPAD PROCESS --------
  if (faceVerified) {
    char key = keypad.getKey();

    if (key) {
      if (key == '#') {
        enteredOTP = "";
        lcd.setCursor(0, 1);
        lcd.print("                ");
        lcd.setCursor(0, 1);

      } else if (key >= '0' && key <= '9') {
        if (enteredOTP.length() < 6) {
          enteredOTP += key;
          lcd.setCursor(0, 1);
          lcd.print(enteredOTP);
        }

        if (enteredOTP.length() == 6) {
          delay(200);

          time_t now = time(nullptr);

          // FIX: Immediately cast to String to prevent the static buffer from overwriting itself
          String otpNow = String(totp.getCode(now));
          String otpPrev = String(totp.getCode(now - 30));
          String otpNext = String(totp.getCode(now + 30));

          // -------- DEBUG PRINTS --------
          Serial.println("---------------");
          Serial.println("Entered: " + enteredOTP);
          Serial.println("Now:  " + otpNow);
          Serial.println("Prev: " + otpPrev);
          Serial.println("Next: " + otpNext);
          Serial.print("Unix Time: ");
          Serial.println(now);
          Serial.println("---------------");

          bool otpValid = false;
          bool masterValid = false;

          if (enteredOTP == otpNow || enteredOTP == otpPrev || enteredOTP == otpNext) {
            otpValid = true;
          }

          // ---- MASTER PIN LOGIC ----
          bool wifiOff = (WiFi.status() != WL_CONNECTED);

          if (enteredOTP == masterPin && wifiOff) {
            masterValid = true;
          }

          if (otpValid || masterValid) {
            lcd.clear();
            lcd.print("ACCESS GRANTED");

            doorUnlocked = true;

            digitalWrite(RELAY_PIN, HIGH);
            delay(5000);
            digitalWrite(RELAY_PIN, LOW);

            doorUnlocked = false;
            wrongAttempts = 0;
            masterAttempts = 0;
            lcd.clear();
            lcd.print("Waiting for Face...");

          } else {

            if (enteredOTP == masterPin) {
              masterAttempts++;
            } else {
              wrongAttempts++;
            }

            if (masterAttempts >= 3) {
              systemLocked = true;
              lockStartTime = millis();

              lcd.clear();
              lcd.print("MASTER LOCKED");
              wrongAttempts = 0;
              masterAttempts = 0;

              for (int i = 0; i < 5; i++) {
                digitalWrite(BUZZER_PIN, HIGH);
                delay(100);
                digitalWrite(BUZZER_PIN, LOW);
                delay(100);
              }
              digitalWrite(BUZZER_PIN, LOW);

              sendAlert("MASTER_PIN_ATTACK");
              return;
            }

            lcd.clear();
            if (enteredOTP == masterPin) {
              lcd.print("WRONG MASTER");
            } else {
              lcd.print("WRONG OTP");
            }
            delay(1500);

            if (wrongAttempts >= 5) {
              systemLocked = true;
              lockStartTime = millis();

              lcd.clear();
              lcd.print("SYSTEM LOCKED");

              for (int i = 0; i < 5; i++) {
                digitalWrite(BUZZER_PIN, HIGH);
                delay(100);
                digitalWrite(BUZZER_PIN, LOW);
                delay(100);
              }
              digitalWrite(BUZZER_PIN, LOW);
              
              sendAlert("OTP_ATTACK");
              wrongAttempts = 0;
              masterAttempts = 0;
            }
          }

          enteredOTP = "";
          faceVerified = false;

          if (!systemLocked) {
            delay(2000);  // show wrong message for 2 sec
            lcd.clear();
            lcd.print("Waiting for Face...");
          }
        }
      }
    }
  }
}

void sendAlert(String type) {
  WiFi.begin(ssid, password);
  unsigned long startAttempt = millis();
  
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 8000) {
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    client.setInsecure();  // skip certificate validation

    HTTPClient http;

    String message = "🚨 ALERT: " + type;
    message.replace(" ", "%20");

    String url = "https://api.telegram.org/bot";
    url += botToken;
    url += "/sendMessage?chat_id=";
    url += chatID;
    url += "&text=";
    url += message;

    http.begin(client, url);
    http.GET();
    http.end();

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  } else {
    Serial.println("Telegram send failed - no WiFi");
  }
}