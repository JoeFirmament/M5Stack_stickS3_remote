# StickS3 Remote firmware

这里是 StickS3 Remote 的 ESP-IDF 生产固件。当前版本已在真实 StickS3、Joystick2、MacBook Air 和 iPhone 上验证。

主要运行行为：

- 自动检测 I²C 地址 `0x63` 的 Joystick2，并通过顶部红色/灰色 `J` 徽标显示状态。
- Mac 模式使用摇杆左右方向键控制浏览器视频后退/前进，单击仍发送播放/暂停媒体键。
- 20 秒无操作后关闭 LCD，5 分钟无操作后切断 Joystick2 的 Grove 5V；拿起整机或按 A/B 恢复模块。
- BMI270 使用 25Hz 低功耗加速度模式，在熄屏时以 10Hz 采样；动作变化达到 `0.32g` 且连续出现两次才唤醒，避免桌面轻碰误触发，同时不中断 BLE 连接。
- Joystick2 断电后唤醒采用 Grove 5V → 模块启动/探测 → LCD 的顺序，降低低电量时的瞬时负载并避免 Brownout。
- 启用 CPU 动态频率调节和 BLE modem sleep，并通过串口周期输出电池电压。

## 低功耗实现参数

参数集中在 `main/stick_s3_ui.cpp`：

| 参数 | 当前值 | 说明 |
| --- | --- | --- |
| `kDisplaySleepMs` | 20 秒 | LCD 自动休眠 |
| `kJoystickPowerSaveMs` | 5 分钟 | Grove 5V 自动关闭 |
| `kMotionPollMs` | 100ms | 熄屏后的 BMI270 采样周期 |
| `kMotionArmDelayMs` | 600ms | 熄屏后动作检测保护时间 |
| `kMotionThresholdG` | `0.32g` | 相对基线的三轴矢量变化阈值 |
| `kMotionSamplesRequired` | 2 | 连续命中次数 |

传感器寄存器配置为 `ACC_CONF=0x26`、`PWR_CTRL=0x04`、`PWR_CONF=0x01`：25Hz 低功耗加速度计开启，陀螺仪、温度和 AUX 关闭，Advanced Power Save 开启。实机串口已验证寄存器读回、拿起唤醒、BLE 保持连接和 Joystick2 正常识别。

完整的功能、按键、依赖和烧录说明见仓库根目录的 [README](../README.md)。后续开发计划见 [TODO](TODO.md)。
