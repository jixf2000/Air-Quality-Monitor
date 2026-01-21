# Air-Quality-Monitor
Air Quality Monitor ESP32

ver 1.30

1. 实现了用户界面配色功能，允许用户通过Web界面自定义设备的底色和字体颜色
2. 添加了UI颜色变量定义（UI_BG, UI_LIGHT, UI_ACCENT等）和相应的默认值
3. 实现了颜色配置的保存和加载功能，使用Preferences库持久化存储
4. 添加了RGB565与HEX颜色格式之间的转换函数
5. 在Web配置界面中添加了颜色配置选项，使用HTML颜色选择器

<img width="381" height="376" alt="UI Color config" src="https://github.com/user-attachments/assets/d34f7598-78cb-4e10-b884-597d8ff76ebc" />






ver1.20
1. 修改bug;
2. 增加配置页面；

V1.10  
2026.1.20
现在设备具备了以下功能：
OTA更新页面访问：通过设备的配置页面（通常是 http://设备IP:8080）可以访问OTA更新功能
固件上传：用户可以通过网页界面选择并上传新的固件文件（.bin文件）
进度显示：上传过程中会显示进度条，让用户了解更新进度
自动重启：更新成功后设备会自动重启并应用新固件
错误处理：如果更新失败，会有相应的错误提示

![OTA](https://github.com/user-attachments/assets/9d96fb07-7bb3-4eeb-872c-a03fda0ec6cf)



2026.1.18
1. 增加适配2.2寸-ILI9341-esp32c3-mini-pro 版本；
2. 增加适配2.2寸-ILI9341-esp32c3-mini-pro bin版本；

2026.1.17
1. 增加适配3.2寸-st7789-esp32s3;
2. 增加适配3.2寸-st7789屏幕开孔的3d 模型（壳体/面板/底座/mcu座）

2026.1.13
1. 调整显示界面；
2. 增加MCU版本，适配 3.2寸-ILI9341-ESP32S3;
3. 增加MCU版本，适配 2.2寸-ILI9341-ESP32S3;
4. 增加MCU版本, 适配 2.4寸-ST7789-ESP32S3;

2026.1.9 ver1.0

1. 调整第一页面字体；
2. 增加第二页面 环境参数显示；
3. 增加第三页面 后台配置页面:IP:8080；可以修订温度补偿值；
4. 增加MQTT配置界面；实现参数推送；
5. 增加 一个按钮动作，切换三个页面；
   




