# StickS3 Remote firmware

这里是 StickS3 Remote 的 ESP-IDF 生产固件。当前版本已在真实 StickS3、Joystick2、MacBook Air 和 iPhone 上验证。

主要运行行为：

- 自动检测 I²C 地址 `0x63` 的 Joystick2，并通过顶部红色/灰色 `J` 徽标显示状态。
- Mac 模式使用摇杆左右方向键控制浏览器视频后退/前进，单击仍发送播放/暂停媒体键。
- 20 秒无操作后关闭 LCD，5 分钟无操作后切断 Joystick2 的 Grove 5V；按 A/B 恢复模块。
- 启用 CPU 动态频率调节和 BLE modem sleep，并通过串口周期输出电池电压。

完整的功能、按键、依赖和烧录说明见仓库根目录的 [README](../README.md)。后续开发计划见 [TODO](TODO.md)。
