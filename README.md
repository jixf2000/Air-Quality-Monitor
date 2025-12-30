# Air-Quality-Monitor
Air Quality Monitor ESP32
// 引用库文件
#include <WiFi.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
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




精简代码，仅实现空气质量监测功能；
2025.12.30
1. 精简代码；
2. 补充wifi ap 配网模式，第一次登录，访问内置wifi:
