#include <WiFi.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_AHTX0.h>
#include <ScioSense_ENS160.h>
#include "time.h"
#include <Preferences.h>
#include <WebServer.h>


// ====== WiFi和时间配置 ======
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec      = 8 * 3600;   // GMT+8 中国标准时间
const int   daylightOffset_sec = 0;

// ===================== 配网参数 =====================
#define AP_SSID           "Air Conditioner"  // 配网AP名称
#define AP_PASSWORD       "12345678"             // 配网AP密码（至少8位）
#define AP_CHANNEL        1                      // AP信道
#define MAX_WIFI_RECORDS  5                      // 最大存储WiFi记录数
#define WIFI_CONNECT_TIMEOUT  10000              // WiFi连接超时时间（ms）
#define SCAN_WIFI_DELAY   2000                   // 扫描WiFi延迟（ms）

// ===================== 配网全局对象 =====================
Preferences preferences;  // Flash存储对象（用于保存WiFi信息）
String ssidList[MAX_WIFI_RECORDS];  // 存储WiFi名称
String pwdList[MAX_WIFI_RECORDS];   // 存储WiFi密码
int wifiRecordCount = 0;            // 已存储WiFi记录数

// ====== 引脚定义 ======
#define TFT_CS   9
#define TFT_DC   8
#define TFT_RST  7
#define TFT_MOSI 6
#define TFT_SCLK 5

#define ENC_A_PIN    10
#define ENC_B_PIN    20
#define ENC_BTN_PIN  21
#define KEY0_PIN     0

#define SDA_PIN 1
#define SCL_PIN 2

#define LED_PIN 4
#define BUZZ_PIN 3

// ====== TFT和传感器对象 ======
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
Adafruit_AHTX0   aht;
ScioSense_ENS160 ens160(0x53);

// ====== 中文字体支持 ======
// 为支持中文字符定义的映射表




// ====== 颜色定义 ======
#define CYBER_BG      ST7735_BLACK
#define CYBER_GREEN   0x07E0  
#define CYBER_ACCENT  0x07FF  
#define CYBER_LIGHT   0xFD20  
#define CYBER_BLUE    0x07FF  
#define CYBER_PINK    0xF81F

#define AQ_BAR_GREEN  0x07E0
#define AQ_BAR_YELLOW 0xFFE0
#define AQ_BAR_ORANGE 0xFD20
#define AQ_BAR_RED    0xF800
#define CYBER_DARK    0x4208

#ifndef PI
#define PI 3.1415926
#endif

// ====== 界面模式 ======
enum UIMode {
  MODE_MENU = 0,
  MODE_CLOCK,
  MODE_ALARM
};
UIMode currentMode = MODE_CLOCK;
int menuIndex = 0;
const int MENU_ITEMS = 2;


// ====== 环境数值 ======
float    curTemp = 0;
float    curHum  = 0;
uint16_t curTVOC = 0;
uint16_t curECO2 = 400;
unsigned long lastEnvRead = 0;

// ====== 时钟变量 ======
int    prevSecond  = -1;
String prevTimeStr = "";

// ====== 编码器和按钮 ======
int  lastEncA   = HIGH;
int  lastEncB   = HIGH;
bool lastEncBtn = HIGH;
bool lastKey0   = HIGH;
unsigned long lastBtnMs = 0;

// ====== 闹钟 ======
bool     alarmEnabled = false;
uint8_t  alarmHour    = 7;
uint8_t  alarmMinute  = 0;
bool     alarmRinging = false;
int      alarmSelectedField = 0;
int      lastAlarmDayTriggered = -1;

// ====== 警报/LED闪烁 ======
enum AlertLevel {
  ALERT_NONE = 0,
  ALERT_CO2,
  ALERT_ALARM
};
AlertLevel currentAlertLevel = ALERT_NONE;
unsigned long lastLedToggleMs = 0;
bool ledState = false;

unsigned long lastCo2BlinkMs = 0;
bool co2BlinkOn = false;

// ========= 辅助函数: 编码器和按钮 =========
int readEncoderStep() {
  int encA = digitalRead(ENC_A_PIN);
  int encB = digitalRead(ENC_B_PIN);
  int step = 0;
  if (encA != lastEncA) {
    if (encA == LOW) {
      if (encB == HIGH) step = +1;
      else              step = -1;
    }
  }
  lastEncA = encA;
  lastEncB = encB;
  return step;
}

bool checkButtonPressed(uint8_t pin, bool &lastState) {
  bool cur = digitalRead(pin);
  bool pressed = false;
  unsigned long now = millis();
  if (cur == LOW && lastState == HIGH && (now - lastBtnMs) > 150) {
    pressed = true;
    lastBtnMs = now;
  }
  lastState = cur;
  return pressed;
}

// ========= 闹钟图标 =========
void drawAlarmIcon() {
  int x = 308;
  tft.fillRect(x - 10, 0, 12, 12, CYBER_BG);
  if (!alarmEnabled) return;

  uint16_t c = CYBER_LIGHT;
  tft.drawRoundRect(x - 9, 2, 10, 7, 2, c);
  tft.drawFastHLine(x - 8, 8, 8, c);
  tft.fillCircle(x - 4, 10, 1, c);
}

// ========= 中文文本显示辅助函数 =========
// 注意：ST7735库不直接支持中文字符，这里提供兼容接口
void displayChinese(int x, int y, const char* text, uint16_t color = ST7735_WHITE, uint16_t bgColor = CYBER_BG) {
  // 使用标准Adafruit GFX函数显示文本
  tft.setTextColor(color, bgColor);
  tft.setCursor(x, y);
  tft.print(text);
}

void displayChineseCenter(int x0, int x1, int y, const char* text, uint16_t color = ST7735_WHITE, uint16_t bgColor = CYBER_BG) {
  int16_t bx, by;
  uint16_t w, h;
  tft.setTextSize(1);  // 设置字体大小为1
  tft.getTextBounds((char*)text, 0, 0, &bx, &by, &w, &h);
  
  int x_pos = x0 + ((x1 - x0) - (int)w) / 2;
  
  tft.setTextColor(color, bgColor);
  tft.setCursor(x_pos, y);
  tft.print(text);
  tft.setTextSize(1);  // 恢复默认字体大小
}

// ========= WiFi和时间同步 =========
void connectWiFiAndSyncTimeOriginal() {
  const char* ssid      = "HUAWEI-B311-850C";
  const char* password  = "jixiao85";
  tft.fillScreen(CYBER_BG);
  displayChinese(20, 110, "Connecting WiFi", CYBER_LIGHT);
  
  WiFi.begin(ssid, password);

  uint8_t retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(300);
    tft.fillRect(160 + retry*10, 110, 8, 8, CYBER_LIGHT);
    retry++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    tft.fillScreen(CYBER_BG);
    displayChinese(20, 110, "Syncing Time...", CYBER_LIGHT);
    delay(800);
  } else {
    tft.fillScreen(CYBER_BG);
    displayChinese(20, 110, "WiFi Connect Failed!", ST7735_RED);
    delay(1000);
  }
}

String getTimeStr(char type) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "--";
  char buf[8];
  if (type == 'H') strftime(buf, sizeof(buf), "%H", &timeinfo);
  else if (type == 'M') strftime(buf, sizeof(buf), "%M", &timeinfo);
  else if (type == 'S') strftime(buf, sizeof(buf), "%S", &timeinfo);
  return String(buf);
}

// ===================== 配网函数声明 =====================
void initAPMode();                  // 初始化AP配网模式
void scanSurroundingWiFi();         // 扫描周边WiFi热点
void saveWiFiToFlash(String ssid, String pwd);  // 保存WiFi信息到Flash
void loadWiFiFromFlash();           // 从Flash加载WiFi信息
bool connectWiFi(String ssid, String pwd);      // 连接指定WiFi
void autoConnectWiFi();             // 自动连接已存储的WiFi
void clearInvalidWiFiRecord(String invalidSsid);  // 清除无效WiFi记录

// ===================== Web服务器相关 =====================
WebServer server(80);                // Web服务器，端口80
void handleRoot();                   // 处理根路径请求
void handleConfig();                 // 处理配置请求
void sendConfigPage();               // 发送配置页面

// 为了解决函数声明问题，重新声明原始的WiFi连接函数
void connectWiFiAndSyncTimeOriginal();

// ========= 环境传感器读取 =========
// 全局变量用于跟踪传感器初始化状态
bool ahtInitialized = false;
bool ens160Initialized = false;

void updateEnvSensors(bool force = false) {
  unsigned long now = millis();
  if (!force && (now - lastEnvRead) < 5000) return;

  // 防止传感器读取阻塞，确保非阻塞操作
  sensors_event_t hum, temp;
  
  // 读取AHT21传感器，仅在传感器已初始化时进行
  if (ahtInitialized) {
    if (aht.getEvent(&hum, &temp)) {
      curTemp = temp.temperature;
      curHum  = hum.relative_humidity;
      Serial.print("AHT21 Reading: Temp=");
      Serial.print(curTemp);
      Serial.print("C, Hum=");
      Serial.print(curHum);
      Serial.println("%");
    } else {
      Serial.println("AHT21 sensor read failed");
    }
  }
  
  // 设置环境数据并触发ENS160测量，仅在传感器已初始化时进行
  if (ens160Initialized) {
    // 首先设置环境数据
    ens160.set_envdata(curTemp, curHum);
    
    // 然后进行测量
    if (ens160.measure()) {
      uint16_t newTVOC = ens160.getTVOC();
      uint16_t newCO2  = ens160.geteCO2();
      
      // 检查返回值是否有效
      if (newTVOC != 0xFFFF) {
        curTVOC = newTVOC;
      }
      if (newCO2 != 0xFFFF) {
        curECO2 = newCO2;
      }
      
      Serial.print("ENS160 Reading: TVOC=");
      Serial.print(curTVOC);
      Serial.print(" ppb, eCO2=");
      Serial.print(curECO2);
      Serial.println(" ppm");
    } else {
      Serial.println("ENS160 measure failed");
    }
  }

  // 确保时间戳更新，避免因传感器问题导致的阻塞
  lastEnvRead = now;
}

// ========= 时钟界面 =========
const int GRID_L   = 8;
const int GRID_R   = 152;
const int GRID_TOP = 56;
const int GRID_MID = 80;
const int GRID_BOT = 104;
const int GRID_MID_X = (GRID_L + GRID_R) / 2;

const int TOP_LABEL_Y    = GRID_TOP + 4;
const int TOP_VALUE_Y    = GRID_TOP + 15;
const int BOTTOM_LABEL_Y = GRID_MID + 4;
const int BOTTOM_VALUE_Y = GRID_MID + 15;

const int BAR_MARGIN_X = 4;
const int BAR_GAP      = 4;
const int BAR_Y        = 110;
const int BAR_H        = 12;
const int BAR_W        = (160 - 2 * BAR_MARGIN_X - 3 * BAR_GAP) / 4;

void printCenteredText(const String &txt,
                       int x0, int x1,
                       int y,
                       uint16_t color,
                       uint16_t bg,
                       uint8_t size = 1) {
  int16_t bx, by;
  uint16_t w, h;
  tft.setTextSize(size);
  tft.getTextBounds(txt, 0, 0, &bx, &by, &w, &h);
  int x = x0 + ((x1 - x0) - (int)w) / 2;
  tft.setTextColor(color, bg);
  tft.setCursor(x, y);
  tft.print(txt);
}

uint16_t colorForCO2(uint16_t eco2) {
  if (eco2 <= 800)  return AQ_BAR_GREEN;
  if (eco2 <= 1200) return AQ_BAR_YELLOW;
  if (eco2 <= 1800) return AQ_BAR_ORANGE;
  return AQ_BAR_RED;
}

void initClockStaticUI() {
  tft.fillScreen(CYBER_BG);
  
  displayChinese(4, 4, "Environment", CYBER_LIGHT);
  displayChinese(4, 44, "Air Quality:", ST7735_BLACK);

  tft.drawFastHLine(GRID_L, GRID_TOP, GRID_R - GRID_L, ST7735_WHITE);
  tft.drawFastHLine(GRID_L, GRID_MID, GRID_R - GRID_L, ST7735_WHITE);
  tft.drawFastHLine(GRID_L, GRID_BOT, GRID_R - GRID_L, ST7735_WHITE);
  tft.drawFastVLine(GRID_MID_X, GRID_TOP, GRID_BOT - GRID_TOP, ST7735_WHITE);

  displayChineseCenter(GRID_L, GRID_MID_X, TOP_LABEL_Y, "Humidity", ST7735_WHITE);
  displayChineseCenter(GRID_MID_X, GRID_R, TOP_LABEL_Y, "Temperature", ST7735_WHITE);
  displayChineseCenter(GRID_L, GRID_MID_X, BOTTOM_LABEL_Y, "TVOC", ST7735_WHITE);
  displayChineseCenter(GRID_MID_X, GRID_R, BOTTOM_LABEL_Y, "CO2", ST7735_WHITE);

  int x = BAR_MARGIN_X;
  tft.fillRect(x,                          BAR_Y, BAR_W, BAR_H, AQ_BAR_GREEN);
  tft.fillRect(x + (BAR_W + BAR_GAP),      BAR_Y, BAR_W, BAR_H, AQ_BAR_YELLOW);
  tft.fillRect(x + 2 * (BAR_W + BAR_GAP),  BAR_Y, BAR_W, BAR_H, AQ_BAR_ORANGE);
  tft.fillRect(x + 3 * (BAR_W + BAR_GAP),  BAR_Y, BAR_W, BAR_H, AQ_BAR_RED);

  drawAlarmIcon();
}

void drawClockTime(String hourStr, String minStr, String secStr) {
  String cur = hourStr + ":" + minStr + ":" + secStr;
  if (cur == prevTimeStr) return;
  prevTimeStr = cur;

  tft.setTextSize(3);
  int16_t x1, y1;
  uint16_t w, h;
  tft.getTextBounds(cur, 0, 0, &x1, &y1, &w, &h);
  int x = (160 - w) / 2;
  int y = 16;

  tft.setTextColor(CYBER_LIGHT, CYBER_BG);
  tft.setCursor(x, y);
  tft.print(cur);
}

void drawCO2Value(uint16_t eco2, uint16_t col) {
  char co2Buf[12];
  sprintf(co2Buf, "%4uppm", eco2);
  printCenteredText(String(co2Buf),
                    GRID_MID_X, GRID_R,
                    BOTTOM_VALUE_Y,
                    col, CYBER_BG, 1);
}

void drawEnvDynamic(float temp, float hum, uint16_t tvoc, uint16_t eco2) {
  uint16_t colHUMI = CYBER_ACCENT;
  uint16_t colTEMP = CYBER_LIGHT;
  uint16_t colTVOC = CYBER_GREEN;
  uint16_t colCO2  = colorForCO2(eco2);

  char humBuf[8];
  sprintf(humBuf, "%2.0f%%", hum);
  printCenteredText(String(humBuf),
                    GRID_L, GRID_MID_X,
                    TOP_VALUE_Y,
                    colHUMI, CYBER_BG, 1);

  char tempBuf[10];
  sprintf(tempBuf, "%2.1fC", temp);
  printCenteredText(String(tempBuf),
                    GRID_MID_X, GRID_R,
                    TOP_VALUE_Y,
                    colTEMP, CYBER_BG, 1);

  float tvoc_mg = tvoc / 1000.0f;
  char tvocBuf[16];
  sprintf(tvocBuf, "%.3f mg/m3", tvoc_mg);
  printCenteredText(String(tvocBuf),
                    GRID_L, GRID_MID_X,
                    BOTTOM_VALUE_Y,
                    colTVOC, CYBER_BG, 1);

  drawCO2Value(eco2, colCO2);

  uint8_t level = 1;
  if (eco2 > 1800) level = 4;
  else if (eco2 > 1200) level = 3;
  else if (eco2 > 800)  level = 2;

  tft.fillRect(0, BAR_Y + BAR_H + 2, 160, 16, CYBER_BG);
  int centerX = BAR_MARGIN_X + (BAR_W / 2) + (level - 1) * (BAR_W + BAR_GAP);
  int tipY    = BAR_Y + BAR_H + 4;
  tft.fillTriangle(centerX,     tipY - 8,
                   centerX - 8, tipY + 4,
                   centerX + 8, tipY + 4,
                   ST7735_WHITE);
}

// ========= 菜单界面 =========
const char* menuItemsEN[] = {
  "Environment",
  "Alarm"
};

void drawMenu() {
  tft.fillScreen(CYBER_BG);

  displayChinese(10, 10, "Mode Select", CYBER_LIGHT);

  for (int i = 0; i < MENU_ITEMS; i++) {
    int y = 32 + i * 18;
    if (i == menuIndex) {
      tft.fillRect(6, y - 2, 148, 14, CYBER_ACCENT);
      displayChinese(12, y, menuItemsEN[i], CYBER_BG, CYBER_ACCENT);
    } else {
      tft.fillRect(6, y - 2, 148, 14, CYBER_BG);
      displayChinese(12, y, menuItemsEN[i], ST7735_BLACK);
    }
  }
  drawAlarmIcon();
}


// ========= 闹钟界面 =========
void drawAlarmScreen(bool full) {
  if (full) {
    tft.fillScreen(CYBER_BG);
    displayChinese(8, 4, "Alarm Setup", CYBER_LIGHT);
    drawAlarmIcon();
  }

  char hBuf[3];
  char mBuf[3];
  sprintf(hBuf, "%02d", alarmHour);
  sprintf(mBuf, "%02d", alarmMinute);

  tft.setTextSize(3);

  String disp = String(hBuf) + ":" + String(mBuf);
  int16_t x1, y1;
  uint16_t w, h;
  tft.getTextBounds(disp, 0, 0, &x1, &y1, &w, &h);
  int x = (160 - w) / 2;
  int y = 15;

  tft.setCursor(x, y);
  tft.setTextColor(alarmSelectedField == 0 ? CYBER_LIGHT : ST7735_BLACK, CYBER_BG);
  tft.print(hBuf);

  tft.setTextColor(ST7735_BLACK, CYBER_BG);
  tft.print(":");

  tft.setTextColor(alarmSelectedField == 1 ? CYBER_LIGHT : ST7735_BLACK, CYBER_BG);
  tft.print(mBuf);

  tft.setTextSize(2);
  tft.setTextColor(ST7735_BLACK, CYBER_BG);
  tft.fillRect(20, 80, 120, 24, CYBER_BG);
  
  displayChinese(30, 84, "Alarm:", ST7735_BLACK);
  
  uint16_t enColor = alarmSelectedField == 2
                     ? CYBER_LIGHT
                     : (alarmEnabled ? CYBER_GREEN : ST7735_RED);
  
  if (alarmEnabled) {
    displayChinese(80, 84, "ON", enColor);
  } else {
    displayChinese(80, 84, "OFF", enColor);
  }
}

void drawAlarmRingingScreen() {
  tft.fillScreen(ST7735_RED);
  displayChineseCenter(0, 160, 40, "Alarm Ringing!", ST7735_WHITE, ST7735_RED);
}

// ========= 闹钟逻辑 =========
void checkAlarmTrigger() {
  if (!alarmEnabled || alarmRinging) return;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  if (timeinfo.tm_hour == alarmHour &&
      timeinfo.tm_min  == alarmMinute &&
      timeinfo.tm_sec  == 0) {

    alarmRinging = true;
    lastAlarmDayTriggered = timeinfo.tm_mday;
    currentMode = MODE_ALARM;
    drawAlarmRingingScreen();
  }
}

// ========= 警报视觉+声音 =========
void updateAlertStateAndLED() {
  if (alarmRinging) currentAlertLevel = ALERT_ALARM;
  else if (curECO2 > 1800) currentAlertLevel = ALERT_CO2;
  else currentAlertLevel = ALERT_NONE;

  unsigned long now = millis();

  unsigned long interval;
  if (currentAlertLevel == ALERT_ALARM)      interval = 120;
  else if (currentAlertLevel == ALERT_CO2)   interval = 250;
  else                                       interval = 1000;

  if (now - lastLedToggleMs > interval) {
    lastLedToggleMs = now;
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? HIGH : LOW);
  }

  if (currentAlertLevel == ALERT_CO2) {
    if (now - lastCo2BlinkMs > 350) {
      lastCo2BlinkMs = now;
      co2BlinkOn = !co2BlinkOn;
      uint16_t baseCol = colorForCO2(curECO2);
      uint16_t col = co2BlinkOn ? baseCol : CYBER_DARK;
      drawCO2Value(curECO2, col);
      tone(BUZZ_PIN, 1800, 80);
    }
  }
}



// ========= 初始化设置 =========
void setup() {
  Serial.begin(115200);
  delay(1500);

  pinMode(ENC_A_PIN,   INPUT_PULLUP);
  pinMode(ENC_B_PIN,   INPUT_PULLUP);
  pinMode(ENC_BTN_PIN, INPUT_PULLUP);
  pinMode(KEY0_PIN,    INPUT_PULLUP);

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZ_PIN, OUTPUT);

  Wire.begin(SDA_PIN, SCL_PIN);
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);

  tft.initR(INITR_BLACKTAB); // 初始化ST7735，使用黑色TAB版本
  tft.setRotation(1); // 根据需要调整方向
  tft.fillScreen(CYBER_BG);

  // 初始化字体设置
  tft.setTextSize(1);


  connectWiFiAndSyncTime();

  // 初始化AHT21传感器，添加超时保护
  unsigned long initStartTime = millis();
  if (!aht.begin()) {
    Serial.println("AHT21 sensor not found");
    ahtInitialized = false;
  } else {
    Serial.println("AHT21 sensor initialized");
    ahtInitialized = true;
  }
  
  // 检查初始化是否花费了太长时间
  if((millis() - initStartTime) > 1000) {
    Serial.println("AHT21 initialization timeout");
  }
  
  // 初始化ENS160传感器，添加超时保护
  initStartTime = millis();
  if (!ens160.begin()) {
    Serial.println("ENS160 initialization failed");
    ens160Initialized = false;
  } else {
    // 设置ENS160为标准操作模式
    if (ens160.setMode(ENS160_OPMODE_STD)) {
      Serial.println("ENS160 sensor initialized and set to standard mode");
      ens160Initialized = true;
    } else {
      Serial.println("ENS160 setMode failed");
      ens160Initialized = false;
    }
  }
  
  // 检查初始化是否花费了太长时间
  if((millis() - initStartTime) > 1000) {
    Serial.println("ENS160 initialization timeout");
  }
  
  updateEnvSensors(true);

  initClockStaticUI();
  prevTimeStr = "";
  drawClockTime(getTimeStr('H'), getTimeStr('M'), getTimeStr('S'));
  drawEnvDynamic(curTemp, curHum, curTVOC, curECO2);
}

// ========= 主循环 =========
void loop() {
  static unsigned long lastWifiCheck = 0;
  if (millis() - lastWifiCheck > 10000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      // 尝试自动连接已保存的WiFi
      autoConnectWiFi();
    }
  }

  int encStep     = readEncoderStep();
  bool encPressed = checkButtonPressed(ENC_BTN_PIN, lastEncBtn);
  bool k0Pressed  = checkButtonPressed(KEY0_PIN,    lastKey0);

  checkAlarmTrigger();
  updateAlertStateAndLED();

  switch (currentMode) {
    case MODE_MENU: {
      if (encStep != 0) {
        menuIndex += encStep;
        if (menuIndex < 0) menuIndex = MENU_ITEMS - 1;
        if (menuIndex >= MENU_ITEMS) menuIndex = 0;
        drawMenu();
      }
      if (encPressed) {
        if (menuIndex == 0) {
          currentMode = MODE_CLOCK;
          initClockStaticUI();
          prevTimeStr = "";
          updateEnvSensors(true);
          drawClockTime(getTimeStr('H'), getTimeStr('M'), getTimeStr('S'));
          drawEnvDynamic(curTemp, curHum, curTVOC, curECO2);
        } else if (menuIndex == 1) {
          currentMode = MODE_ALARM;
          alarmSelectedField = 0;
          drawAlarmScreen(true);
        }
      }
      break;
    }

    case MODE_CLOCK: {
      struct tm timeinfo;
      if (getLocalTime(&timeinfo)) {
        int sec = timeinfo.tm_sec;
        if (sec != prevSecond) {
          prevSecond = sec;
          drawClockTime(getTimeStr('H'), getTimeStr('M'), getTimeStr('S'));
          if (sec % 5 == 0) {
            updateEnvSensors(true);
            drawEnvDynamic(curTemp, curHum, curTVOC, curECO2);
          }
        }
      }
      if (k0Pressed) {
        currentMode = MODE_MENU;
        drawMenu();
      }
      break;
    }


    case MODE_ALARM: {
      if (alarmRinging) {
        static unsigned long lastBeep = 0;
        if (millis() - lastBeep > 1000) {
          lastBeep = millis();
          tone(BUZZ_PIN, 2000, 400);
        }
        if (encPressed || k0Pressed) {
          alarmRinging = false;
          lastAlarmDayTriggered = -1;
          noTone(BUZZ_PIN);
          drawAlarmScreen(true);
        }
        break;
      }

      bool changed = false;
      if (encStep != 0) {
        if (alarmSelectedField == 0) {
          if (encStep > 0) alarmHour = (alarmHour + 1) % 24;
          else             alarmHour = (alarmHour + 23) % 24;
          changed = true;
        } else if (alarmSelectedField == 1) {
          if (encStep > 0) alarmMinute = (alarmMinute + 1) % 60;
          else             alarmMinute = (alarmMinute + 59) % 60;
          changed = true;
        } else if (alarmSelectedField == 2) {
          alarmEnabled = !alarmEnabled;
          changed = true;
        }

        if (changed) {
          lastAlarmDayTriggered = -1;
        }
      }

      if (encPressed) {
        alarmSelectedField = (alarmSelectedField + 1) % 3;
        changed = true;
      }

      if (k0Pressed) {
        currentMode = MODE_MENU;
        drawMenu();
        break;
      }

      if (changed) {
        drawAlarmScreen(false);
        drawAlarmIcon();
      }

      break;
    }


  }
}

// ===================== 配网函数实现 =====================

void initAPMode() {
  // 断开当前可能的WiFi连接
  WiFi.disconnect(true);
  delay(500);

  Serial.println("\n=== 进入AP配网模式 ===");
  Serial.printf("AP名称：%s\n", AP_SSID);
  Serial.printf("AP密码：%s\n", AP_PASSWORD);
  Serial.printf("配网IP：192.168.4.1\n");

  // 启动SoftAP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL);
  IPAddress apIP = WiFi.softAPIP();
  Serial.print("AP IP地址: ");
  Serial.println(apIP);

  // 设置Web服务器路由
  server.on("/", handleRoot);
  server.on("/config", handleConfig);
  server.on("/scan", HTTP_GET, []() {
    Serial.println("收到WiFi扫描请求");
    int n = WiFi.scanNetworks();
    String json = "{\"networks\":[";
    for (int i = 0; i < n; ++i) {
      if (i > 0) json += ",";
      String ssid = String(WiFi.SSID(i));
      // 转义引号以避免JSON格式错误
      ssid.replace("\"", "\\\"");
      json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + WiFi.RSSI(i) + ",\"secure\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true") + "}";
    }
    json += "]}";
    server.send(200, "application/json", json);
    Serial.println("发送扫描结果: " + json);
  });
  server.begin();
  Serial.println("Web服务器已启动，等待客户端连接...");

  tft.fillScreen(CYBER_BG);
  displayChinese(10, 30, "AP Config Mode", ST7735_WHITE);
  
  String ssidText = "SSID: " + String(AP_SSID);
  String pwdText = "PWD: " + String(AP_PASSWORD);
  
  displayChinese(10, 40, ssidText.c_str(), ST7735_WHITE);
  displayChinese(10, 50, pwdText.c_str(), ST7735_WHITE);
  displayChinese(10, 60, "IP: 192.168.4.1", ST7735_WHITE);
  displayChinese(10, 70, "Open Browser:", ST7735_WHITE);
  displayChinese(10, 80, "http://192.168.4.1", ST7735_WHITE);

  // 等待用户通过Web界面配置WiFi信息
  Serial.println("\n=== 等待用户通过Web界面配置WiFi信息 ===");
  
  unsigned long startTime = millis();
  while(millis() - startTime < 300000) {  // 等待5分钟
    server.handleClient();  // 处理Web请求
    delay(1);
  }
  
  Serial.println("=== AP配网超时 ===");
  WiFi.softAPdisconnect(true); // 断开AP
}

void scanSurroundingWiFi() {
  Serial.println("\n=== 开始扫描周边WiFi热点 ===");
  int n = WiFi.scanNetworks();
  if (n == 0) {
    Serial.println("未扫描到任何WiFi热点");
  } else {
    Serial.printf("扫描到 %d 个WiFi热点\n", n);
    for (int i = 0; i < n; i++) {
      Serial.printf("%d. SSID：%s \t 信号强度：%d dBm \t 加密方式：%d\n",
                    i + 1,
                    WiFi.SSID(i).c_str(),
                    WiFi.RSSI(i),
                    WiFi.encryptionType(i));
      delay(10);
    }
  }
  Serial.println("=== WiFi扫描完成 ===");
  WiFi.scanDelete(); // 清除扫描缓存
}

void saveWiFiToFlash(String ssid, String pwd) {
  preferences.begin("WiFi_Config", false);
  wifiRecordCount = preferences.getInt("RecordCount", 0);

  if (wifiRecordCount >= MAX_WIFI_RECORDS) {
    Serial.println("=== WiFi记录数已达上限，覆盖最旧记录 ===");
    wifiRecordCount = MAX_WIFI_RECORDS - 1;
  }

  String ssidKey = "SSID_" + String(wifiRecordCount);
  String pwdKey = "PWD_" + String(wifiRecordCount);
  preferences.putString(ssidKey.c_str(), ssid);
  preferences.putString(pwdKey.c_str(), pwd);

  wifiRecordCount++;
  preferences.putInt("RecordCount", wifiRecordCount);
  preferences.end();

  Serial.println("=== WiFi信息已保存到Flash ===");
}

void loadWiFiFromFlash() {
  preferences.begin("WiFi_Config", true);
  wifiRecordCount = preferences.getInt("RecordCount", 0);
  Serial.printf("\n=== 从Flash加载到 %d 条WiFi记录 ===\n", wifiRecordCount);

  for (int i = 0; i < wifiRecordCount; i++) {
    String ssidKey = "SSID_" + String(i);
    String pwdKey = "PWD_" + String(i);
    ssidList[i] = preferences.getString(ssidKey.c_str(), "");
    pwdList[i] = preferences.getString(pwdKey.c_str(), "");

    if (ssidList[i] != "") {
      Serial.printf("记录 %d：SSID=%s, PWD=%s\n",
                    i + 1,
                    ssidList[i].c_str(),
                    pwdList[i].c_str());
    }
  }
  preferences.end();
}

bool connectWiFi(String ssid, String pwd) {
  if (ssid == "" || pwd == "") {
    Serial.println("WiFi SSID或密码为空，无法连接");
    return false;
  }

  Serial.println("\n=== 开始连接WiFi ===");
  Serial.printf("目标SSID：%s\n", ssid.c_str());
  Serial.printf("目标密码：%s\n", pwd.c_str());

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(500);

  WiFi.begin(ssid.c_str(), pwd.c_str());

  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < WIFI_CONNECT_TIMEOUT) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("=== WiFi连接成功 ===");
    Serial.print("IP地址：");
    Serial.println(WiFi.localIP());
    Serial.print("子网掩码：");
    Serial.println(WiFi.subnetMask());
    Serial.print("网关：");
    Serial.println(WiFi.gatewayIP());
    Serial.print("MAC地址：");
    Serial.println(WiFi.macAddress());
    return true;
  } else {
    Serial.println("=== WiFi连接超时/失败 ===");
    return false;
  }
}

void autoConnectWiFi() {
  if (wifiRecordCount == 0) {
    Serial.println("=== 无已存储的WiFi记录，无法自动连接 ===");
    return;
  }

  Serial.println("\n=== 开始自动连接已存储的WiFi ===");
  for (int i = 0; i < wifiRecordCount; i++) {
    String currentSSID = ssidList[i];
    String currentPWD = pwdList[i];

    if (currentSSID == "") continue;

    Serial.printf("\n=== 尝试连接第 %d 条记录 ===", i + 1);
    if (connectWiFi(currentSSID, currentPWD)) {
      Serial.println("=== 自动连接WiFi成功 ===");
      return;
    } else {
      Serial.printf("=== 第 %d 条WiFi记录连接失败 ===\n", i + 1);
      clearInvalidWiFiRecord(currentSSID);
    }
  }

  Serial.println("=== 所有已存储WiFi均无法连接 ===");
}

void clearInvalidWiFiRecord(String invalidSsid) {
  preferences.begin("WiFi_Config", false);
  wifiRecordCount = preferences.getInt("RecordCount", 0);

  for (int i = 0; i < wifiRecordCount; i++) {
    String ssidKey = "SSID_" + String(i);
    String currentSSID = preferences.getString(ssidKey.c_str(), "");
    if (currentSSID == invalidSsid) {
      preferences.remove(ssidKey.c_str());
      preferences.remove(("PWD_" + String(i)).c_str());
      Serial.printf("=== 已清除无效WiFi记录：%s ===\n", invalidSsid.c_str());
      break;
    }
  }

  preferences.putInt("RecordCount", --wifiRecordCount);
  preferences.end();
  loadWiFiFromFlash();
}

void connectWiFiAndSyncTime() {
  tft.fillScreen(CYBER_BG);
  displayChinese(20, 110, "Loading WiFi Config", CYBER_LIGHT);
  
  // 1. 从Flash加载已保存的WiFi信息
  loadWiFiFromFlash();

  // 2. 尝试自动连接已存储的WiFi
  autoConnectWiFi();
  
  // 3. 如果自动连接失败，进入AP配网模式
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("=== 自动连接失败，进入AP配网模式 ===");
    initAPMode();
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    tft.fillScreen(CYBER_BG);
    displayChinese(20, 110, "Syncing Time...", CYBER_LIGHT);
    delay(800);
  } else {
    tft.fillScreen(CYBER_BG);
    displayChinese(20, 110, "WiFi Config Failed!", ST7735_RED);
    delay(1000);
  }
}

// Web服务器处理函数
void sendConfigPage() {
  String html = "<!DOCTYPE html>\n";
  html += "<html><head><title>WiFi Configuration</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 40px; background-color: #f0f0f0; color: black; }";
  html += "h1 { color: black; text-align: center; }";
  html += ".container { max-width: 400px; margin: 0 auto; padding: 20px; background: white; border-radius: 8px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
  html += "input[type='text'], input[type='password'] { width: 100%; padding: 12px; margin: 10px 0; box-sizing: border-box; border: 1px solid #ddd; border-radius: 4px; color: black; }";
  html += "input[type='submit'] { width: 100%; background-color: #4CAF50; color: white; padding: 14px 20px; margin: 10px 0; border: none; border-radius: 4px; cursor: pointer; }";
  html += "input[type='submit']:hover { background-color: #45a049; }";
  html += "button { width: 100%; background-color: #2196F3; color: white; padding: 14px 20px; margin: 10px 0; border: none; border-radius: 4px; cursor: pointer; }";
  html += "button:hover { background-color: #0b7dda; }";
  html += "select { width: 100%; padding: 12px; margin: 10px 0; box-sizing: border-box; border: 1px solid #ddd; border-radius: 4px; color: black; }";
  html += "label { color: black; }";
  html += "</style>";
  html += "<script>";
  html += "function scanWiFi() {";
  html += "  document.getElementById('scanBtn').disabled = true;";
  html += "  document.getElementById('scanBtn').value = 'Scanning...';";
  html += "  fetch('/scan')";
  html += "    .then(response => response.json())";
  html += "    .then(data => {";
  html += "      let select = document.getElementById('ssid');";
  html += "      select.innerHTML = '';";
  html += "      data.networks.forEach(network => {";
  html += "        let option = document.createElement('option');";
  html += "        option.value = network.ssid;";
  html += "        option.textContent = network.ssid + ' (' + network.rssi + ' dBm)';";
  html += "        select.appendChild(option);";
  html += "      });";
  html += "      document.getElementById('scanBtn').disabled = false;";
  html += "      document.getElementById('scanBtn').value = 'Rescan';";
  html += "    })";
  html += "    .catch(error => {";
  html += "      alert('Scan failed: ' + error);";
  html += "      document.getElementById('scanBtn').disabled = false;";
  html += "      document.getElementById('scanBtn').value = 'Rescan';";
  html += "    });";
  html += "}";
  html += "</script>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h1>WiFi Configuration</h1>";
  html += "<form action='/config' method='POST'>";
  html += "<label for='ssid'>WiFi Name:</label>";
  html += "<select id='ssid' name='ssid' required></select><br>";
  html += "<label for='password'>WiFi Password:</label>";
  html += "<input type='password' id='password' name='password'><br>";
  html += "<input type='submit' value='Connect'>";
  html += "</form>";
  html += "<button type='button' id='scanBtn' onclick='scanWiFi()'>Scan WiFi</button>";
  html += "</div>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleRoot() {
  Serial.println("收到根路径请求");
  sendConfigPage();
}

void handleConfig() {
  Serial.println("收到配置请求");
  
  String ssid = server.arg("ssid");
  String password = server.arg("password");
  
  Serial.printf("收到WiFi配置: SSID=%s, Password=%s\n", ssid.c_str(), password.c_str());
  
  if (ssid.length() > 0) {
    // 保存WiFi信息到Flash
    saveWiFiToFlash(ssid, password);
    
    // 尝试连接WiFi
    if (connectWiFi(ssid, password)) {
      Serial.println("=== Web配网成功，已连接目标WiFi ===");
      
      // 发送成功页面
      String successHtml = "<!DOCTYPE html>\n";
      successHtml += "<html><head><title>Configuration Success</title>";
      successHtml += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
      successHtml += "<style>";
      successHtml += "body { font-family: Arial, sans-serif; margin: 40px; text-align: center; color: black; }";
      successHtml += ".success { color: green; font-size: 18px; }";
      successHtml += "</style>";
      successHtml += "</head><body>";
      successHtml += "<h1>Configuration Success!</h1>";
      successHtml += "<p class='success'>Connected to WiFi: " + ssid + "</p>";
      successHtml += "<p>Device will restart or return to normal mode in a few seconds</p>";
      successHtml += "</body></html>";
      
      server.send(200, "text/html", successHtml);
      
      delay(2000);
      WiFi.softAPdisconnect(true); // 断开AP，节省资源
      return;
    } else {
      Serial.println("=== Web配网失败，目标WiFi无法连接 ===");
      
      // 发送错误页面
      String errorHtml = "<!DOCTYPE html>\n";
      errorHtml += "<html><head><title>Configuration Failed</title>";
      errorHtml += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
      errorHtml += "<style>";
      errorHtml += "body { font-family: Arial, sans-serif; margin: 40px; text-align: center; color: black; }";
      errorHtml += ".error { color: red; font-size: 18px; }";
      errorHtml += "</style>";
      errorHtml += "</head><body>";
      errorHtml += "<h1>Configuration Failed!</h1>";
      errorHtml += "<p class='error'>Cannot connect to WiFi: " + ssid + "</p>";
      errorHtml += "<p><a href='/'>Return to configuration page</a></p>";
      errorHtml += "</body></html>";
      
      server.send(200, "text/html", errorHtml);
    }
  } else {
    // 如果没有收到参数，发送配置页面
    sendConfigPage();
  }
}