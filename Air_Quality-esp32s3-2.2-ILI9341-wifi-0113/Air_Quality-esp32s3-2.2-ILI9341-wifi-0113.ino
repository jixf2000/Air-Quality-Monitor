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
#include <WiFiClient.h>
#include <PubSubClient.h>


// ====== WiFi和时间配置 ======
const char* ntpServers[] = {
  "ntp.aliyun.com",      // 阿里云时间服务器
  "cn.pool.ntp.org",     // 国际公共NTP服务器
  "time.windows.com",    // 微软时间服务器
  "time.nist.gov"        // NIST时间服务器
};
const int numNtpServers = sizeof(ntpServers) / sizeof(ntpServers[0]);
const long  gmtOffset_sec      = 8 * 3600;   // GMT+8 中国标准时间
const int   daylightOffset_sec = 0;

// ===================== 配网参数 =====================
#define AP_SSID           "Air-Quality-Monitor"  // 配网AP名称
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

// ===================== MQTT配置 =====================
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// MQTT配置结构体
struct MQTTConfig {
  char server[64];
  uint16_t port;
  char username[32];
  char password[64];
  char clientId[32];
  char topic[64];
  bool enabled;
};

MQTTConfig mqttConfig;

// MQTT配置默认值
const char* MQTT_DEFAULT_SERVER = "mqtt.eclipse.org";
const uint16_t MQTT_DEFAULT_PORT = 1883;
const char* MQTT_DEFAULT_TOPIC = "home/env/sensor";
const bool MQTT_DEFAULT_ENABLED = false;

// ====== 引脚定义 ======
#define TFT_CS   10
#define TFT_DC   11
#define TFT_RST  12
#define TFT_MOSI 13
#define TFT_MISO -1  // ILI9341通常不需要MISO
#define TFT_SCLK 9

// #define ENC_A_PIN    14
// #define ENC_B_PIN    20
// #define ENC_BTN_PIN  21
#define KEY0_PIN     0
#define KEY1_PIN     18  // 新增按键引脚，用于切换页面

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
#define CYBER_BG      ILI9341_BLACK
#define CYBER_GREEN   ILI9341_GREEN  
#define CYBER_ACCENT  ILI9341_CYAN  
#define CYBER_LIGHT   0xFD20  
#define CYBER_BLUE    ILI9341_BLUE  
#define CYBER_PINK    ILI9341_MAGENTA
#define CYBER_LIGHT_GREY 0xC618      // 浅灰色 (RGB: 198, 201, 207 -> 0xC618)

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
  MODE_ENVIRONMENT,  // 新增环境参数四等分显示模式
  MODE_DATE_TIME,    // 新增日期时间显示模式
  // MODE_HISTORY     // 新增历史曲线显示模式（已禁用）
};
UIMode currentMode = MODE_CLOCK;
int menuIndex = 0;


// ====== 环境数值 ======

// 温度校准偏移量（在实际读数上加上这个值来校准）
float TEMP_CALIBRATION_OFFSET = 0.0;  // 可以根据实际校准情况调整此值，如果温度偏差大请重新校准

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

// 第一页专用温度值，用于确保第二页取值与第一页一致
float first_page_temperature = 0.0f;

// 用于清除旧环境变量显示的变量
String prevHumidityStr = "";
String prevTemperatureStr = "";
String prevTvocStr = "";
String prevEco2Str = "";

// ====== 时钟变量 ======
int    prevSecond  = -1;
String prevTimeStr = "";
struct tm cachedTimeInfo;
bool timeCacheValid = false;
unsigned long lastTimeCacheUpdate = 0;
unsigned long lastTimeDisplayUpdate = 0;  // 记录上次时间显示更新时间

// ====== 历史数据存储 ======
#define HISTORY_SIZE_HOURLY   24    // 每小时数据点数（24小时）
#define HISTORY_SIZE_DAILY    7     // 每天数据点数（7天）
#define HISTORY_SIZE_WEEKLY   4     // 每周数据点数（4周）

enum HistoryPeriod {
  HP_HOURLY = 0,    // 按小时显示
  HP_DAILY,         // 按天显示
  HP_WEEKLY         // 按周显示
};

// 历史数据结构体
typedef struct {
  float temperature;
  float humidity;
  uint16_t tvoc;
  uint16_t eco2;
  time_t timestamp;  // 记录时间戳
} HistoryDataPoint;

// 历史数据数组 - 按小时存储
HistoryDataPoint hourlyHistory[HISTORY_SIZE_HOURLY];
int hourlyHistoryIndex = 0;
bool hourlyHistoryFull = false;

// 历史数据数组 - 按天存储
HistoryDataPoint dailyHistory[HISTORY_SIZE_DAILY];
int dailyHistoryIndex = 0;
bool dailyHistoryFull = false;

// 历史数据数组 - 按周存储
HistoryDataPoint weeklyHistory[HISTORY_SIZE_WEEKLY];
int weeklyHistoryIndex = 0;
bool weeklyHistoryFull = false;

// 当前选择的历史周期
HistoryPeriod currentHistoryPeriod = HP_HOURLY;

// 上次记录历史数据的时间
unsigned long lastHistoryRecordTime = 0;

// 记录历史数据函数
void recordHistoryData() {
  time_t currentTime = time(nullptr);
  struct tm *timeInfo = localtime(&currentTime);
  
  // 创建新的历史数据点
  HistoryDataPoint newData;
  newData.temperature = current_env_data.temperature;
  newData.humidity = current_env_data.humidity;
  newData.tvoc = current_env_data.tvoc;
  newData.eco2 = current_env_data.eco2;
  newData.timestamp = currentTime;
  
  unsigned long currentMillis = millis();
  
  // 每小时记录一次数据到小时历史
  if (currentMillis - lastHistoryRecordTime >= 3600000) {  // 1小时 = 3600000毫秒
    // 添加到小时历史数组
    hourlyHistory[hourlyHistoryIndex] = newData;
    hourlyHistoryIndex++;
    if (hourlyHistoryIndex >= HISTORY_SIZE_HOURLY) {
      hourlyHistoryIndex = 0;
      hourlyHistoryFull = true;
    }
    
    lastHistoryRecordTime = currentMillis;
    
    // 同时添加到日历史（每天0点记录）
    if (timeInfo->tm_hour == 0) {
      dailyHistory[dailyHistoryIndex] = newData;
      dailyHistoryIndex++;
      if (dailyHistoryIndex >= HISTORY_SIZE_DAILY) {
        dailyHistoryIndex = 0;
        dailyHistoryFull = true;
      }
    }
    
    // 同时添加到周历史（每周一0点记录）
    if (timeInfo->tm_wday == 1 && timeInfo->tm_hour == 0) {  // 周一
      weeklyHistory[weeklyHistoryIndex] = newData;
      weeklyHistoryIndex++;
      if (weeklyHistoryIndex >= HISTORY_SIZE_WEEKLY) {
        weeklyHistoryIndex = 0;
        weeklyHistoryFull = true;
      }
    }
  }
}

// ====== 编码器和按钮 ======
// int  lastEncA   = HIGH;
// int  lastEncB   = HIGH;
// bool lastEncBtn = HIGH;
bool lastKey0   = HIGH;
bool lastKey1   = HIGH;  // 新增按键状态变量
unsigned long lastBtnMs = 0;

// ====== 长按复位功能 ======
unsigned long key0PressStartTime = 0;  // 记录KEY0按下开始时间
bool key0LongPressActive = false;      // 标记长按是否已激活



// ====== 警报/LED闪烁 ======
enum AlertLevel {
  ALERT_NONE = 0,
  ALERT_CO2
};
AlertLevel currentAlertLevel = ALERT_NONE;
unsigned long lastLedToggleMs = 0;
bool ledState = false;

unsigned long lastCo2BlinkMs = 0;
bool co2BlinkOn = false;

// ========= 辅助函数: 按钮 =========
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

// 长按复位功能：检测KEY0长按1分钟复位系统
void handleLongPressReset() {
  // 获取当前KEY0引脚状态
  bool currentKey0State = digitalRead(KEY0_PIN);
  unsigned long currentTime = millis();
  
  // 检查是否开始按下
  if (currentKey0State == LOW && !key0LongPressActive) {
    // 如果是刚开始按下的，记录按下开始时间
    if (key0PressStartTime == 0) {
      key0PressStartTime = currentTime;
    }
    
    // 检查是否长按达到1分钟（60000毫秒）
    if (currentTime - key0PressStartTime >= 60000) {  // 1分钟 = 60 * 1000毫秒
      Serial.println("检测到KEY0长按1分钟，正在复位系统...");
      
      // 显示提示信息
      tft.fillScreen(CYBER_BG);
      displayChineseSmooth(60, 100, "System Reset", CYBER_LIGHT);
      displayChineseSmooth(40, 130, "Long Press 1 Min", CYBER_LIGHT);
      delay(1000);
      
      // 执行系统复位
      ESP.restart();
      key0LongPressActive = true;  // 标记长按已激活，防止重复触发
    }
  } 
  // 检查是否释放了按键
  else if (currentKey0State == HIGH) {
    // 按键释放，重置长按状态
    key0PressStartTime = 0;
    key0LongPressActive = false;
  }
}



// ========= 中文文本显示辅助函数 =========
// 注意：使用Adafruit GFX库显示字符
void displayChinese(int x, int y, const char* text, uint16_t color, uint16_t bgColor) {
  // 使用FreeSans字体以获得更平滑的显示效果
  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(color, bgColor);
  tft.setCursor(x, y);
  tft.print(text);
  // 恢复默认字体
  tft.setFont();
}

void displayChinese(int x, int y, const char* text, uint16_t color) {
  displayChinese(x, y, text, color, CYBER_BG);  // 使用默认背景色
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
  displayChineseSmooth(20, 110, "Connecting WiFi", CYBER_LIGHT);
  
  WiFi.begin(ssid, password);

  uint8_t retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(300);
    tft.fillRect(160 + retry*10, 110, 8, 8, CYBER_LIGHT);
    retry++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServers[0]);
    tft.fillScreen(CYBER_BG);
    displayChineseSmooth(20, 110, "Syncing Time...", CYBER_LIGHT);
    delay(800);
  } else {
    tft.fillScreen(CYBER_BG);
    displayChineseSmooth(20, 110, "WiFi Connect Failed!", ILI9341_RED);
    delay(1000);
  }
}

String getTimeStr(char type) {
  unsigned long currentTime = millis();
  // 每30秒最多更新一次时间缓存，避免频繁调用getLocalTime造成阻塞
  if (!timeCacheValid || currentTime - lastTimeCacheUpdate >= 30000) {
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

// ===================== 显示函数声明 =====================
void displayChinese(int x, int y, const char* text, uint16_t color, uint16_t bgColor);  // 显示中文字符
void displayChinese(int x, int y, const char* text, uint16_t color);  // 重载版本，使用默认背景色

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
              float old_temp = current_env_data.temperature;
              current_env_data.temperature = temp_event.temperature + TEMP_CALIBRATION_OFFSET;
              // 仅在温度值发生变化时输出调试信息
              if (abs(current_env_data.temperature - old_temp) > 0.1) {
                Serial.printf("=== AHT21: 传感器读数: %.2f, 校准偏移: %.2f, 最终温度: %.2f ===\n", 
                          temp_event.temperature, TEMP_CALIBRATION_OFFSET, current_env_data.temperature);
              }
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
    // 在时钟模式下不再更新环境数据显示，改为实时更新
    // 在其他模式下可选择性地更新显示（如果需要）
    last_display_update = millis();
  }
  
  // 记录历史数据
  recordHistoryData();
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
const int GRID_TOP = 82;
const int GRID_MID = 140;
const int GRID_BOT = 198;
const int GRID_MID_X = (GRID_L + GRID_R) / 2;

const int TOP_LABEL_Y      = GRID_TOP + 17;  // 向下移动10个像素，再向下移动5个像素
const int TOP_VALUE_Y      = GRID_TOP + 47;  // 向下移动8个像素，再向下移动5个像素
const int BOTTOM_LABEL_Y   = GRID_MID + 12;
const int BOTTOM_VALUE_Y   = GRID_MID + 39;
const int TVOC_LABEL_Y     = GRID_MID + 19;  // 向下移动5个像素
const int TVOC_VALUE_Y     = GRID_MID + 45;
const int CO2_LABEL_Y      = GRID_MID + 19;  // 向下移动5个像素
const int CO2_VALUE_Y      = GRID_MID + 45;

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
                       uint8_t size) {
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
  
  displayChineseSmooth(8, 16, "Environment Monitor", CYBER_LIGHT);
  // displayChineseSmooth(8, 88, "Air Quality:", ILI9341_BLACK);  // 空气质量标签（已隐藏）

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

  // drawAlarmIcon(); // 闹钟功能已移除
}

void drawClockTime(String hourStr, String minStr, String secStr) {
  String cur = hourStr + ":" + minStr;  // 只显示小时和分钟，去掉秒
  
  // 清除旧时间显示区域
  if (prevTimeStr != "") {
    int16_t x1, y1;
    uint16_t w, h;
    tft.setFont(&FreeSans24pt7b);  // 使用相同字体测量旧文本
    tft.getTextBounds(prevTimeStr.c_str(), 0, 0, &x1, &y1, &w, &h);
    // 计算带字符间距的实际宽度
    int adjustedWidth = w + (prevTimeStr.length() * 10);  // 每个字符额外增加10像素间距
    int oldX = (320 - adjustedWidth) / 2;
    // 增加上部范围和整体额外宽度以确保完全清除旧文本（缩小上下范围）
    tft.fillRect(oldX + x1 - 15, 20, adjustedWidth + 30, h + 27, CYBER_BG);
  }
  
  prevTimeStr = cur;

  // 使用平滑字体显示时间
  tft.setFont(&FreeSans24pt7b);  // 使用平滑字体，最大可用字号
  tft.setTextColor(CYBER_LIGHT, CYBER_BG);
  
  int16_t x1, y1;
  uint16_t w, h;
  // 准确计算带字符间距的总宽度：逐个字符测量并累加
  int totalWidth = 0;
  for (int i = 0; i < cur.length(); i++) {
    String singleChar = String(cur.charAt(i));
    tft.getTextBounds(singleChar, 0, 0, &x1, &y1, &w, &h);
    totalWidth += w + 10;  // 每个字符宽度加上字符间距
  }
  if (cur.length() > 0) totalWidth -= 10;  // 减去最后一个字符后的多余间距
  
  int baseX = (320 - totalWidth) / 2;  // 使用总宽度计算居中位置
  int y = 65;  // 调整Y坐标以适应字体的基线

  // 计算字符间距，增加字符间的距离
  int charSpacing = 10; // 额外字符间距
  int currentX = baseX;
  
  // 逐个字符绘制以增加字符间距，并实现加粗效果
  for (int i = 0; i < cur.length(); i++) {
    String singleChar = String(cur.charAt(i));
    tft.getTextBounds(singleChar, 0, 0, &x1, &y1, &w, &h);
    
    // 使用单次绘制实现视觉加粗效果，减少闪烁
    tft.setTextColor(CYBER_LIGHT, CYBER_BG);
    
    // 绘制主字符
    tft.setCursor(currentX, y);
    tft.print(singleChar);  // 主字符
    
    // 为模拟加粗效果，仅在右侧绘制一次偏移，减少重绘次数
    tft.setCursor(currentX + 1, y);
    tft.print(singleChar);  // 右侧轻微偏移以增强视觉厚度
    
    // 计算下一个字符的位置，包括字符宽度和额外间距
    currentX += w + charSpacing;
  }
  
  // 在时间显示下方绘制细灰色分割线 - 已移除
  // int lineY = y + h/2 + 8;  // 在时间显示下方添加分割线
  // tft.drawFastHLine(5, lineY, 310, CYBER_LIGHT_GREY);  // 绘制水平分割线
  
  tft.setFont();  // 恢复默认字体
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
  String currentEco2Str = String(co2Buf);
  
  // 计算CO2值显示区域的中心坐标
  int co2_x0 = GRID_MID_X;
  int co2_x1 = GRID_R;
  int co2_y = CO2_VALUE_Y;
  
  // 清除旧的CO2值显示
  if (prevEco2Str != "") {
    tft.setFont(&FreeSans12pt7b);
    int16_t old_x4, old_y4;
    uint16_t old_w4, old_h4;
    tft.getTextBounds(prevEco2Str.c_str(), 0, 0, &old_x4, &old_y4, &old_w4, &old_h4);
    int old_co2_x_pos = co2_x0 + ((co2_x1 - co2_x0) - old_w4) / 2;
    // 增加清除区域的宽度以确保完全清除旧文本
    tft.fillRect(old_co2_x_pos + old_x4 - 2, co2_y - old_h4 + 2, old_w4 + 4, old_h4 + 4, CYBER_BG);
  }
  
  tft.setFont(&FreeSans12pt7b);  // 使用与第二页相同的平滑字体
  int16_t x4, y4;
  uint16_t w4, h4;
  tft.getTextBounds(co2Buf, 0, 0, &x4, &y4, &w4, &h4);
  int co2_x_pos = co2_x0 + ((co2_x1 - co2_x0) - w4) / 2;
  
  tft.setTextColor(col, CYBER_BG);
  tft.setCursor(co2_x_pos, co2_y);
  tft.print(co2Buf);
  
  // 更新CO2值记录
  prevEco2Str = currentEco2Str;
  
  // 恢复默认字体
  tft.setFont();
}

void drawEnvDynamic(float temp, float hum, uint16_t tvoc, uint16_t eco2) {
  uint16_t colHUMI = CYBER_BLUE;
  uint16_t colTEMP = CYBER_BLUE;
  uint16_t colTVOC = CYBER_BLUE;
  uint16_t colCO2  = CYBER_BLUE;

  // 左上区域 - 湿度值
  char humBuf[10];
  sprintf(humBuf, "%.1f%%", hum);
  String currentHumidityStr = String(humBuf);
  // 计算湿度值显示区域的中心坐标
  int hum_x0 = GRID_L;
  int hum_x1 = GRID_MID_X;
  int hum_y = TOP_VALUE_Y;
  
  // 清除旧的湿度值显示
  if (prevHumidityStr != "") {
    tft.setFont(&FreeSans12pt7b);
    int16_t old_x, old_y;
    uint16_t old_w, old_h;
    tft.getTextBounds(prevHumidityStr.c_str(), 0, 0, &old_x, &old_y, &old_w, &old_h);
    int old_hum_x_pos = hum_x0 + ((hum_x1 - hum_x0) - old_w) / 2;
    // 增加清除区域的宽度以确保完全清除旧文本
    tft.fillRect(old_hum_x_pos + old_x - 2, hum_y - old_h + 2, old_w + 4, old_h + 4, CYBER_BG);
  }
  
  tft.setFont(&FreeSans12pt7b);  // 使用与第二页相同的平滑字体
  int16_t x1, y1;
  uint16_t w1, h1;
  tft.getTextBounds(humBuf, 0, 0, &x1, &y1, &w1, &h1);
  int hum_x_pos = hum_x0 + ((hum_x1 - hum_x0) - w1) / 2;
  
  tft.setTextColor(colHUMI, CYBER_BG);
  tft.setCursor(hum_x_pos, hum_y);
  tft.print(humBuf);
  
  // 更新湿度值记录
  prevHumidityStr = currentHumidityStr;
  
  // 右上区域 - 温度值
  char tempBuf[10];
  sprintf(tempBuf, "%.1fC", temp);
  String currentTemperatureStr = String(tempBuf);
  // 计算温度值显示区域的中心坐标
  int temp_x0 = GRID_MID_X;
  int temp_x1 = GRID_R;
  int temp_y = TOP_VALUE_Y;
  
  // 清除旧的温度值显示
  if (prevTemperatureStr != "") {
    tft.setFont(&FreeSans12pt7b);
    int16_t old_x2, old_y2;
    uint16_t old_w2, old_h2;
    tft.getTextBounds(prevTemperatureStr.c_str(), 0, 0, &old_x2, &old_y2, &old_w2, &old_h2);
    int old_temp_x_pos = temp_x0 + ((temp_x1 - temp_x0) - old_w2) / 2;
    // 增加清除区域的宽度以确保完全清除旧文本
    tft.fillRect(old_temp_x_pos + old_x2 - 2, temp_y - old_h2 + 2, old_w2 + 4, old_h2 + 4, CYBER_BG);
  }
  
  int16_t x2, y2;
  uint16_t w2, h2;
  tft.getTextBounds(tempBuf, 0, 0, &x2, &y2, &w2, &h2);
  int temp_x_pos = temp_x0 + ((temp_x1 - temp_x0) - w2) / 2;
  
  tft.setTextColor(colTEMP, CYBER_BG);
  tft.setCursor(temp_x_pos, temp_y);
  tft.print(tempBuf);
  
  // 更新温度值记录
  prevTemperatureStr = currentTemperatureStr;
  
  // 左下区域 - TVOC值
  float tvoc_mg = tvoc / 1000.0f;
  char tvocBuf[16];
  sprintf(tvocBuf, "%.3f mg/m3", tvoc_mg);
  String currentTvocStr = String(tvocBuf);
  // 计算TVOC值显示区域的中心坐标
  int tvoc_x0 = GRID_L;
  int tvoc_x1 = GRID_MID_X;
  int tvoc_y = TVOC_VALUE_Y;
  
  // 清除旧的TVOC值显示
  if (prevTvocStr != "") {
    tft.setFont(&FreeSans12pt7b);
    int16_t old_x3, old_y3;
    uint16_t old_w3, old_h3;
    tft.getTextBounds(prevTvocStr.c_str(), 0, 0, &old_x3, &old_y3, &old_w3, &old_h3);
    int old_tvoc_x_pos = tvoc_x0 + ((tvoc_x1 - tvoc_x0) - old_w3) / 2;
    // 增加清除区域的宽度以确保完全清除旧文本
    tft.fillRect(old_tvoc_x_pos + old_x3 - 2, tvoc_y - old_h3 + 2, old_w3 + 4, old_h3 + 4, CYBER_BG);
  }
  
  int16_t x3, y3;
  uint16_t w3, h3;
  tft.getTextBounds(tvocBuf, 0, 0, &x3, &y3, &w3, &h3);
  int tvoc_x_pos = tvoc_x0 + ((tvoc_x1 - tvoc_x0) - w3) / 2;
  
  tft.setTextColor(colTVOC, CYBER_BG);
  tft.setCursor(tvoc_x_pos, tvoc_y);
  tft.print(tvocBuf);
  
  // 更新TVOC值记录
  prevTvocStr = currentTvocStr;

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
  "Env Params",   // 环境参数显示选项
  "Date/Time"     // 日期时间显示选项
};
const int MENU_ITEMS = 3;  // 更新菜单项数量，移除闹钟页面

void drawMenu() {
  tft.fillScreen(CYBER_BG);

  displayChineseSmooth(20, 20, "Mode Select", CYBER_LIGHT);

  for (int i = 0; i < MENU_ITEMS; i++) {
    int y = 64 + i * 36;
    if (i == menuIndex) {
      tft.fillRect(12, y - 4, 296, 28, CYBER_ACCENT);
      displayChineseSmooth(24, y, menuItemsEN[i], CYBER_BG, CYBER_ACCENT);
    } else {
      tft.fillRect(12, y - 4, 296, 28, CYBER_BG);
      displayChineseSmooth(24, y, menuItemsEN[i], ILI9341_BLACK);
    }
  }
  // drawAlarmIcon(); // 闹钟功能已移除
}


// 闹钟功能已被移除
void drawAlarmScreen(bool full) {
  // 闹钟功能已被移除，此函数为空实现
  tft.fillScreen(CYBER_BG);
  displayChineseSmooth(20, 100, "Feature Removed", ILI9341_BLACK);
}

void drawAlarmRingingScreen() {
  // 闹钟功能已被移除，此函数为空实现
  tft.fillScreen(CYBER_BG);
  displayChineseSmooth(20, 100, "Feature Removed", ILI9341_BLACK);
}

// ========= 环境参数四等分显示界面 =========
void initEnvironmentStaticUI() {
  tft.fillScreen(CYBER_BG);
  
  // 绘制十字分割线 - 细的浅灰色线条
  // 绘制水平分割线
  tft.drawFastHLine(0, 120, 320, CYBER_LIGHT_GREY);  // 使用细线绘制浅灰色水平线
  // 绘制垂直分割线
  tft.drawFastVLine(160, 0, 240, CYBER_LIGHT_GREY);  // 使用细线绘制浅灰色垂直线
  
  // 左上区域 - 湿度
  displayChineseCenterSmooth(0, 160, 32, "Humidity", ILI9341_YELLOW, CYBER_BG, false);
  
  // 右上区域 - 温度
  displayChineseCenterSmooth(160, 320, 32, "Temperature", ILI9341_YELLOW, CYBER_BG, false);
  
  // 左下区域 - TVOC
  displayChineseCenterSmooth(0, 160, 152, "TVOC", ILI9341_YELLOW, CYBER_BG, false);
  
  // 右下区域 - CO2
  displayChineseCenterSmooth(160, 320, 152, "CO2", ILI9341_YELLOW, CYBER_BG, false);
  
  // 显示初始数值
  drawEnvQuadrant(current_env_data.humidity, first_page_temperature, 
                  current_env_data.tvoc, current_env_data.eco2);
}

void drawEnvQuadrant(float humidity, float temperature, uint16_t tvoc, uint16_t eco2) {
  // 左上区域 - 湿度值
  char humBuf[10];
  sprintf(humBuf, "%.1f%%", humidity);
  // 先清除旧的数值区域，再显示新数值
  tft.fillRect(0, 40, 150, 80, CYBER_BG);  // 清除湿度值区域（扩大）
  displayChineseCenterSmooth(0, 160, 80, humBuf, CYBER_BLUE, CYBER_BG, true);  // 使用大字体显示数值
  
  // 右上区域 - 温度值
  char tempBuf[10];
  sprintf(tempBuf, "%.1fC", temperature);
  // 先清除旧的数值区域，再显示新数值
  tft.fillRect(160, 40, 150, 80, CYBER_BG);  // 清除温度值区域（扩大）
  displayChineseCenterSmooth(160, 320, 80, tempBuf, CYBER_BLUE, CYBER_BG, true);  // 使用大字体显示数值
  
  // 左下区域 - TVOC值
  char tvocBuf[12];
  sprintf(tvocBuf, "%d ppb", tvoc);
  // 先清除旧的数值区域，再显示新数值
  tft.fillRect(0, 160, 150, 80, CYBER_BG);  // 清除TVOC值区域（扩大）
  displayChineseCenterSmooth(0, 160, 200, tvocBuf, CYBER_BLUE, CYBER_BG, true);  // 使用大字体显示数值
  
  // 右下区域 - CO2值
  char eco2Buf[12];
  sprintf(eco2Buf, "%d ppm", eco2);
  // 先清除旧的数值区域，再显示新数值
  tft.fillRect(160, 160, 150, 80, CYBER_BG);  // 清除CO2值区域（扩大）
  displayChineseCenterSmooth(160, 320, 200, eco2Buf, CYBER_BLUE, CYBER_BG, true);  // 使用大字体显示数值
}

// 新增：绘制日期时间页面函数
void drawDateTimePage() {
  // 清除屏幕
  tft.fillScreen(CYBER_BG);
  
  // 获取当前时间
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    // 格式化年月日
    char dateStr[32];
    sprintf(dateStr, "%04d-%02d-%02d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
    
    // 格式化时间（时:分）
    char timeStr[32];
    sprintf(timeStr, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    
    // 设置与第一页时钟相同的颜色
    tft.setTextColor(CYBER_LIGHT, CYBER_BG);
    
    // 设置字体为Arial Black风格（使用FreeSans18pt7b作为替代）
    tft.setFont(&FreeSans18pt7b);
    tft.setTextSize(1);
    
    // 计算日期文本的宽度以居中显示
    int16_t x1, y1;
    uint16_t w, h;
    tft.getTextBounds(dateStr, 0, 0, &x1, &y1, &w, &h);
    int dateX = (320 - w) / 2;
    
    // 显示日期（年月日）在第一行
    tft.setCursor(dateX, 80);
    tft.print(dateStr);
    
    // 计算时间文本的宽度以居中显示
    tft.getTextBounds(timeStr, 0, 0, &x1, &y1, &w, &h);
    int timeX = (320 - w) / 2;
    
    // 显示时间（时:分）在第二行
    tft.setCursor(timeX, 120);
    tft.print(timeStr);
    
    // 添加IP地址信息，使用较小的字体
    tft.setFont(&FreeSans9pt7b);
    tft.setTextColor(CYBER_LIGHT, CYBER_BG);  // 统一使用与日期时间相同的颜色
    
    // 获取IP地址并显示
    String ipStr = "IP: " + WiFi.localIP().toString();
    tft.setCursor(10, 160);
    tft.print(ipStr);
    
    // 显示配置页面地址和端口
    tft.setCursor(10, 180);
    tft.print("Config: ");
    tft.setCursor(10, 200);
    tft.print(WiFi.localIP().toString() + ":8080");
    
    // 显示版本信息和作者邮箱
    tft.setCursor(10, 220);
    tft.print("v1.0 7591196@qq.com");
    
  } else {
    // 如果无法获取时间，显示错误信息
    tft.setTextColor(ILI9341_RED, CYBER_BG);
    tft.setFont(&FreeSans12pt7b);
    tft.setTextSize(1);
    tft.setCursor(50, 120);
    tft.print("Time Sync Error");
  }
}

/*
// ========= 历史曲线显示界面 =========
void initHistoryStaticUI() {
  tft.fillScreen(CYBER_BG);
  
  // 绘制标题
  displayChineseSmooth(20, 16, "History Graphs", CYBER_LIGHT);
  
  // 绘制当前历史周期显示
  const char* periodLabels[] = {"Hourly", "Daily", "Weekly"};
  displayChineseSmooth(200, 16, periodLabels[currentHistoryPeriod], CYBER_BLUE);
  
  // 绘制坐标轴
  tft.drawLine(40, 40, 40, 220, ILI9341_BLACK);  // Y轴
  tft.drawLine(40, 220, 300, 220, ILI9341_BLACK);  // X轴
  
  // 绘制网格线
  for (int i = 1; i < 5; i++) {
    tft.drawFastHLine(40, 40 + i * 36, 260, CYBER_LIGHT_GREY);  // 水平网格线
  }
  for (int i = 1; i < 13; i++) {
    tft.drawFastVLine(40 + i * 20, 40, 180, CYBER_LIGHT_GREY);  // 垂直网格线
  }
  
  // 绘制图例
  tft.fillRect(240, 60, 8, 8, CYBER_BLUE);     // 温度
  displayChineseSmooth(250, 60, "Temp", ILI9341_BLACK);
  
  tft.fillRect(240, 80, 8, 8, CYBER_GREEN);     // 湿度
  displayChineseSmooth(250, 80, "Humi", ILI9341_BLACK);
  
  tft.fillRect(240, 100, 8, 8, CYBER_PINK);    // TVOC
  displayChineseSmooth(250, 100, "TVOC", ILI9341_BLACK);
  
  tft.fillRect(240, 120, 8, 8, CYBER_ACCENT);   // CO2
  displayChineseSmooth(250, 120, "CO2", ILI9341_BLACK);
}

void drawHistoryGraphs() {
  // 清除图表区域
  tft.fillRect(41, 41, 259, 179, CYBER_BG);
  
  // 根据当前历史周期选择数据源
  HistoryDataPoint* historyData;
  int historySize;
  int historyIndex;
  bool historyFull;
  
  switch (currentHistoryPeriod) {
    case HP_HOURLY:
      historyData = hourlyHistory;
      historySize = HISTORY_SIZE_HOURLY;
      historyIndex = hourlyHistoryIndex;
      historyFull = hourlyHistoryFull;
      break;
    case HP_DAILY:
      historyData = dailyHistory;
      historySize = HISTORY_SIZE_DAILY;
      historyIndex = dailyHistoryIndex;
      historyFull = dailyHistoryFull;
      break;
    case HP_WEEKLY:
      historyData = weeklyHistory;
      historySize = HISTORY_SIZE_WEEKLY;
      historyIndex = weeklyHistoryIndex;
      historyFull = weeklyHistoryFull;
      break;
  }
  
  // 计算最大值和最小值，用于缩放
  float maxTemp = -100, minTemp = 100;
  float maxHumi = 0, minHumi = 100;
  uint16_t maxTvoc = 0, minTvoc = 65535;
  uint16_t maxEco2 = 0, minEco2 = 65535;
  
  int validPoints = historyFull ? historySize : historyIndex;
  
  for (int i = 0; i < validPoints; i++) {
    if (historyData[i].temperature > maxTemp) maxTemp = historyData[i].temperature;
    if (historyData[i].temperature < minTemp) minTemp = historyData[i].temperature;
    if (historyData[i].humidity > maxHumi) maxHumi = historyData[i].humidity;
    if (historyData[i].humidity < minHumi) minHumi = historyData[i].humidity;
    if (historyData[i].tvoc > maxTvoc) maxTvoc = historyData[i].tvoc;
    if (historyData[i].tvoc < minTvoc) minTvoc = historyData[i].tvoc;
    if (historyData[i].eco2 > maxEco2) maxEco2 = historyData[i].eco2;
    if (historyData[i].eco2 < minEco2) minEco2 = historyData[i].eco2;
  }
  
  // 防止除零错误
  if (maxTemp == minTemp) maxTemp = minTemp + 1;
  if (maxHumi == minHumi) maxHumi = minHumi + 1;
  if (maxTvoc == minTvoc) maxTvoc = minTvoc + 1;
  if (maxEco2 == minEco2) maxEco2 = minEco2 + 1;
  
  // 绘制数据曲线
  int startX = 40;
  int endY = 220;
  int graphHeight = 180;
  int graphWidth = 260;
  
  // 只有在有足够数据点时才绘制曲线
  if (validPoints > 1) {
    for (int i = 0; i < validPoints - 1; i++) {
      int idx1, idx2;
      if (historyFull) {
        idx1 = (historyIndex + i) % historySize;
        idx2 = (historyIndex + i + 1) % historySize;
      } else {
        idx1 = i;
        idx2 = i + 1;
      }
      
      // 绘制温度曲线 (蓝色)
      int x1_temp = startX + (i * graphWidth) / validPoints;
      int x2_temp = startX + ((i + 1) * graphWidth) / validPoints;
      int y1_temp = endY - (int)((historyData[idx1].temperature - minTemp) / (maxTemp - minTemp) * graphHeight);
      int y2_temp = endY - (int)((historyData[idx2].temperature - minTemp) / (maxTemp - minTemp) * graphHeight);
      tft.drawLine(x1_temp, y1_temp, x2_temp, y2_temp, CYBER_BLUE);
      
      // 绘制湿度曲线 (绿色)
      int y1_humi = endY - (int)((historyData[idx1].humidity - minHumi) / (maxHumi - minHumi) * graphHeight);
      int y2_humi = endY - (int)((historyData[idx2].humidity - minHumi) / (maxHumi - minHumi) * graphHeight);
      tft.drawLine(x1_temp, y1_humi, x2_temp, y2_humi, CYBER_GREEN);
      
      // 绘制TVOC曲线 (粉色)
      int y1_tvoc = endY - (int)((historyData[idx1].tvoc - minTvoc) / (maxTvoc - minTvoc) * graphHeight);
      int y2_tvoc = endY - (int)((historyData[idx2].tvoc - minTvoc) / (maxTvoc - minTvoc) * graphHeight);
      tft.drawLine(x1_temp, y1_tvoc, x2_temp, y2_tvoc, CYBER_PINK);
      
      // 绘制CO2曲线 (青色)
      int y1_eco2 = endY - (int)((historyData[idx1].eco2 - minEco2) / (maxEco2 - minEco2) * graphHeight);
      int y2_eco2 = endY - (int)((historyData[idx2].eco2 - minEco2) / (maxEco2 - minEco2) * graphHeight);
      tft.drawLine(x1_temp, y1_eco2, x2_temp, y2_eco2, CYBER_ACCENT);
    }
  } else {
    // 如果没有足够的数据点，显示提示信息
    displayChineseSmooth(100, 120, "No Data Yet", ILI9341_RED);
  }
  
  // 绘制当前周期标签
  const char* periodLabels[] = {"Hourly", "Daily", "Weekly"};
  tft.fillRect(190, 10, 120, 20, CYBER_BG);  // 清除旧标签
  displayChineseSmooth(200, 16, periodLabels[currentHistoryPeriod], CYBER_BLUE);
}

// 切换历史周期
void switchHistoryPeriod() {
  currentHistoryPeriod = (HistoryPeriod)((currentHistoryPeriod + 1) % 3);
  initHistoryStaticUI();  // 重新初始化静态UI
  drawHistoryGraphs();    // 重新绘制图表
}
*/

// 闹钟逻辑已被移除
void checkAlarmTrigger() {
  // 闹钟功能已被移除，此函数为空实现
}

// ========= 警报视觉+声音 =========

void connectToMQTT() {
  if (!mqttConfig.enabled) return;
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, cannot connect to MQTT");
    return;
  }
  
  Serial.printf("Connecting to MQTT broker at %s:%d\n", mqttConfig.server, mqttConfig.port);
  
  // 设置MQTT服务器
  mqttClient.setServer(mqttConfig.server, mqttConfig.port);
  
  // 尝试连接
  if (strlen(mqttConfig.username) > 0 && strlen(mqttConfig.password) > 0) {
    // 使用用户名和密码连接
    if (mqttClient.connect(mqttConfig.clientId, mqttConfig.username, mqttConfig.password)) {
      Serial.println("MQTT connected successfully");
    } else {
      Serial.printf("MQTT connection failed, state: %d\n", mqttClient.state());
    }
  } else {
    // 不使用认证连接
    if (mqttClient.connect(mqttConfig.clientId)) {
      Serial.println("MQTT connected successfully");
    } else {
      Serial.printf("MQTT connection failed, state: %d\n", mqttClient.state());
    }
  }
}

void publishSensorData() {
  if (!mqttConfig.enabled) return;
  
  if (!mqttClient.connected()) {
    connectToMQTT();
  }
  
  if (mqttClient.connected()) {
    // 创建JSON格式的传感器数据
    String json = "{\"temperature\":" + String(current_env_data.temperature, 2) + 
                  ",\"humidity\":" + String(current_env_data.humidity, 2) + 
                  ",\"tvoc\":" + String(current_env_data.tvoc) + 
                  ",\"eco2\":" + String(current_env_data.eco2) + 
                  ",\"aqi\":" + String(current_env_data.aqi) + 
                  ",\"timestamp\":" + String(millis()) + "}";
    
    Serial.printf("Publishing to MQTT: %s\n", json.c_str());
    
    // 发布数据到MQTT主题
    if (mqttClient.publish(mqttConfig.topic, json.c_str())) {
      Serial.println("MQTT publish successful");
    } else {
      Serial.println("MQTT publish failed");
    }
  }
}

void updateAlertStateAndLED() {
  if (ens160_initialized && current_env_data.eco2 > 1800) currentAlertLevel = ALERT_CO2;  // 只有在ENS160初始化成功时才检查CO2警报
  else currentAlertLevel = ALERT_NONE;

  unsigned long now = millis();

  unsigned long interval;
  if (currentAlertLevel == ALERT_CO2)   interval = 250;
  else                                  interval = 1000;

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

  // pinMode(ENC_A_PIN,   INPUT_PULLUP);
  // pinMode(ENC_B_PIN,   INPUT_PULLUP);
  // pinMode(ENC_BTN_PIN, INPUT_PULLUP);
  pinMode(KEY0_PIN,    INPUT_PULLUP);
  pinMode(KEY1_PIN,    INPUT_PULLUP);  // 初始化新增按键引脚

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
  lastTimeDisplayUpdate = millis();  // 初始化时间显示更新时间
  
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

  // int encStep     = readEncoderStep();
  // bool encPressed = checkButtonPressed(ENC_BTN_PIN, lastEncBtn);
  bool k0Pressed  = checkButtonPressed(KEY0_PIN,    lastKey0);
  bool k1Pressed  = checkButtonPressed(KEY1_PIN,    lastKey1);  // 检测新增按键
  
  // 检测KEY0长按复位功能
  handleLongPressReset();
  
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
  
  // 处理MQTT连接和数据发布
  if (WiFi.status() == WL_CONNECTED && mqttConfig.enabled) {
    // 每隔一定时间发布一次传感器数据
    static unsigned long lastMQTTPublish = 0;
    if (millis() - lastMQTTPublish > 60000) {  // 每分钟发布一次
      publishSensorData();
      lastMQTTPublish = millis();
    }
    
    // 让MQTT客户端处理消息
    mqttClient.loop();
  }

  switch (currentMode) {
    case MODE_MENU: {
      // if (encStep != 0) {
      //   menuIndex += encStep;
      //   if (menuIndex < 0) menuIndex = MENU_ITEMS - 1;
      //   if (menuIndex >= MENU_ITEMS) menuIndex = 0;
      //   drawMenu();
      // }
      // if (encPressed) {
      //   if (menuIndex == 0) {
      //     currentMode = MODE_CLOCK;
      //     initClockStaticUI();
      //     prevTimeStr = "";
      //     drawClockTime(getTimeStr('H'), getTimeStr('M'), getTimeStr('S'));
      //     drawEnvDynamic(current_env_data.temperature, current_env_data.humidity, current_env_data.tvoc, current_env_data.eco2);
      //   } else if (menuIndex == 1) {
      //     currentMode = MODE_ALARM;
      //     alarmSelectedField = 0;
      //     drawAlarmScreen(true);
      //   } else if (menuIndex == 2) {  // 新增环境参数显示模式
      //     currentMode = MODE_ENVIRONMENT;
      //     initEnvironmentStaticUI();
      //   }
      // }
      
      // 新按键用于循环切换模式
      if (k1Pressed) {
        menuIndex++;
        if (menuIndex >= MENU_ITEMS) menuIndex = 0;
        
        if (menuIndex == 0) {
          currentMode = MODE_CLOCK;
          initClockStaticUI();
          prevTimeStr = "";
          lastTimeDisplayUpdate = millis();  // 初始化时间显示更新时间
          drawClockTime(getTimeStr('H'), getTimeStr('M'), getTimeStr('S'));
          drawEnvDynamic(current_env_data.temperature, current_env_data.humidity, current_env_data.tvoc, current_env_data.eco2);
        } else if (menuIndex == 1) {  // 环境参数显示模式
          currentMode = MODE_ENVIRONMENT;
          initEnvironmentStaticUI();
        } else if (menuIndex == 2) {  // 日期时间显示模式
          currentMode = MODE_DATE_TIME;
          drawDateTimePage();
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
        // 只有当时间有效时才绘制时钟，且每30秒最多更新一次显示
        unsigned long currentTime = millis();
        if (currentTime - lastTimeDisplayUpdate >= 30000) {  // 30秒更新一次显示
          drawClockTime(hourStr, minStr, secStr);
          lastTimeDisplayUpdate = currentTime;
        }
      } else {
        // 如果时间无效，显示错误信息
        Serial.println("时间无效，显示默认时间");
      }
      
      // 在时钟模式下也实时更新环境数据显示，保持与环境参数四等分显示模式的一致性
      // 同时更新第一页专用温度值
      first_page_temperature = current_env_data.temperature;
      drawEnvDynamic(current_env_data.temperature, current_env_data.humidity, current_env_data.tvoc, current_env_data.eco2);
      
      if (k0Pressed) {
        currentMode = MODE_MENU;
        drawMenu();
      }
      
      // 新按键用于循环切换页面：时钟->环境参数->日期时间
      if (k1Pressed) {
        currentMode = MODE_ENVIRONMENT;
        initEnvironmentStaticUI();
      }
      break;
    }



    case MODE_ENVIRONMENT: {
      // 每15秒更新一次环境数据显示
      unsigned long currentTime = millis();
      if (currentTime - lastTimeDisplayUpdate >= 15000) {  // 15秒更新一次显示
        drawEnvQuadrant(current_env_data.humidity, first_page_temperature, 
                        current_env_data.tvoc, current_env_data.eco2);
        lastTimeDisplayUpdate = currentTime;
      }
      
      if (k0Pressed) {
        currentMode = MODE_MENU;
        drawMenu();
      }
      
      // 新按键用于循环切换页面：环境参数->日期时间->时钟
      if (k1Pressed) {
        currentMode = MODE_DATE_TIME;
        drawDateTimePage();
      }
      break;
    }

    case MODE_DATE_TIME: {
      // 检查时间是否有效
      struct tm timeinfo;
      if (getLocalTime(&timeinfo)) {
        // 每30秒更新一次显示
        unsigned long currentTime = millis();
        if (currentTime - lastTimeDisplayUpdate >= 30000) {  // 30秒更新一次显示
          drawDateTimePage();
          lastTimeDisplayUpdate = currentTime;
        }
      } else {
        // 如果时间无效，显示错误信息
        Serial.println("日期时间页面：时间无效");
      }
      
      if (k0Pressed) {
        currentMode = MODE_MENU;
        drawMenu();
      }
      
      // 新按键用于循环切换页面：日期时间->时钟->环境参数
      if (k1Pressed) {
        currentMode = MODE_CLOCK;
        initClockStaticUI();
        prevTimeStr = "";
        lastTimeDisplayUpdate = millis();  // 初始化时间显示更新时间
        drawClockTime(getTimeStr('H'), getTimeStr('M'), getTimeStr('S'));
        drawEnvDynamic(current_env_data.temperature, current_env_data.humidity, current_env_data.tvoc, current_env_data.eco2);
      }
      break;
    }

    /*
    case MODE_HISTORY: {
      // 绘制历史曲线
      drawHistoryGraphs();
      
      if (k0Pressed) {
        currentMode = MODE_MENU;
        drawMenu();
      }
      
      // 新按键用于切换到下一历史周期
      if (k1Pressed) {
        switchHistoryPeriod();  // 切换小时/天/周显示
      }
      break;
    }
    */

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
  displayChineseSmooth(20, 50, "AP Config Mode", CYBER_LIGHT);
  
  String ssidText = "SSID: " + String(AP_SSID);
  String pwdText = "PWD: " + String(AP_PASSWORD);
  
  displayChineseSmooth(20, 80, ssidText.c_str(), CYBER_LIGHT);
  displayChineseSmooth(20, 110, pwdText.c_str(), CYBER_LIGHT);
  displayChineseSmooth(20, 140, "IP: 192.168.4.1", CYBER_LIGHT);
  displayChineseSmooth(20, 170, "Open Browser:", CYBER_LIGHT);
  displayChineseSmooth(20, 200, "http://192.168.4.1", CYBER_LIGHT);

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

void loadMQTTConfig() {
  preferences.begin("MQTT_Config", true);
  
  // 读取MQTT服务器配置
  String serverStr = preferences.getString("MQTT_Server", MQTT_DEFAULT_SERVER);
  serverStr.toCharArray(mqttConfig.server, sizeof(mqttConfig.server));
  
  mqttConfig.port = preferences.getUInt("MQTT_Port", MQTT_DEFAULT_PORT);
  
  // 读取MQTT认证信息
  String usernameStr = preferences.getString("MQTT_Username", "");
  usernameStr.toCharArray(mqttConfig.username, sizeof(mqttConfig.username));
  
  String passwordStr = preferences.getString("MQTT_Password", "");
  passwordStr.toCharArray(mqttConfig.password, sizeof(mqttConfig.password));
  
  String clientIdStr = preferences.getString("MQTT_ClientId", "ESP32_EnvMonitor");
  clientIdStr.toCharArray(mqttConfig.clientId, sizeof(mqttConfig.clientId));
  
  // 读取主题和启用状态
  String topicStr = preferences.getString("MQTT_Topic", MQTT_DEFAULT_TOPIC);
  topicStr.toCharArray(mqttConfig.topic, sizeof(mqttConfig.topic));
  
  mqttConfig.enabled = preferences.getBool("MQTT_Enabled", MQTT_DEFAULT_ENABLED);
  
  preferences.end();
  
  Serial.printf("=== Loaded MQTT config: server=%s, port=%d, topic=%s, enabled=%s ===\n", 
                mqttConfig.server, mqttConfig.port, mqttConfig.topic, mqttConfig.enabled ? "true" : "false");
}

void saveMQTTConfig() {
  preferences.begin("MQTT_Config", false);
  
  preferences.putString("MQTT_Server", String(mqttConfig.server));
  preferences.putUInt("MQTT_Port", mqttConfig.port);
  preferences.putString("MQTT_Username", String(mqttConfig.username));
  preferences.putString("MQTT_Password", String(mqttConfig.password));
  preferences.putString("MQTT_ClientId", String(mqttConfig.clientId));
  preferences.putString("MQTT_Topic", String(mqttConfig.topic));
  preferences.putBool("MQTT_Enabled", mqttConfig.enabled);
  
  preferences.end();
  
  Serial.printf("=== Saved MQTT config: server=%s, port=%d, topic=%s, enabled=%s ===\n", 
                mqttConfig.server, mqttConfig.port, mqttConfig.topic, mqttConfig.enabled ? "true" : "false");
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

bool apModeInitiated = false;  // 全局变量，标记AP模式是否已经启动过

void connectWiFiAndSyncTime() {
  tft.fillScreen(CYBER_BG);
  displayChineseSmooth(20, 110, "Loading WiFi Config", CYBER_LIGHT);
  
  // 1. 从Flash加载已保存的WiFi信息
  loadWiFiFromFlash();
  
  // 加载温度校准值
  loadTempCalibration();
  
  // 加载MQTT配置
  loadMQTTConfig();

  // 2. 尝试自动连接已存储的WiFi
  autoConnectWiFi();
  
  // 3. 如果自动连接失败，等待一段时间看是否会连接成功
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("=== Auto connection failed, waiting a bit more... ===");
    // 再等待一段时间，因为某些网络可能连接较慢
    int waitCount = 0;
    const int maxWaitAttempts = 10; // 最多重试10次，每次500ms，总共约5秒
    
    while (WiFi.status() != WL_CONNECTED && waitCount < maxWaitAttempts) {
      delay(500);
      waitCount++;
      Serial.printf("Waiting for WiFi connection... (%d/%d)\n", waitCount, maxWaitAttempts);
    }
    
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("=== Still couldn't connect to WiFi ===");
      
      // 检查是否已经启动过AP模式，避免重复进入长时间等待
      if (!apModeInitiated) {
        Serial.println("=== Entering AP mode for configuration ===");
        apModeInitiated = true;  // 标记AP模式已启动
        initAPMode();
      } else {
        Serial.println("=== AP mode already initiated, skipping to basic operation mode ===");
        // 如果AP模式已经启动过，直接进入基本工作模式，不重复进入AP等待循环
      }
    } else {
      Serial.println("=== WiFi connected after waiting ===");
    }
  }
  
  // 如果WiFi连接成功，进行时间同步
  if (WiFi.status() == WL_CONNECTED) {
    // 尝试多个时间服务器进行时间同步
    bool timeSyncSuccess = false;
    struct tm timeinfo;
    
    for (int i = 0; i < numNtpServers && !timeSyncSuccess; i++) {
      Serial.printf("尝试时间服务器: %s (%d/%d)\n", ntpServers[i], i+1, numNtpServers);
      configTime(gmtOffset_sec, daylightOffset_sec, ntpServers[i]);
      
      tft.fillScreen(CYBER_BG);
      displayChineseSmooth(20, 110, "Syncing Time...", CYBER_LIGHT);
      char serverDisplay[50];
      strcpy(serverDisplay, "Server: ");
      strcat(serverDisplay, ntpServers[i]);
      displayChineseSmooth(20, 130, serverDisplay, CYBER_LIGHT);
      
      // 等待时间同步，但设置超时以避免无限等待
      unsigned long syncStartTime = millis();
      const unsigned long syncTimeout = 5000; // 5秒超时
      
      // 显示同步进度
      while(millis() - syncStartTime < syncTimeout) {
        if (getLocalTime(&timeinfo)) {
          Serial.printf("时间同步成功，使用服务器: %s\n", ntpServers[i]);
          timeSyncSuccess = true;
          break;
        }
        delay(100);
        
        // 每秒更新一次显示，让用户知道仍在同步中
        if ((millis() / 1000) % 2 == 0) {
          tft.fillRect(20, 150, 280, 20, CYBER_BG); // 清除之前的内容
          displayChineseSmooth(20, 150, "Time Syncing...", CYBER_LIGHT);
        } else {
          tft.fillRect(20, 150, 280, 20, CYBER_BG); // 清除之前的内容
          displayChineseSmooth(20, 150, "Time Syncing..", CYBER_LIGHT);
        }
      }
    }
    
    // 检查是否所有服务器都同步失败
    if (!timeSyncSuccess) {
      Serial.println("所有时间服务器同步失败，将使用设备启动时间作为基准");
      tft.fillRect(20, 150, 280, 20, CYBER_BG);
      displayChineseSmooth(20, 150, "Time Sync Timeout", ILI9341_RED);
      
      // 不设置具体时间，而是标记时间无效，让后续的getTimeStr函数处理
      timeCacheValid = false;
      
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
    lastTimeDisplayUpdate = millis();  // 初始化时间显示更新时间
    
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
    // 即使WiFi连接失败，也显示错误信息并进入基本工作模式
    tft.fillScreen(CYBER_BG);
    displayChineseSmooth(20, 110, "WiFi Config Failed!", ILI9341_RED);
    
    // 等待几秒后继续进入基本界面，而不是完全失败
    delay(2000);
    
    // 初始化基础界面
    initClockStaticUI();
    prevTimeStr = "";
    
    // 即使没有WiFi，也显示本地时间（基于系统启动时间）
    String hourStr = getTimeStr('H');
    String minStr = getTimeStr('M');
    String secStr = getTimeStr('S');
    
    if (hourStr != "--" && minStr != "--" && secStr != "--") {
      drawClockTime(hourStr, minStr, secStr);
    }
    
    drawEnvDynamic(current_env_data.temperature, current_env_data.humidity, current_env_data.tvoc, current_env_data.eco2);
  }
}


// Web服务器处理函数
void sendMainConfigPage() {
  String html = "<!DOCTYPE html>\n";
  html += "<html><head><title>Air-Quality-Monitor Configuration</title>";
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
  html += "<h1>Air-Quality-Monitor Configuration</h1>";
  html += "<p>Current IP: " + WiFi.localIP().toString() + "</p>";
  html += "<button onclick='window.location.href=\"/calibrateTemp\"'>Device Configuration</button>";
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
  html += "<html><head><title>Device Configuration</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 40px; background-color: #f0f0f0; color: black; }";
  html += "h1 { color: black; text-align: center; }";
  html += ".container { max-width: 400px; margin: 0 auto; padding: 20px; background: white; border-radius: 8px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
  html += "input[type='number'], input[type='text'], input[type='password'] { width: 100%; padding: 12px; margin: 10px 0; box-sizing: border-box; border: 1px solid #ddd; border-radius: 4px; color: black; }";
  html += "input[type='submit'] { width: 100%; background-color: #4CAF50; color: white; padding: 14px 20px; margin: 10px 0; border: none; border-radius: 4px; cursor: pointer; }";
  html += "input[type='submit']:hover { background-color: #45a049; }";
  html += "button { width: 100%; background-color: #2196F3; color: white; padding: 14px 20px; margin: 10px 0; border: none; border-radius: 4px; cursor: pointer; }";
  html += "button:hover { background-color: #0b7dda; }";
  html += "label { color: black; }";
  html += ".section { margin-bottom: 20px; padding: 15px; border: 1px solid #ddd; border-radius: 4px; }";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h1>Device Configuration</h1>";
  
  // 温度校准部分
  html += "<div class='section'>";
  html += "<h2>Temperature Calibration</h2>";
  html += "<form action='/calibrateTemp' method='POST'>";
  html += "<label for='offset'>Calibration Offset (°C):</label>";
  html += "<input type='number' id='offset' name='offset' step='0.1' value='" + String(TEMP_CALIBRATION_OFFSET, 2) + "' min='-20' max='20'><br>";
  html += "<input type='submit' value='Save Calibration'>";
  html += "</form>";
  html += "</div>";
  
  // MQTT配置部分
  html += "<div class='section'>";
  html += "<h2>MQTT Configuration</h2>";
  html += "<form action='/configMQTT' method='POST'>";
  html += "<label for='mqtt_server'>MQTT Server:</label>";
  html += "<input type='text' id='mqtt_server' name='mqtt_server' value='" + String(mqttConfig.server) + "'><br>";
  html += "<label for='mqtt_port'>MQTT Port:</label>";
  html += "<input type='number' id='mqtt_port' name='mqtt_port' value='" + String(mqttConfig.port) + "' min='1' max='65535'><br>";
  html += "<label for='mqtt_username'>Username (optional):</label>";
  html += "<input type='text' id='mqtt_username' name='mqtt_username' value='" + String(mqttConfig.username) + "'><br>";
  html += "<label for='mqtt_password'>Password (optional):</label>";
  html += "<input type='password' id='mqtt_password' name='mqtt_password' value='" + String(mqttConfig.password) + "'><br>";
  html += "<label for='mqtt_topic'>Topic:</label>";
  html += "<input type='text' id='mqtt_topic' name='mqtt_topic' value='" + String(mqttConfig.topic) + "'><br>";
  html += "<label for='mqtt_client_id'>Client ID:</label>";
  html += "<input type='text' id='mqtt_client_id' name='mqtt_client_id' value='" + String(mqttConfig.clientId) + "'><br>";
  html += "<label for='mqtt_enabled'>Enable MQTT:</label>";
  html += "<input type='checkbox' id='mqtt_enabled' name='mqtt_enabled' " + String(mqttConfig.enabled ? "checked" : "") + "><br>";
  html += "<input type='submit' value='Save MQTT Config'>";
  html += "</form>";
  html += "</div>";
  
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

void handleMQTTConfig() {
  Serial.println("MQTT configuration request received");
  
  // 获取表单参数
  String serverStr = backgroundServer.arg("mqtt_server");
  String portStr = backgroundServer.arg("mqtt_port");
  String usernameStr = backgroundServer.arg("mqtt_username");
  String passwordStr = backgroundServer.arg("mqtt_password");
  String topicStr = backgroundServer.arg("mqtt_topic");
  String clientIdStr = backgroundServer.arg("mqtt_client_id");
  String enabledStr = backgroundServer.arg("mqtt_enabled");
  
  Serial.printf("Received MQTT config: server=%s, port=%s, topic=%s, enabled=%s\n", 
                serverStr.c_str(), portStr.c_str(), topicStr.c_str(), enabledStr.c_str());
  
  // 更新MQTT配置
  serverStr.toCharArray(mqttConfig.server, sizeof(mqttConfig.server));
  mqttConfig.port = portStr.toInt();
  usernameStr.toCharArray(mqttConfig.username, sizeof(mqttConfig.username));
  passwordStr.toCharArray(mqttConfig.password, sizeof(mqttConfig.password));
  topicStr.toCharArray(mqttConfig.topic, sizeof(mqttConfig.topic));
  clientIdStr.toCharArray(mqttConfig.clientId, sizeof(mqttConfig.clientId));
  mqttConfig.enabled = (enabledStr == "on");
  
  // 保存配置到Flash
  saveMQTTConfig();
  
  // 发送成功页面
  String successHtml = "<!DOCTYPE html>\n";
  successHtml += "<html><head><title>MQTT Config Success</title>";
  successHtml += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  successHtml += "<style>";
  successHtml += "body { font-family: Arial, sans-serif; margin: 40px; text-align: center; color: black; }";
  successHtml += ".success { color: green; font-size: 18px; }";
  successHtml += "</style>";
  successHtml += "</head><body>";
  successHtml += "<h1>MQTT Config Success!</h1>";
  successHtml += "<p class='success'>MQTT configuration updated successfully</p>";
  successHtml += "<p><a href='/'>Return to Config Page</a> | <a href='/calibrateTemp'>Config Again</a></p>";
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
  backgroundServer.on("/configMQTT", HTTP_POST, handleMQTTConfig);
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
  html += "body { font-family: Arial, sans-serif; margin: 40px; background-color: #f0f0f0; color: blue; }";
  html += "h1 { color: blue; text-align: center; }";
  html += ".container { max-width: 400px; margin: 0 auto; padding: 20px; background: white; border-radius: 8px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
  html += "input[type='text'], input[type='password'] { width: 100%; padding: 12px; margin: 10px 0; box-sizing: border-box; border: 1px solid #ddd; border-radius: 4px; color: blue; }";
  html += "input[type='submit'] { width: 100%; background-color: #4CAF50; color: white; padding: 14px 20px; margin: 10px 0; border: none; border-radius: 4px; cursor: pointer; }";
  html += "input[type='submit']:hover { background-color: #45a049; }";
  html += "button { width: 100%; background-color: #2196F3; color: white; padding: 14px 20px; margin: 10px 0; border: none; border-radius: 4px; cursor: pointer; }";
  html += "button:hover { background-color: #0b7dda; }";
  html += "select { width: 100%; padding: 12px; margin: 10px 0; box-sizing: border-box; border: 1px solid #ddd; border-radius: 4px; color: blue; }";
  html += "label { color: blue; }";
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
      
      // 发送成功页面，包含自动跳转到工作页面的脚本
      String successHtml = "<!DOCTYPE html>\n";
      successHtml += "<html><head><title>Configuration Success</title>";
      successHtml += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
      successHtml += "<style>";
      successHtml += "body { font-family: Arial, sans-serif; margin: 40px; text-align: center; color: blue; }";
      successHtml += ".success { color: green; font-size: 18px; }";
      successHtml += "</style>";
      
      String jsScript = "<script>setTimeout(function(){ window.location.href = 'http://" + WiFi.localIP().toString() + ":8080'; }, 3000);</script>";  // 3秒后跳转到工作页面
      successHtml += jsScript;
      
      successHtml += "</head><body>";
      successHtml += "<h1>Configuration Success!</h1>";
      successHtml += "<p class='success'>Connected to WiFi: " + ssid + "</p>";
      successHtml += "<p>Device will restart to apply settings in 3 seconds</p>";
      successHtml += "<p>Redirecting to work page...</p>";
      successHtml += "</body></html>";
      
      server.send(200, "text/html", successHtml);
      
      delay(3000);
      Serial.println("=== WiFi配置成功，设备将重启以应用新设置 ===");
      
      // 确保服务器停止后重启
      server.stop();
      WiFi.softAPdisconnect(true); // 断开AP模式
      ESP.restart(); // 重启ESP32以应用新的WiFi设置
      return;
    } else {
      Serial.println("=== Web config failed, target WiFi cannot connect ===");
      
      // 发送错误页面，包含自动跳转到工作页面的脚本
      String errorHtml = "<!DOCTYPE html>\n";
      errorHtml += "<html><head><title>Configuration Failed</title>";
      errorHtml += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
      errorHtml += "<style>";
      errorHtml += "body { font-family: Arial, sans-serif; margin: 40px; text-align: center; color: blue; }";
      errorHtml += ".error { color: red; font-size: 18px; }";
      errorHtml += "</style>";
      
      String jsErrorScript = "<script>setTimeout(function(){ window.location.href = 'http://" + WiFi.localIP().toString() + ":8080'; }, 5000);</script>";  // 5秒后跳转到工作页面
      errorHtml += jsErrorScript;
      
      errorHtml += "</head><body>";
      errorHtml += "<h1>Configuration Failed!</h1>";
      errorHtml += "<p class='error'>Cannot connect to WiFi: " + ssid + "</p>";
      errorHtml += "<p>Will exit AP mode and enter normal operation...</p>";
      errorHtml += "<p>Redirecting to work page in 5 seconds...</p>";
      errorHtml += "<p><a href='/'>Try Again</a></p>";
      errorHtml += "</body></html>";
      
      server.send(200, "text/html", errorHtml);
      
      // 即使连接失败，也等待一段时间后退出AP模式并进入正常工作流程
      delay(5000);
      
      // 停止AP模式服务器并进入正常工作模式
      server.stop();
      WiFi.softAPdisconnect(true); // 断开AP模式
      WiFi.mode(WIFI_STA); // 确保处于STA模式
      
      // 重新初始化完整的工作流程
      loadWiFiFromFlash();
      loadTempCalibration();
      loadMQTTConfig();
      
      // 尝试连接其他已保存的WiFi
      autoConnectWiFi();
      
      // 初始化后台Web服务器
      initBackgroundWebServer();
      
      initClockStaticUI();
      prevTimeStr = "";
      lastTimeDisplayUpdate = millis();  // 初始化时间显示更新时间
      
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
  } else {
    // 如果没有收到参数，发送配置页面
    sendConfigPage();
  }
}