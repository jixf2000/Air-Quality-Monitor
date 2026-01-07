#include <WiFi.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSans24pt7b.h>
#include <Wire.h>
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

// ===================== Web服务器对象 =====================
WebServer server(80);           // AP模式下的Web服务器
WebServer backgroundServer(8080);  // 后台配置Web服务器（正常工作时使用）

// ====== 引脚定义 ======
#define TFT_CS   10
#define TFT_DC   11
#define TFT_RST  12
#define TFT_MOSI 13
#define TFT_MISO -1  // ILI9341通常不需要MISO
#define TFT_SCLK 9

#define ENC_A_PIN    14
#define ENC_B_PIN    20
#define ENC_BTN_PIN  21
#define KEY0_PIN     0

#define SDA_PIN 1
#define SCL_PIN 2

#define LED_PIN 4
#define BUZZ_PIN 3

// ====== TFT和传感器对象 ======
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);

// 环境传感器对象实例化
Adafruit_AHTX0 aht21;  // AHT21温湿度传感器对象
ScioSense_ENS160 ens160(0x53);         // ENS160空气质量传感器对象


// ====== 中文字体支持 ======
// 为支持中文字符定义的映射表




// ====== 颜色定义 ======
#define CYBER_BG      ILI9341_WHITE
#define CYBER_GREEN   ILI9341_GREEN  
#define CYBER_ACCENT  ILI9341_CYAN  
#define CYBER_LIGHT   0xFD20  
#define CYBER_BLUE    ILI9341_BLUE  
#define CYBER_PINK    ILI9341_MAGENTA

#define AQ_BAR_GREEN  ILI9341_GREEN    // 0x07E0
#define AQ_BAR_YELLOW ILI9341_YELLOW   // 0xFFE0
#define AQ_BAR_ORANGE 0xFD20            // 自定义橙色
#define AQ_BAR_RED    ILI9341_RED      // 0xF800
#define CYBER_DARK    0x4208           // 深色主题

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

// 温度校准偏移量（在实际读数上加上这个值来校准）
float TEMP_CALIBRATION_OFFSET = 0.0;  // 可以根据实际校准情况调整此值

// 环境数据结构体定义
typedef struct {
  float temperature;    // 温度（℃）
  float humidity;       // 湿度（%RH）
  uint8_t aqi;          // 空气质量指数（1-5，对应优-差）
  uint16_t tvoc;        // 总挥发性有机化合物（ppb）
  uint16_t eco2;        // 等效二氧化碳（ppm）
  uint8_t sensor_status;// ENS160传感器状态
} EnvData_t;

EnvData_t current_env_data;  // 当前环境数据存储变量

// ====== 时钟变量 ======
int    prevSecond  = -1;
String prevTimeStr = "";
struct tm cachedTimeInfo;
bool timeCacheValid = false;
unsigned long lastTimeCacheUpdate = 0;

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
// 注意：使用Adafruit GFX库显示字符
void displayChinese(int x, int y, const char* text, uint16_t color = ILI9341_WHITE, uint16_t bgColor = CYBER_BG) {
  // 使用FreeSans字体以获得更平滑的显示效果
  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(color, bgColor);
  tft.setCursor(x, y);
  tft.print(text);
  // 恢复默认字体
  tft.setFont();
}

void displayChineseSmooth(int x, int y, const char* text, uint16_t color, uint16_t bgColor, bool largeFont) {
  // 使用FreeSans字体以获得更平滑的显示效果
  if(largeFont) {
    tft.setFont(&FreeSans18pt7b);
  } else {
    tft.setFont(&FreeSans12pt7b);
  }
  tft.setTextColor(color, bgColor);
  tft.setCursor(x, y);
  tft.print(text);
  // 恢复默认字体
  tft.setFont();
}

// 重载函数，使用默认参数
void displayChineseSmooth(int x, int y, const char* text, uint16_t color, uint16_t bgColor) {
  displayChineseSmooth(x, y, text, color, bgColor, false);
}

// 重载函数，使用默认参数
void displayChineseSmooth(int x, int y, const char* text, uint16_t color) {
  displayChineseSmooth(x, y, text, color, CYBER_BG, false);
}

// 重载函数，使用默认参数
void displayChineseSmooth(int x, int y, const char* text) {
  displayChineseSmooth(x, y, text, ILI9341_WHITE, CYBER_BG, false);
}

void displayChineseCenterSmooth(int x0, int x1, int y, const char* text, uint16_t color, uint16_t bgColor, bool largeFont) {
  // 使用FreeSans字体以获得更平滑的显示效果
  if(largeFont) {
    tft.setFont(&FreeSans18pt7b);
  } else {
    tft.setFont(&FreeSans12pt7b);
  }
  
  int16_t x, y1;
  uint16_t w, h;
  tft.getTextBounds((char*)text, 0, 0, &x, &y1, &w, &h);
  
  int x_pos = x0 + ((x1 - x0) - w) / 2;
  
  tft.setTextColor(color, bgColor);
  tft.setCursor(x_pos, y);
  tft.print(text);
  // 恢复默认字体
  tft.setFont();
}

// 重载函数，使用默认参数
void displayChineseCenterSmooth(int x0, int x1, int y, const char* text, uint16_t color, uint16_t bgColor) {
  displayChineseCenterSmooth(x0, x1, y, text, color, bgColor, false);
}

// 重载函数，使用默认参数
void displayChineseCenterSmooth(int x0, int x1, int y, const char* text, uint16_t color) {
  displayChineseCenterSmooth(x0, x1, y, text, color, CYBER_BG, false);
}

// 重载函数，使用默认参数
void displayChineseCenterSmooth(int x0, int x1, int y, const char* text) {
  displayChineseCenterSmooth(x0, x1, y, text, ILI9341_WHITE, CYBER_BG, false);
}

void displayChineseCenter(int x0, int x1, int y, const char* text, uint16_t color = ILI9341_WHITE, uint16_t bgColor = CYBER_BG) {
  // 使用平滑字体版本
  displayChineseCenterSmooth(x0, x1, y, text, color, bgColor, false);
}

// 专门用于小字体标签的函数
void displayChineseCenterSmall(int x0, int x1, int y, const char* text, uint16_t color = ILI9341_WHITE, uint16_t bgColor = CYBER_BG) {
  tft.setFont(&FreeSans9pt7b);
  
  int16_t x, y1;
  uint16_t w, h;
  tft.getTextBounds((char*)text, 0, 0, &x, &y1, &w, &h);
  
  int x_pos = x0 + ((x1 - x0) - w) / 2;
  
  tft.setTextColor(color, bgColor);
  tft.setCursor(x_pos, y);
  tft.print(text);
  
  // 恢复默认字体
  tft.setFont();
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
    displayChinese(20, 110, "WiFi Connect Failed!", ILI9341_RED);
    delay(1000);
  }
}

String getTimeStr(char type) {
  unsigned long currentTime = millis();
  // 每秒最多更新一次时间缓存，避免频繁调用getLocalTime造成阻塞
  if (!timeCacheValid || currentTime - lastTimeCacheUpdate >= 1000) {
    if (getLocalTime(&cachedTimeInfo)) {
      timeCacheValid = true;
      lastTimeCacheUpdate = currentTime;
    } else {
      // 如果无法获取时间，返回错误标记
      return "--";
    }
  }
  
  if (!timeCacheValid) {
    return "--";
  }
  
  char buf[8];
  if (type == 'H') strftime(buf, sizeof(buf), "%H", &cachedTimeInfo);
  else if (type == 'M') strftime(buf, sizeof(buf), "%M", &cachedTimeInfo);
  else if (type == 'S') strftime(buf, sizeof(buf), "%S", &cachedTimeInfo);
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
void handleRoot();                   // 处理根路径请求
void handleConfig();                 // 处理配置请求
void sendConfigPage();               // 发送配置页面

// 为了解决函数声明问题，重新声明原始的WiFi连接函数
void connectWiFiAndSyncTimeOriginal();

// ========= 环境传感器读取 =========

// 环境数据更新间隔
const unsigned long ENV_DATA_UPDATE_INTERVAL = 2000;  // 环境数据更新间隔：2秒


// 环境传感器状态枚举
enum SensorState {
  SENSOR_AHT21,
  SENSOR_ENS160
};

// 传感器状态机变量
SensorState current_sensor = SENSOR_AHT21;
unsigned long last_sensor_access = 0;

// 传感器访问超时和重试机制
unsigned long aht21_last_access = 0;
unsigned long ens160_last_access = 0;
bool aht21_initialized = false;
bool ens160_initialized = false;
int aht21_error_count = 0;
int ens160_error_count = 0;

// ========= 传感器初始化函数 =========
bool env_sensor_init() {
  aht21_initialized = false;
  ens160_initialized = false;
  // 初始化I2C总线（默认GPIO21=SDA, GPIO22=SCL）
  Wire.begin(SDA_PIN, SCL_PIN);
  delay(100); // 等待I2C总线稳定
  
  // 初始化AHT21温湿度传感器
  if (!aht21.begin()) {
    Serial.println("错误：无法找到AHT21传感器，请检查接线！");
    
    // 尝试多次初始化
    for (int attempts = 0; attempts < 3; attempts++) {
      Serial.printf("AHT21初始化尝试 %d/3...\n", attempts + 1);
      delay(1000);
      if (aht21.begin()) {
        Serial.println("AHT21传感器初始化成功");
        aht21_initialized = true;
        break;
      }
      if (attempts == 2) {  // 最后一次尝试失败
        Serial.println("AHT21传感器初始化失败，继续执行程序");
      }
    }
  } else {
    Serial.println("AHT21传感器初始化成功");
    aht21_initialized = true;
  }

  delay(100); // 避免I2C总线冲突
  
  // 初始化ENS160空气质量传感器
  if (!ens160.begin()) {
    Serial.println("错误：无法找到ENS160传感器，请检查接线！");
    
    // 尝试多次初始化
    for (int attempts = 0; attempts < 3; attempts++) {
      Serial.printf("ENS160初始化尝试 %d/3...\n", attempts + 1);
      delay(1000);
      if (ens160.begin()) {
        Serial.println("ENS160传感器初始化成功");
        ens160_initialized = true;
        break;
      }
      if (attempts == 2) {  // 最后一次尝试失败
        Serial.println("ENS160传感器初始化失败，继续执行程序");
        // 不再返回false，允许程序继续运行，只是没有空气质量数据
      }
    }
  } else {
    Serial.println("ENS160传感器初始化成功");
    ens160_initialized = true;
  }

  delay(100); // 避免I2C总线冲突
  
  // 配置ENS160工作模式（正常模式，低功耗，适合长期监测）
  if (ens160.setMode(ENS160_OPMODE_STD)) {
    Serial.println("ENS160设置为标准模式");
  } else {
    Serial.println("ENS160模式设置失败");
  }
  
  // 初始化环境数据结构体（默认值）
  current_env_data.temperature = 0.0f;
  current_env_data.humidity = 0.0f;
  current_env_data.aqi = 0;
  current_env_data.tvoc = 0;
  current_env_data.eco2 = 400;  // eCO2的默认值
  current_env_data.sensor_status = 0;

  return true;
}

// ========= 时钟界面 =========
// ILI9341分辨率为320x240，与ST7789相同，保持坐标不变

// 非阻塞式环境数据采集函数（使用状态机避免I2C冲突）
void collect_env_data_non_blocking() {
  unsigned long current_time = millis();
  
  // 确保两次传感器访问之间有足够的时间间隔
  if (current_time - last_sensor_access < 50) {  // 增加间隔时间，避免传感器过载
    return; // 如果时间间隔太短，直接返回
  }
  
  switch (current_sensor) {
    case SENSOR_AHT21:
      {
        // 检查AHT21是否已初始化
        if (aht21_initialized) {
          sensors_event_t temp_event, hum_event;
          

          
          // 读取AHT21数据
          if (aht21.getEvent(&temp_event, &hum_event)) {
            // 检查读取的数据是否有效
            if (temp_event.temperature > -100 && temp_event.temperature < 100 && 
                hum_event.relative_humidity >= 0 && hum_event.relative_humidity <= 100) {
              // 更新环境数据（应用温度校准）
              current_env_data.temperature = temp_event.temperature + TEMP_CALIBRATION_OFFSET;
              current_env_data.humidity = hum_event.relative_humidity;
              aht21_error_count = 0;  // 重置错误计数
            } else {
              Serial.println("警告：AHT21读取到无效数据");
              aht21_error_count++;
            }
          } else {
            Serial.println("警告：AHT21数据采集失败");
            aht21_error_count++;
            
            // 如果连续错误次数过多，尝试重新初始化
            if (aht21_error_count > 5) {
              Serial.println("AHT21连续错误过多，尝试重新初始化");
              aht21.begin();  // 尝试重新初始化
              aht21_error_count = 0;  // 重置错误计数
            }
          }
          
          // 更新最后访问时间
          aht21_last_access = current_time;
        }
        
        // 切换到下一个传感器
        current_sensor = SENSOR_ENS160;
        last_sensor_access = current_time;
      }
      break;
      
    case SENSOR_ENS160:
      {
        // 检查ENS160是否已初始化
        if (ens160_initialized) {
          // 设置环境数据（温度和湿度来自AHT21传感器）
          if (current_env_data.temperature != 0.0f || current_env_data.humidity != 0.0f) {
            if (!ens160.set_envdata(current_env_data.temperature, current_env_data.humidity)) {
              Serial.println("警告：ENS160环境数据设置失败");
            }
          }
          
          // 读取ENS160数据
          if (ens160.measure()) {
            // 读取空气质量数据
            current_env_data.aqi = ens160.getAQI();        // 空气质量指数
            current_env_data.tvoc = ens160.getTVOC();      // TVOC浓度
            current_env_data.eco2 = ens160.geteCO2();      // eCO2浓度
            current_env_data.sensor_status = 0;  // 成功读取
            ens160_error_count = 0;  // 重置错误计数
          } else {
            Serial.println("警告：ENS160传感器读取失败");
            // 异常时重置数据，避免界面显示脏数据
            current_env_data.aqi = 0;
            current_env_data.tvoc = 0;
            current_env_data.eco2 = 400;  // 默认值
            ens160_error_count++;
            
            // 如果连续错误次数过多，尝试重新初始化
            if (ens160_error_count > 5) {
              Serial.println("ENS160连续错误过多，尝试重新初始化");
              ens160.begin();  // 尝试重新初始化
              ens160.setMode(ENS160_OPMODE_STD);  // 重新设置模式
              ens160_error_count = 0;  // 重置错误计数
            }
          }
          
          // 更新最后访问时间
          ens160_last_access = current_time;
        }
        
        // 切换到下一个传感器
        current_sensor = SENSOR_AHT21;
        last_sensor_access = current_time;
      }
      break;
  }
}

// 非阻塞式环境数据显示更新函数
void update_env_display() {
  // 检查是否需要更新显示
  static unsigned long last_display_update = 0;
  if (millis() - last_display_update > 5000) {  // 每5秒更新一次显示
    // 在时钟模式下更新环境数据显示
    if (currentMode == MODE_CLOCK) {
      drawEnvDynamic(current_env_data.temperature, current_env_data.humidity, current_env_data.tvoc, current_env_data.eco2);
    }
    last_display_update = millis();
  }
}

// 安全的传感器检查函数
void check_sensors_health() {
  // 定期检查传感器健康状态，如果长时间没有数据更新，尝试重新初始化
  static unsigned long last_health_check = 0;
  unsigned long current_time = millis();
  
  if (current_time - last_health_check > 30000) {  // 每30秒检查一次传感器健康状态
    last_health_check = current_time;
    
    // 检查是否长时间没有传感器数据更新
    if (current_env_data.eco2 == 400 && current_env_data.temperature == 0.0f && current_env_data.humidity == 0.0f) {
      // 如果传感器数据保持默认值，说明可能传感器初始化失败或通信异常
      Serial.println("检测到传感器数据异常，尝试重新初始化...");
      
      // 重新初始化I2C总线
      Wire.begin(SDA_PIN, SCL_PIN);
      delay(100);
      
      // 重新初始化AHT21
      if (!aht21_initialized) {
        if (aht21.begin()) {
          aht21_initialized = true;
          Serial.println("AHT21重新初始化成功");
        }
      }
      
      // 重新初始化ENS160
      if (!ens160_initialized) {
        if (ens160.begin()) {
          ens160.setMode(ENS160_OPMODE_STD);
          ens160_initialized = true;
          Serial.println("ENS160重新初始化成功");
        }
      }
    }
  }
}


// 环境数据汇总打印函数（用于调试，可替换为界面显示逻辑）
void print_env_data() {
  Serial.println("==================== 环境监测数据 ====================");
  Serial.printf("温度：%.2f ℃\n", current_env_data.temperature);
  Serial.printf("湿度：%.2f %%RH\n", current_env_data.humidity);
  Serial.printf("空气质量指数（AQI）：%d （1=优，2=良，3=中，4=差，5=劣）\n", current_env_data.aqi);
  Serial.printf("总挥发性有机物（TVOC）：%d ppb\n", current_env_data.tvoc);
  Serial.printf("等效二氧化碳（eCO2）：%d ppm\n", current_env_data.eco2);
  Serial.printf("传感器状态：%d （0=正常）\n", current_env_data.sensor_status);
  Serial.println("======================================================\n");
}

const int GRID_L   = 8;
const int GRID_R   = 312;
const int GRID_TOP = 112;
const int GRID_MID = 160;
const int GRID_BOT = 208;
const int GRID_MID_X = (GRID_L + GRID_R) / 2;

const int TOP_LABEL_Y      = GRID_TOP + 12;
const int TOP_VALUE_Y      = GRID_TOP + 34;
const int BOTTOM_LABEL_Y   = GRID_MID + 12;
const int BOTTOM_VALUE_Y   = GRID_MID + 34;
const int TVOC_LABEL_Y     = GRID_MID + 24;
const int TVOC_VALUE_Y     = GRID_MID + 40;
const int CO2_LABEL_Y      = GRID_MID + 24;
const int CO2_VALUE_Y      = GRID_MID + 40;

const int BAR_MARGIN_X = 4;
const int BAR_GAP      = 4;
const int BAR_Y        = 220;
const int BAR_H        = 12;
const int BAR_W        = (320 - 2 * BAR_MARGIN_X - 3 * BAR_GAP) / 4;

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
  tft.setTextSize(1); // 恢复默认字体大小为1
}



void initClockStaticUI() {
  tft.fillScreen(CYBER_BG);
  
  displayChinese(8, 16, "Environment Monitor", CYBER_LIGHT);
  // displayChinese(8, 92, "Air Quality:", ILI9341_BLACK);  // 注释掉Air Quality标签

  tft.drawFastHLine(GRID_L, GRID_TOP, GRID_R - GRID_L, ILI9341_WHITE);
  tft.drawFastHLine(GRID_L, GRID_MID, GRID_R - GRID_L, ILI9341_WHITE);
  tft.drawFastHLine(GRID_L, GRID_BOT, GRID_R - GRID_L, ILI9341_WHITE);
  tft.drawFastVLine(GRID_MID_X, GRID_TOP, GRID_BOT - GRID_TOP, ILI9341_WHITE);

  // 环境参数标签
  displayChineseCenterSmall(GRID_L, GRID_MID_X, TOP_LABEL_Y, "Humidity", ILI9341_YELLOW, CYBER_BG);
  displayChineseCenterSmall(GRID_MID_X, GRID_R, TOP_LABEL_Y, "Temperature", ILI9341_YELLOW, CYBER_BG);
  displayChineseCenterSmall(GRID_L, GRID_MID_X, TVOC_LABEL_Y, "TVOC", ILI9341_YELLOW, CYBER_BG);
  displayChineseCenterSmall(GRID_MID_X, GRID_R, CO2_LABEL_Y, "CO2", ILI9341_YELLOW, CYBER_BG);
  
  // 空气质量条形图
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

  tft.setTextSize(6);
  int16_t x1, y1;
  uint16_t w, h;
  tft.getTextBounds(cur, 0, 0, &x1, &y1, &w, &h);
  int x = (320 - w) / 2;
  int y = 36;

  tft.setTextColor(CYBER_LIGHT, CYBER_BG);
  tft.setCursor(x, y);
  tft.print(cur);
}

// 环境数据显示函数
uint16_t colorForCO2(uint16_t eco2) {
  if (eco2 <= 800)  return AQ_BAR_GREEN;
  if (eco2 <= 1200) return AQ_BAR_YELLOW;
  if (eco2 <= 1800) return AQ_BAR_ORANGE;
  return AQ_BAR_RED;
}

void drawCO2Value(uint16_t eco2, uint16_t col) {
  char co2Buf[12];
  sprintf(co2Buf, "%4uppm", eco2);
  printCenteredText(String(co2Buf),
                    GRID_MID_X, GRID_R,
                    CO2_VALUE_Y,
                    col, CYBER_BG, 2);
}

void drawEnvDynamic(float temp, float hum, uint16_t tvoc, uint16_t eco2) {
  uint16_t colHUMI = CYBER_BLUE;
  uint16_t colTEMP = CYBER_BLUE;
  uint16_t colTVOC = CYBER_BLUE;
  uint16_t colCO2  = CYBER_BLUE;

  char humBuf[8];
  sprintf(humBuf, "%2.0f%%", hum);
  printCenteredText(String(humBuf),
                    GRID_L, GRID_MID_X,
                    TOP_VALUE_Y,
                    colHUMI, CYBER_BG, 2);

  char tempBuf[10];
  sprintf(tempBuf, "%2.1fC", temp);
  printCenteredText(String(tempBuf),
                    GRID_MID_X, GRID_R,
                    TOP_VALUE_Y,
                    colTEMP, CYBER_BG, 2);

  float tvoc_mg = tvoc / 1000.0f;
  char tvocBuf[16];
  sprintf(tvocBuf, "%.3f mg/m3", tvoc_mg);
  printCenteredText(String(tvocBuf),
                    GRID_L, GRID_MID_X,
                    TVOC_VALUE_Y,
                    colTVOC, CYBER_BG, 2);

  drawCO2Value(eco2, colCO2);

  uint8_t level = 1;
  if (eco2 > 1800) level = 4;
  else if (eco2 > 1200) level = 3;
  else if (eco2 > 800) level = 2;
  
  int x = BAR_MARGIN_X;
  for (int i = 0; i < 4; i++) {
    uint16_t barCol = (i < level) ? colorForCO2((i + 1) * 600) : CYBER_DARK;
    tft.fillRect(x + i * (BAR_W + BAR_GAP), BAR_Y, BAR_W, BAR_H, barCol);
  }
}





// ========= 菜单界面 =========
const char* menuItemsEN[] = {
  "Environment",
  "Alarm"
};

void drawMenu() {
  tft.fillScreen(CYBER_BG);

  displayChinese(20, 20, "Mode Select", CYBER_LIGHT);

  for (int i = 0; i < MENU_ITEMS; i++) {
    int y = 64 + i * 36;
    if (i == menuIndex) {
      tft.fillRect(12, y - 4, 296, 28, CYBER_ACCENT);
      displayChinese(24, y, menuItemsEN[i], CYBER_BG, CYBER_ACCENT);
    } else {
      tft.fillRect(12, y - 4, 296, 28, CYBER_BG);
      displayChinese(24, y, menuItemsEN[i], ILI9341_BLACK);
    }
  }
  drawAlarmIcon();
}


// ========= 闹钟界面 =========
void drawAlarmScreen(bool full) {
  if (full) {
    tft.fillScreen(CYBER_BG);
    displayChinese(16, 8, "Alarm Setup", CYBER_LIGHT);
    drawAlarmIcon();
  }

  char hBuf[3];
  char mBuf[3];
  sprintf(hBuf, "%02d", alarmHour);
  sprintf(mBuf, "%02d", alarmMinute);

  tft.setTextSize(6);

  String disp = String(hBuf) + ":" + String(mBuf);
  int16_t x1, y1;
  uint16_t w, h;
  tft.getTextBounds(disp, 0, 0, &x1, &y1, &w, &h);
  int x = (320 - w) / 2;
  int y = 60;

  tft.setCursor(x, y);
  tft.setTextColor(alarmSelectedField == 0 ? CYBER_LIGHT : ILI9341_BLACK, CYBER_BG);
  tft.print(hBuf);

  tft.setTextColor(ILI9341_BLACK, CYBER_BG);
  tft.print(":");

  tft.setTextColor(alarmSelectedField == 1 ? CYBER_LIGHT : ILI9341_BLACK, CYBER_BG);
  tft.print(mBuf);

  tft.setTextSize(2);
  tft.setTextColor(ILI9341_BLACK, CYBER_BG);
  tft.fillRect(40, 160, 240, 48, CYBER_BG);
  
  displayChinese(60, 168, "Alarm:", ILI9341_BLACK);
  
  uint16_t enColor = alarmSelectedField == 2
                     ? CYBER_LIGHT
                     : (alarmEnabled ? CYBER_GREEN : ILI9341_RED);
  
  if (alarmEnabled) {
    displayChinese(160, 168, "ON", enColor);
  } else {
    displayChinese(160, 168, "OFF", enColor);
  }
}

void drawAlarmRingingScreen() {
  tft.fillScreen(ILI9341_RED);
  displayChineseCenter(0, 320, 80, "Alarm Ringing!", ILI9341_WHITE, ILI9341_RED);
}

// ========= 闹钟逻辑 =========
void checkAlarmTrigger() {
  if (!alarmEnabled || alarmRinging) return;
  
  // 使用时间缓存以避免频繁调用getLocalTime
  unsigned long currentTime = millis();
  if (!timeCacheValid || currentTime - lastTimeCacheUpdate >= 1000) {
    if (getLocalTime(&cachedTimeInfo)) {
      timeCacheValid = true;
      lastTimeCacheUpdate = currentTime;
    } else {
      return; // 如果无法获取时间，则返回
    }
  }
  
  // 检查闹钟时间（仅在秒为0时触发，避免重复触发）
  if (cachedTimeInfo.tm_hour == alarmHour &&
      cachedTimeInfo.tm_min  == alarmMinute &&
      cachedTimeInfo.tm_sec  == 0) {
    
    alarmRinging = true;
    lastAlarmDayTriggered = cachedTimeInfo.tm_mday;
    currentMode = MODE_ALARM;
    drawAlarmRingingScreen();
  }
}

// ========= 警报视觉+声音 =========

void updateAlertStateAndLED() {
  if (alarmRinging) currentAlertLevel = ALERT_ALARM;
  else if (ens160_initialized && current_env_data.eco2 > 1800) currentAlertLevel = ALERT_CO2;  // 只有在ENS160初始化成功时才检查CO2警报
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
      uint16_t baseCol = current_env_data.eco2 <= 800 ? AQ_BAR_GREEN : 
                        current_env_data.eco2 <= 1200 ? AQ_BAR_YELLOW :
                        current_env_data.eco2 <= 1800 ? AQ_BAR_ORANGE : AQ_BAR_RED;
      uint16_t col = co2BlinkOn ? baseCol : CYBER_DARK;
      drawCO2Value(current_env_data.eco2, col);
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
  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);  // 为ILI9341配置SPI引脚

  tft.begin(); // 初始化ILI9341，无需指定分辨率
  tft.setRotation(1);
  tft.fillScreen(CYBER_BG);

  // 初始化字体设置
  tft.setTextSize(1);


  connectWiFiAndSyncTime();

  // 初始化环境传感器
  env_sensor_init();  // 不再根据返回值判断是否失败，因为即使传感器初始化失败，程序也应继续运行
  Serial.println("环境传感器初始化完成");

  // 初始化后台Web服务器
  initBackgroundWebServer();

  initClockStaticUI();
  prevTimeStr = "";
  
  // 使用与MODE_CLOCK中相同的逻辑来显示时间，检查时间是否有效
  String hourStr = getTimeStr('H');
  String minStr = getTimeStr('M');
  String secStr = getTimeStr('S');
  
  // 检查时间是否有效（避免显示"--:--:--"）
  if (hourStr != "--" && minStr != "--" && secStr != "--") {
    // 只有当时间有效时才绘制时钟
    drawClockTime(hourStr, minStr, secStr);
  } else {
    // 如果时间无效，显示提示信息
    Serial.println("启动时时间无效，等待时间同步完成");
  }
  
  drawEnvDynamic(current_env_data.temperature, current_env_data.humidity, current_env_data.tvoc, current_env_data.eco2);
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
  
  // 核心：非阻塞式采集（使用状态机避免I2C总线冲突）
  collect_env_data_non_blocking();
  
  // 检查传感器健康状态
  check_sensors_health();
  
  // 更新环境数据显示
  update_env_display();

  // 其他任务：如界面刷新、数据上传等（可在此添加，不会被采集操作阻塞）
  // 示例：每10秒打印一次汇总数据（避免串口刷屏）
  static unsigned long last_print_time = 0;
  if (millis() - last_print_time >= 10000) {
    print_env_data();
    last_print_time = millis();
  }

  updateAlertStateAndLED();
  
  // 处理后台Web服务器请求（在STA模式下）
  if (WiFi.status() == WL_CONNECTED) {
    backgroundServer.handleClient();
  }

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
          drawClockTime(getTimeStr('H'), getTimeStr('M'), getTimeStr('S'));
          drawEnvDynamic(current_env_data.temperature, current_env_data.humidity, current_env_data.tvoc, current_env_data.eco2);
        } else if (menuIndex == 1) {
          currentMode = MODE_ALARM;
          alarmSelectedField = 0;
          drawAlarmScreen(true);
        }
      }
      break;
    }

    case MODE_CLOCK: {
      // 使用统一的时间获取函数，该函数使用缓存机制避免频繁调用getLocalTime
      String hourStr = getTimeStr('H');
      String minStr = getTimeStr('M');
      String secStr = getTimeStr('S');
      
      // 检查时间是否有效（避免显示"--:--:--"）
      if (hourStr != "--" && minStr != "--" && secStr != "--") {
        // 只有当时间有效时才绘制时钟
        drawClockTime(hourStr, minStr, secStr);
      } else {
        // 如果时间无效，显示错误信息
        Serial.println("时间无效，显示默认时间");
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
    Serial.println("WiFi scan request received");
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
    Serial.println("Sending scan results: " + json);
  });
  server.begin();
  Serial.println("Web server started, waiting for client connections...");

  tft.fillScreen(CYBER_BG);
  displayChinese(20, 60, "AP Config Mode", ILI9341_BLACK);
  
  String ssidText = "SSID: " + String(AP_SSID);
  String pwdText = "PWD: " + String(AP_PASSWORD);
  
  displayChinese(20, 80, ssidText.c_str(), ILI9341_BLACK);
  displayChinese(20, 100, pwdText.c_str(), ILI9341_BLACK);
  displayChinese(20, 120, "IP: 192.168.4.1", ILI9341_BLACK);
  displayChinese(20, 140, "Open Browser:", ILI9341_BLACK);
  displayChinese(20, 160, "http://192.168.4.1", ILI9341_BLACK);

  // 等待用户通过Web界面配置WiFi信息
  Serial.println("\n=== Waiting for user to configure WiFi via Web interface ===");
  
  unsigned long startTime = millis();
  while(millis() - startTime < 300000) {  // 等待5分钟
    server.handleClient();  // 处理Web请求
    delay(1);
  }
  
  Serial.println("=== AP config timeout ===");
  WiFi.softAPdisconnect(true); // 断开AP
}

void scanSurroundingWiFi() {
  Serial.println("\n=== Starting scan for surrounding WiFi networks ===");
  int n = WiFi.scanNetworks();
  if (n == 0) {
    Serial.println("No WiFi networks found");
  } else {
    Serial.printf("Found %d WiFi networks\n", n);
    for (int i = 0; i < n; i++) {
      Serial.printf("%d. SSID: %s \t Signal strength: %d dBm \t Encryption: %d\n",
                    i + 1,
                    WiFi.SSID(i).c_str(),
                    WiFi.RSSI(i),
                    WiFi.encryptionType(i));
      delay(10);
    }
  }
  Serial.println("=== WiFi scan completed ===");
  WiFi.scanDelete(); // 清除扫描缓存
}

void saveWiFiToFlash(String ssid, String pwd) {
  preferences.begin("WiFi_Config", false);
  wifiRecordCount = preferences.getInt("RecordCount", 0);

  // 检查是否已存在相同的SSID，如果存在则更新该记录，而不是添加新记录
  bool ssidExists = false;
  for (int i = 0; i < wifiRecordCount; i++) {
    String ssidKey = "SSID_" + String(i);
    String existingSSID = preferences.getString(ssidKey.c_str(), "");
    if (existingSSID == ssid) {
      // 更新已存在的SSID的密码
      String pwdKey = "PWD_" + String(i);
      preferences.putString(pwdKey.c_str(), pwd);
      Serial.printf("=== Updated password for existing WiFi: %s ===\n", ssid.c_str());
      ssidExists = true;
      break;
    }
  }

  // 如果SSID不存在，则添加新记录
  if (!ssidExists) {
    if (wifiRecordCount >= MAX_WIFI_RECORDS) {
      Serial.println("=== WiFi record limit reached, overwriting oldest record ===");
      wifiRecordCount = MAX_WIFI_RECORDS - 1;
    }

    String ssidKey = "SSID_" + String(wifiRecordCount);
    String pwdKey = "PWD_" + String(wifiRecordCount);
    preferences.putString(ssidKey.c_str(), ssid);
    preferences.putString(pwdKey.c_str(), pwd);

    wifiRecordCount++;
    preferences.putInt("RecordCount", wifiRecordCount);
    Serial.printf("=== Added new WiFi record: %s ===\n", ssid.c_str());
  }

  preferences.end();

  Serial.println("=== WiFi information saved to Flash ===");
  // 重新加载WiFi记录以更新内存中的数据
  loadWiFiFromFlash();
}

void loadTempCalibration() {
  preferences.begin("Sensor_Calibration", true);
  TEMP_CALIBRATION_OFFSET = preferences.getFloat("TempOffset", 0.0);
  Serial.printf("=== Loaded temperature calibration offset: %.2f ===\n", TEMP_CALIBRATION_OFFSET);
  preferences.end();
}

void saveTempCalibration(float offset) {
  preferences.begin("Sensor_Calibration", false);
  preferences.putFloat("TempOffset", offset);
  TEMP_CALIBRATION_OFFSET = offset;  // 同时更新当前值
  Serial.printf("=== Saved temperature calibration offset: %.2f ===\n", TEMP_CALIBRATION_OFFSET);
  preferences.end();
}

void loadWiFiFromFlash() {
  preferences.begin("WiFi_Config", true);
  wifiRecordCount = preferences.getInt("RecordCount", 0);
  Serial.printf("\n=== Loaded %d WiFi records from Flash ===\n", wifiRecordCount);

  for (int i = 0; i < wifiRecordCount; i++) {
    String ssidKey = "SSID_" + String(i);
    String pwdKey = "PWD_" + String(i);
    ssidList[i] = preferences.getString(ssidKey.c_str(), "");
    pwdList[i] = preferences.getString(pwdKey.c_str(), "");

    if (ssidList[i] != "") {
      Serial.printf("Record %d: SSID=%s, PWD=%s\n",
                    i + 1,
                    ssidList[i].c_str(),
                    pwdList[i].c_str());
    }
  }
  preferences.end();
}

bool connectWiFi(String ssid, String pwd) {
  if (ssid == "" || pwd == "") {
    Serial.println("WiFi SSID or password is empty, cannot connect");
    Serial.printf("Input SSID: '%s', Password length: %d\n", ssid.c_str(), pwd.length());
    return false;
  }

  Serial.println("\n=== Starting WiFi connection ===");
  Serial.printf("Target SSID: %s\n", ssid.c_str());
  Serial.printf("Target password: %s\n", pwd.c_str());

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
    Serial.println("=== WiFi connection successful ===");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.print("Subnet Mask: ");
    Serial.println(WiFi.subnetMask());
    Serial.print("Gateway: ");
    Serial.println(WiFi.gatewayIP());
    Serial.print("MAC Address: ");
    Serial.println(WiFi.macAddress());
    return true;
  } else {
    Serial.println("=== WiFi connection timeout/failed ===");
    return false;
  }
}

void autoConnectWiFi() {
  if (wifiRecordCount == 0) {
    Serial.println("=== No stored WiFi records, cannot auto connect ===");
    return;
  }

  Serial.println("\n=== Starting auto connection to stored WiFi ===");
  for (int i = 0; i < wifiRecordCount; i++) {
    String currentSSID = ssidList[i];
    String currentPWD = pwdList[i];

    if (currentSSID == "") continue;

    Serial.printf("\n=== Attempting to connect to record %d ===", i + 1);
    if (connectWiFi(currentSSID, currentPWD)) {
      Serial.println("=== Auto WiFi connection successful ===");
      return;
    } else {
      Serial.printf("=== Connection failed for record %d ===\n", i + 1);
      // 不在单次连接失败时删除记录，允许用户手动清除或重试
      // clearInvalidWiFiRecord(currentSSID);
    }
  }

  Serial.println("=== All stored WiFi connections failed ===");
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
      Serial.printf("=== Invalid WiFi record cleared: %s ===\n", invalidSsid.c_str());
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
  
  // 加载温度校准值
  loadTempCalibration();

  // 2. 尝试自动连接已存储的WiFi
  autoConnectWiFi();
  
  // 3. 如果自动连接失败，进入AP配网模式
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("=== Auto connection failed, entering AP config mode ===");
    initAPMode();
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    tft.fillScreen(CYBER_BG);
    displayChinese(20, 110, "Syncing Time...", CYBER_LIGHT);
    
    // 等待时间同步，但设置超时以避免无限等待
    unsigned long syncStartTime = millis();
    const unsigned long syncTimeout = 10000; // 10秒超时
    struct tm timeinfo;
    
    // 显示同步进度
    while(millis() - syncStartTime < syncTimeout) {
      if (getLocalTime(&timeinfo)) {
        Serial.println("Time sync successful");
        break;
      }
      delay(100);
      
      // 每秒更新一次显示，让用户知道仍在同步中
      if ((millis() / 1000) % 2 == 0) {
        tft.fillRect(20, 130, 280, 20, CYBER_BG); // 清除之前的内容
        displayChinese(20, 130, "Time Syncing...", CYBER_LIGHT);
      } else {
        tft.fillRect(20, 130, 280, 20, CYBER_BG); // 清除之前的内容
        displayChinese(20, 130, "Time Syncing..", CYBER_LIGHT);
      }
    }
    
    // 检查是否超时
    if (!getLocalTime(&timeinfo)) {
      Serial.println("Time sync timeout, using system default time");
      tft.fillRect(20, 130, 280, 20, CYBER_BG);
      displayChinese(20, 130, "Time Sync Timeout", ILI9341_RED);
      
      // 即使时间同步失败，也要设置一个基本的时间缓存以避免显示"--:--:--"
      // 使用系统启动时间作为基础
      timeinfo.tm_year = 2024 - 1900;  // 设置为2024年
      timeinfo.tm_mon = 0;             // 1月
      timeinfo.tm_mday = 1;            // 1日
      timeinfo.tm_hour = 0;
      timeinfo.tm_min = 0;
      timeinfo.tm_sec = 0;
      
      // 更新全局时间缓存
      cachedTimeInfo = timeinfo;
      timeCacheValid = true;
      lastTimeCacheUpdate = millis();
      
      delay(1000);
    } else {
      // 时间同步成功，更新全局时间缓存
      cachedTimeInfo = timeinfo;
      timeCacheValid = true;
      lastTimeCacheUpdate = millis();
    }
    
    // 初始化后台Web服务器
    initBackgroundWebServer();
    
    initClockStaticUI();
    prevTimeStr = "";
    
    // 使用与MODE_CLOCK中相同的逻辑来显示时间，检查时间是否有效
    String hourStr = getTimeStr('H');
    String minStr = getTimeStr('M');
    String secStr = getTimeStr('S');
    
    // 检查时间是否有效（避免显示"--:--:--"）
    if (hourStr != "--" && minStr != "--" && secStr != "--") {
      // 只有当时间有效时才绘制时钟
      drawClockTime(hourStr, minStr, secStr);
    } else {
      // 如果时间无效，显示提示信息
      Serial.println("启动时时间无效，等待时间同步完成");
    }
    
    drawEnvDynamic(current_env_data.temperature, current_env_data.humidity, current_env_data.tvoc, current_env_data.eco2);
  } else {
    tft.fillScreen(CYBER_BG);
    displayChinese(20, 110, "WiFi Config Failed!", ILI9341_RED);
    delay(1000);
  }
}


// Web服务器处理函数
void sendMainConfigPage() {
  String html = "<!DOCTYPE html>\n";
  html += "<html><head><title>ESP32 Configuration</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 40px; background-color: #f0f0f0; color: black; }";
  html += "h1 { color: black; text-align: center; }";
  html += ".container { max-width: 400px; margin: 0 auto; padding: 20px; background: white; border-radius: 8px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
  html += "button { width: 100%; background-color: #2196F3; color: white; padding: 14px 20px; margin: 10px 0; border: none; border-radius: 4px; cursor: pointer; }";
  html += "button:hover { background-color: #0b7dda; }";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h1>ESP32 Configuration</h1>";
  html += "<p>Current IP: " + WiFi.localIP().toString() + "</p>";
  html += "<button onclick='window.location.href=\"/calibrateTemp\"'>Temperature Calibration</button>";
  html += "<button onclick='window.location.href=\"/status\"'>Device Status</button>";
  html += "</div>";
  html += "</body></html>";
  
  backgroundServer.send(200, "text/html", html);
}

void sendStatusPage() {
  String html = "<!DOCTYPE html>\n";
  html += "<html><head><title>Device Status</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 40px; background-color: #f0f0f0; color: black; }";
  html += "h1 { color: black; text-align: center; }";
  html += ".container { max-width: 400px; margin: 0 auto; padding: 20px; background: white; border-radius: 8px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h1>Device Status</h1>";
  html += "<p><strong>Temperature Calibration Offset:</strong> " + String(TEMP_CALIBRATION_OFFSET, 2) + " °C</p>";
  html += "<p><strong>Free Heap:</strong> " + String(ESP.getFreeHeap()) + " bytes</p>";
  html += "<p><strong>Uptime:</strong> " + String(millis()/1000) + " seconds</p>";
  html += "<p><a href='/'>Back to Main</a></p>";
  html += "</div>";
  html += "</body></html>";
  
  backgroundServer.send(200, "text/html", html);
}

// Web服务器处理函数
void sendTempCalibrationPage() {
  String html = "<!DOCTYPE html>\n";
  html += "<html><head><title>Temperature Calibration</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 40px; background-color: #f0f0f0; color: black; }";
  html += "h1 { color: black; text-align: center; }";
  html += ".container { max-width: 400px; margin: 0 auto; padding: 20px; background: white; border-radius: 8px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
  html += "input[type='number'] { width: 100%; padding: 12px; margin: 10px 0; box-sizing: border-box; border: 1px solid #ddd; border-radius: 4px; color: black; }";
  html += "input[type='submit'] { width: 100%; background-color: #4CAF50; color: white; padding: 14px 20px; margin: 10px 0; border: none; border-radius: 4px; cursor: pointer; }";
  html += "input[type='submit']:hover { background-color: #45a049; }";
  html += "button { width: 100%; background-color: #2196F3; color: white; padding: 14px 20px; margin: 10px 0; border: none; border-radius: 4px; cursor: pointer; }";
  html += "button:hover { background-color: #0b7dda; }";
  html += "label { color: black; }";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h1>Temperature Calibration</h1>";
  html += "<form action='/calibrateTemp' method='POST'>";
  html += "<label for='offset'>Calibration Offset (°C):</label>";
  html += "<input type='number' id='offset' name='offset' step='0.1' value='" + String(TEMP_CALIBRATION_OFFSET, 2) + "' min='-20' max='20'><br>";
  html += "<input type='submit' value='Save Calibration'>";
  html += "</form>";
  html += "<button type='button' onclick='window.location.href=\"/\"'>Back to Main</button>";
  html += "</div>";
  html += "</body></html>";
  
  backgroundServer.send(200, "text/html", html);
}

void handleTempCalibration() {
  Serial.println("Temperature calibration request received");
  
  String offsetStr = server.arg("offset");
  float newOffset = offsetStr.toFloat();
  
  Serial.printf("Received temperature calibration offset: %.2f\n", newOffset);
  
  // 保存新的校准值
  saveTempCalibration(newOffset);
  
  // 发送成功页面
  String successHtml = "<!DOCTYPE html>\n";
  successHtml += "<html><head><title>Calibration Success</title>";
  successHtml += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  successHtml += "<style>";
  successHtml += "body { font-family: Arial, sans-serif; margin: 40px; text-align: center; color: black; }";
  successHtml += ".success { color: green; font-size: 18px; }";
  successHtml += "</style>";
  successHtml += "</head><body>";
  successHtml += "<h1>Calibration Success!</h1>";
  successHtml += "<p class='success'>Temperature offset calibrated to: " + String(newOffset, 2) + " °C</p>";
  successHtml += "<p><a href='/'>Return to Config Page</a> | <a href='/calibrateTemp'>Calibrate Again</a></p>";
  successHtml += "</body></html>";
  
  server.send(200, "text/html", successHtml);
}

void handleBackgroundTempCalibration() {
  Serial.println("Background temperature calibration request received");
  
  String offsetStr = backgroundServer.arg("offset");
  float newOffset = offsetStr.toFloat();
  
  Serial.printf("Received background temperature calibration offset: %.2f\n", newOffset);
  
  // 保存新的校准值
  saveTempCalibration(newOffset);
  
  // 发送成功页面
  String successHtml = "<!DOCTYPE html>\n";
  successHtml += "<html><head><title>Calibration Success</title>";
  successHtml += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  successHtml += "<style>";
  successHtml += "body { font-family: Arial, sans-serif; margin: 40px; text-align: center; color: black; }";
  successHtml += ".success { color: green; font-size: 18px; }";
  successHtml += "</style>";
  successHtml += "</head><body>";
  successHtml += "<h1>Calibration Success!</h1>";
  successHtml += "<p class='success'>Temperature offset calibrated to: " + String(newOffset, 2) + " °C</p>";
  successHtml += "<p><a href='/'>Back to Main</a> | <a href='/calibrateTemp'>Calibrate Again</a></p>";
  successHtml += "</body></html>";
  
  backgroundServer.send(200, "text/html", successHtml);
}

void initBackgroundWebServer() {
  // 设置后台Web服务器路由
  backgroundServer.on("/", sendMainConfigPage);
  backgroundServer.on("/calibrateTemp", HTTP_GET, []() {
    sendTempCalibrationPage();
  });
  backgroundServer.on("/calibrateTemp", HTTP_POST, handleBackgroundTempCalibration);
  backgroundServer.on("/status", sendStatusPage);
  
  backgroundServer.begin();
  Serial.println("Background web server started on port 8080");
  Serial.printf("Access via: http://%s:8080\n", WiFi.localIP().toString().c_str());
}

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
  Serial.println("Root path request received");
  sendConfigPage();
}

void handleConfig() {
  Serial.println("Configuration request received");
  
  String ssid = server.arg("ssid");
  String password = server.arg("password");
  
  Serial.printf("Received WiFi config: SSID=%s, Password=%s\n", ssid.c_str(), password.c_str());
  
  if (ssid.length() > 0) {
    // 保存WiFi信息到Flash
    saveWiFiToFlash(ssid, password);
    
    // 尝试连接WiFi
    if (connectWiFi(ssid, password)) {
      Serial.println("=== Web config successful, connected to target WiFi ===");
      
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
      Serial.println("=== WiFi配置成功，设备将重启以应用新设置 ===");
      ESP.restart(); // 重启ESP32以应用新的WiFi设置
      return;
    } else {
      Serial.println("=== Web config failed, target WiFi cannot connect ===");
      
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
      errorHtml += "<p><a href='/'>Return to Config Page</a></p>";
      errorHtml += "</body></html>";
      
      server.send(200, "text/html", errorHtml);
    }
  } else {
    // 如果没有收到参数，发送配置页面
    sendConfigPage();
  }
}