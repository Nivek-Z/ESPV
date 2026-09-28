# ESPV：ESP32-S3-CAM 轻量视频固件

适用于 GOOUUU ESP32-S3-CAM（OV2640、16 MB Flash、8 MB PSRAM）。固件只提供 Wi-Fi/摄像头配置页和 MJPEG 视频流。摄像头直接输出 JPEG，ESP32 不做软件压缩、缩放或 H.264 编码。

## 地址与配置

- 板子重启后自动连接已保存的路由器 Wi-Fi。用串口日志或路由器客户端列表取得板子的局域网 IP。配置页：`http://CAMERA_IP/`；视频源：`http://CAMERA_IP:81/stream`。地址可能随 DHCP 变化，可在路由器中为板子设置地址保留。
- 配置页可以修改 2.4 GHz Wi-Fi、分辨率（QVGA/VGA/SVGA）、JPEG 质量、镜像和翻转。默认 VGA、质量 14。状态接口为 `/api/status`，包含连接状态、RSSI、频道和实际发送帧率。
- 新刷机设备或 Wi-Fi 连接失败约 15 秒后会开启 `ESPV-XXXXXX` 配置热点。**出厂默认热点及管理密码是 `ESPVsetup123`**。连接热点后打开 `http://192.168.4.1/`，保存设置时输入这个密码。首次配置后可在页面中修改；已保存过密码的旧设备会继续使用原密码。密码也会打印到 115200 波特率串口，并保存在 NVS 中。
- 如果板子已连上路由器，但局域网无法打开配置页，在启动完成后长按 **BOOT** 约 3 秒，板子会断开路由器并切换到独立配置热点。热点模式的配置页是 `http://192.168.4.1/`，视频源是 `http://192.168.4.1:81/stream`。热点持续到下次重启，重启后自动连接保存的路由器 Wi-Fi。
- 视频流只允许一个上游连接。接入 Frigate/go2rtc 后，其他设备应从 Frigate 看画面；配置页预览主要用于初次设置。
- 公开的出厂密码只用于初次接入，建议在配置页改成自己的管理 / 热点密码；私人路由器 Wi-Fi 密码不会写入源码或本仓库。

## Frigate 接入

让 Frigate 主机上的 go2rtc/FFmpeg 读取 MJPEG，一次转换为 H.264，再供检测和录像使用。ESP32-S3/OV2640 本身不编码 H.264。将下面的 `CAMERA_IP` 替换为设备实际局域网地址：

```yaml
go2rtc:
  streams:
    espv_cam: "ffmpeg:http://CAMERA_IP:81/stream#video=h264"

cameras:
  espv_cam:
    ffmpeg:
      inputs:
        - path: rtsp://127.0.0.1:8554/espv_cam
          input_args: preset-rtsp-restream
          roles:
            - detect
            - record
    detect:
      width: 800
      height: 600
      fps: 5
```

示例按当前 SVGA 编写；选择其他分辨率时请同步调整检测宽高。转换 H.264 会消耗 Frigate 主机的计算资源。视频 HTTP 地址目前不要求密码，只应在可信任局域网内使用。参见 [Frigate 官方 MJPEG 接入说明](https://docs.frigate.video/configuration/camera_specific/#mjpeg-cameras)。

## 已完成的实测

本板识别 OV2640 和 8 MB PSRAM。此前在独立热点测得 **251 个完整 JPEG 帧 / 10 秒，约 25 FPS**（VGA、质量 14）。

深度排查发现：20 MHz 摄像头外部时钟启动后，STA 网络失联；释放驱动但保留该时钟时仍失联；显式关闭时钟后恢复约 1.09 MB/s。软件将外部时钟降到 8 MHz 后恢复网络。进一步逐档测试后，正式固件使用约 **9.41 MHz XCLK** 和 OV2640 内部时钟倍频。当前保存的 **SVGA、质量 16** 设置下，重启后在局域网连续 120 秒收到 **2824 个完整 JPEG 帧（23.53 FPS，0 坏帧）**。视频发送期间配置页 HTTP 200、0.06 秒返回，30 次 ping 全通，平均 9 ms。原因和完整对照数据见 [NETWORK-DIAGNOSIS.md](NETWORK-DIAGNOSIS.md)。

MJPEG 地址已验证；Frigate/go2rtc 端到端接入尚未实测。

## 卡顿现象与原因判断

**实测现象：**板子直连热点时视频流畅；连接路由器的 STA 模式下，原来的 20 MHz 摄像头外部时钟一启动，网络请求和视频就会超时。摄像头不初始化时，同一局域网可传约 1.1 MB/s。调用 `esp_camera_deinit()` 后网络仍未恢复；再显式停止 GPIO15 的时钟输出，吞吐立即恢复到约 1.09 MB/s。8 MHz 可稳定传视频，逐档测试到约 9.41 MHz 时连续 120 秒保持 23.53 FPS；10 MHz 及更高的测试档位出现超时。路由器信道、频宽和 Wi-Fi 缓冲调整均没有提供同样的改善。各档原始结果见 [NETWORK-DIAGNOSIS.md](NETWORK-DIAGNOSIS.md)。

**原因推测：**最可能是持续输出的摄像头 XCLK 在这块板子上影响了 2.4 GHz Wi-Fi；时钟信号的高次谐波串入射频部分，或经供电线路引入噪声，都可能造成这种频率敏感现象。摄像头外设与 Wi-Fi 底层驱动的时钟交互也尚未完全排除。现有测试证明了触发条件和有效的软件规避方法，**没有测出具体的电磁或供电耦合路径，也不能据此断定板子硬件损坏**。乐鑫仓库中有[外部时钟频率影响 Wi-Fi 的相似报告](https://github.com/espressif/arduino-esp32/issues/5834)，仅作旁证。

## 编译与刷入

当前源码和 `.pio` 编译产物对应已刷入的约 9.41 MHz 正式固件。

工程使用 ESP-IDF 5.3.2 和锁定版本的 `espressif/esp32-camera` 2.1.7。可用 ESP-IDF 5.3 及以上版本执行 `idf.py build`、`idf.py -p COM8 flash monitor`；本机也能用 PlatformIO：

```powershell
$env:PLATFORMIO_CORE_DIR = Join-Path (Get-Location) '.pio-core'
pio run -j 2
pio run -t upload --upload-port COM8
pio device monitor -p COM8 -b 115200
```

刷入前确认实际串口号。本次刷入前已将原固件备份到工作目录中的 `original-flash-backup.bin`；文件被 Git 忽略。修改 `main/index.html` 后，先运行 `python tools/embed_html.py` 更新内嵌页面头文件。

性能设置：OV2640 原生 JPEG、约 9.41 MHz XCLK 和传感器内部倍频、双 PSRAM 帧缓冲、仅抓最新帧、PSRAM DMA、独立视频 HTTP 服务、Wi-Fi 省电关闭、2 秒发送超时。低光照、JPEG 大小和无线链路都会影响实测帧率。
