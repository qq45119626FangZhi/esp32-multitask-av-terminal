AI语音聊天，视频监控项目

下机位主要功能

  1. 主控芯片选型：`ESP32-S3FN8`
   1. Xtensa® 32 位 LX7 **双核**处理器, 时钟频率：最高 240 MHz
   2. 384 KB ROM：用于程序启动和内核功能调用
   3. 512 KB 片上 SRAM：用于数据和指令存储，时钟频率可配置，最大 240 MH
   4. 8MB片内Flash(用于烧写程序)
   5. 45 个可编程 GPIO
   6.  2 个 SPI 接口用于连接 flash 和 RAM
   7. 2 个通用 SPI 接口
   8.  LCD 接口
   9.  DVP 8 位 + 16 位摄像头接口
   10.  3 个 UART 接口
   11. 2 个 I2C 接口
   12.  2 个 I2S 接口
   13. RMT (TX/RX)
   14. 4个54 位通用定时器 , 1个52位系统定时器和 3个看门狗定时器

  <img width="2035" height="1279" alt="6765e22489e9a040207c45bebacf4d72" src="https://github.com/user-attachments/assets/c57566ca-cc13-4a10-999d-7afb91f0c81a" />
  
  2、蓝牙wifi 低功耗蓝牙（基于 GATT）传输方式，二维码扫描配网，SW3重新复位配网，需要重新扫描连接，配网成功，自动连接已配置的wifi
 
 <img width="394" height="685" alt="image" src="https://github.com/user-attachments/assets/565fc6bd-4358-458e-8722-270f7d6906c2" />

  <img width="389" height="402" alt="image" src="https://github.com/user-attachments/assets/b2d77b5d-301a-487d-a1c6-22289bf15e7b" />

 <img width="385" height="537" alt="image" src="https://github.com/user-attachments/assets/b215888d-e146-4eaf-b801-4a2464052c76" />

  
  3、LVGL渲染 ，配网成功，欢迎页面，也是语音交互页面，可语音切换模式，按键切换模式
  
  <img width="2270" height="1024" alt="25308522e5c8f1d3b0935bd8fb1347d2" src="https://github.com/user-attachments/assets/93bc2cd8-6314-44fe-9e97-c7b68d730036" />


  
  4、语音模式，AI智能聊天，采用DeepSeek大模型，使用websocekt通信,任何模式都可以语音交互
  
  语音交换流程：<img width="1565" height="612" alt="语音处理架构" src="https://github.com/user-attachments/assets/5ce26760-8180-4746-aef0-a897b18d8bf8" />
  
  开发参考虾哥开源文档：https://xiaozhi.dev/docs/ \n
  
  虾哥提供的后端交互智能体平台：<img width="696" height="373" alt="image" src="https://github.com/user-attachments/assets/92bf93ac-bc71-4b7e-b404-44ce2e04d9ac" />

  5、门铃模式
    <img width="2837" height="1280" alt="2bb18505b8b0a1b49e578ae8dadbd152" src="https://github.com/user-attachments/assets/80c03d2e-95ba-43f2-a7e0-6b7f104bdcec" />

  6、监控模式，摄像头OV2640驱动 缓存监控录像到SD卡，使用http协议，推流摄像头数据到上机位展示
  <img width="2837" height="1280" alt="8e2178661c4c8bf709dc2b7981fc0747" src="https://github.com/user-attachments/assets/a9df8302-fc71-42cb-9aaa-9383b7c48286" />

  7、OTA升级
  <img width="574" height="276" alt="592f1d728367d209d7663c4b0b5562f2" src="https://github.com/user-attachments/assets/abb2e805-5a6d-4645-8be2-81428dac5622" />

  8、版本信息

上机位

安卓和HTML分别实现，控制切换模式，安卓扫描二维码，使用蓝牙配网，接收下机位监控画面，展示SD卡数据

安卓界面

<img width="387" height="823" alt="image" src="https://github.com/user-attachments/assets/dc7164fd-85b0-4913-9279-0e9b85900c21" />
<img width="380" height="814" alt="image" src="https://github.com/user-attachments/assets/911c1bb3-70bd-45c1-bcbb-5aa088538512" />
<img width="1260" height="2800" alt="6544abf787e319ba6407cc297d8cf4af" src="https://github.com/user-attachments/assets/3a5c03ad-6882-4563-b932-2130d9a4bb6d" />


HTML界面

<img width="385" height="818" alt="image" src="https://github.com/user-attachments/assets/dec74102-142d-4aae-a288-20d6b038570e" />

