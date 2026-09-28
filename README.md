# ESPV：ESP32-S3-CAM 轻量视频固件

适用于 GOOUUU ESP32-S3-CAM（OV2640、16 MB Flash、8 MB PSRAM）。固件只提供 Wi-Fi/摄像头配置页和 MJPEG 视频流。摄像头直接输出 JPEG，ESP32 不做软件压缩、缩放或 H.264 编码。

## 地址与配置

- 板子重启后自动连接已保存的路由器 Wi-Fi。用串口日志或路由器客户端列表取得板子的局域网 IP。配置页：`http://CAMERA_IP/`；视频源：`http://CAMERA_IP:81/stream`。地址可能随 DHCP 变化，可在路由器中为板子设置地址保留。
- 配置页可以修改 2.4 GHz Wi-Fi、分辨率（QVGA/VGA/SVGA/XGA/HD/SXGA/UXGA）、JPEG 质量、镜像和翻转。默认 VGA、质量 14。OV2640 的最高可选分辨率为 UXGA 1600×1200。状态接口为 `/api/status`，包含连接状态、RSSI、频道和实际发送帧率。
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
      width: 640
      height: 480
      fps: 5
```

示例按出厂默认 VGA 编写；选择 SVGA 时请将检测宽高改为 800×600，选择 UXGA 时改为 1600×1200，其他分辨率同理。转换 H.264 会消耗 Frigate 主机的计算资源。视频 HTTP 地址目前不要求密码，只应在可信任局域网内使用。参见 [Frigate 官方 MJPEG 接入说明](https://docs.frigate.video/configuration/camera_specific/#mjpeg-cameras)。

## 已完成的实测

本板识别 OV2640 和 8 MB PSRAM。此前在独立热点测得 **251 个完整 JPEG 帧 / 10 秒，约 25 FPS**（VGA、质量 14）。

最初使用默认 GPIO15 输出驱动强度时，20 MHz 摄像头外部时钟会令 STA 网络失联；9.41 MHz 加传感器内部倍频则能稳定传送 SVGA 23.53 FPS。进一步测试发现，**降低 GPIO15 的 XCLK 输出驱动强度后，12 MHz 外部时钟加 OV2640 内部倍频在本板也能稳定工作**。SVGA、JPEG 质量 12 的局域网连续 120 秒实测为 **3639 个完整 JPEG 帧（30.32 FPS，0 坏帧）**；同时 30 次 ping 全通，配置接口约 0.09 秒响应。UXGA、质量 12 连续 120 秒收到 **1665 帧（13.87 FPS，0 坏帧）**，逐帧确认图像为 1600×1200。详细对照见 [NETWORK-DIAGNOSIS.md](NETWORK-DIAGNOSIS.md)。

MJPEG 地址已验证；Frigate/go2rtc 端到端接入尚未实测。

### 高分辨率实测

下表保留此前约 9.41 MHz XCLK、JPEG 质量 16 下的逐档测试，便于比较。每档重启后接收约 12 秒 MJPEG，逐帧检查 JPEG 首尾和图像内记录的真实宽高。随后在 12 MHz、质量 12 下单独测得 UXGA 13.87 FPS，测试后恢复 SVGA／质量 12。

| 设置 | 实际图像尺寸 | 完整帧 / 时间 | 帧率 | 坏帧 |
|---|---:|---:|---:|---:|
| XGA | 1024×768 | 141 / 12.00 秒 | 11.75 FPS | 0 |
| HD | 1280×720 | 141 / 12.03 秒 | 11.72 FPS | 0 |
| SXGA | 1280×1024 | 141 / 12.00 秒 | 11.75 FPS | 0 |
| UXGA | 1600×1200 | 140 / 12.02 秒 | 11.65 FPS | 0 |

这些是本板在当时场景和网络下的实测值，不同 XCLK 和 JPEG 质量的结果不能直接归因于单个设置。场景复杂度、光线和无线链路也会改变帧率。可用 `python tools/benchmark_resolutions.py --host CAMERA_IP --port COM8 --seconds 12` 复测；该脚本从串口读取设备当前管理密码且不输出密码，依次测试并恢复原分辨率。没有串口时可省略 `--port`，手动输入密码。

### JPEG 质量与数据量

配置页的 JPEG 数值越小，压缩越轻；驱动允许 0–63，本页面为稳定性将范围限制在 8–30。下面是在 SVGA、同一网络下每档约 10 秒的实测，数值为完整 JPEG 帧的平均大小和每秒 JPEG 数据量，不包含 HTTP/TCP/Wi-Fi 包头，也不代表 CPU 使用率。

| JPEG 数值 | 平均每帧 | JPEG 数据量 | 完整帧率 |
|---:|---:|---:|---:|
| 8 | 26.1 KB | 0.608 MB/s | 23.26 FPS |
| 12 | 20.9 KB | 0.486 MB/s | 23.30 FPS |
| 16 | 18.3 KB | 0.426 MB/s | 23.26 FPS |
| 20 | 16.7 KB | 0.390 MB/s | 23.40 FPS |
| 30 | 14.6 KB | 0.321 MB/s | 21.97 FPS |

8 相比 12 每帧约大 25%，相比 30 约大 79%。每档场景和无线状态未作严格控制，30 档的单次帧率偏低不能解释为提高压缩必然降低帧率。正常尺寸预览会缩小细节，8 与 12 的视觉差异可能很难察觉；更低的数值不会增加分辨率。可用 `python tools/benchmark_quality.py --host CAMERA_IP --port COM8 --seconds 10` 复测，脚本结束会恢复开始时的质量设置。

### UDP 传输探索

为了检查 HTTP/TCP 是否限制帧率，曾临时刷入探针固件，对同一块板子分别测试只从摄像头取帧、ESP-IDF/lwIP UDP 发送 JPEG 分片、现有 HTTP MJPEG。接收端按帧编号和偏移重组 JPEG，统计完整帧；每项约 12 秒，JPEG 质量 12。探针并未手写 IP/UDP 协议栈，也未加入正式固件。

| 分辨率 | 只取帧 | UDP 完整接收 | HTTP 完整接收 |
|---|---:|---:|---:|
| SVGA 800×600 | 284 帧，23.66 FPS | 283 帧，23.46 FPS | 282 帧，23.50 FPS |
| UXGA 1600×1200 | 143 帧，11.84 FPS | 59 帧，4.89 FPS | 139 帧，11.58 FPS |

SVGA 下 HTTP 已非常接近摄像头取帧上限。UXGA 下，UDP 探针记录到 143 帧中有 84 帧发送不完整；这种无重传的分片流只要丢一个包就损失整张 JPEG。调整发送重试后依然如此。自定义 UDP 还需常驻接收及转发程序才能供 Frigate 使用；若使用标准 RTP/JPEG，则需实现 [RFC 2435](https://www.rfc-editor.org/info/rfc2435/) 的封包和会话描述，再由兼容的接收器接入。[go2rtc 支持格式](https://github.com/alexxit/go2rtc/blob/master/pkg/README.md)不包括直接读取本探针的自定义 UDP 包。当前没有证据表明这会提高本板的完整视频帧率，因此配置页未增加 UDP 切换。这个结果不代表所有网络条件下 UDP 的延迟都相同。

## 卡顿现象与原因判断

**实测现象：**板子直连热点时视频流畅；连接路由器的 STA 模式下，GPIO15 保持原输出驱动强度时，20 MHz XCLK 会使网络请求和视频超时。摄像头不初始化时，同一局域网可传约 1.1 MB/s；调用 `esp_camera_deinit()` 后网络仍未恢复，显式停止 XCLK 后恢复约 1.09 MB/s。原驱动强度下，9.41 MHz 可持续传 23.53 FPS，而 10 MHz 及以上测试档位曾超时。将 GPIO15 输出驱动强度降到最低档后，11.43 MHz、12 MHz 均可持续工作，12 MHz 达到上述 SVGA 30.32 FPS。20 MHz 保持原驱动强度的对照再次出现超时；22.86 MHz 低驱动强度短测也能传约 28 FPS，但低于 12 MHz 方案。路由器设置未在这轮测试中修改。各档结果见 [NETWORK-DIAGNOSIS.md](NETWORK-DIAGNOSIS.md)。

**原因推测：**XCLK 引脚输出驱动强度能改变同一块板子、同一 STA 网络下的结果，说明单看时钟频率不足以解释故障。较弱驱动通常意味着较小的边沿电流，可能减少经走线、供电或射频部分耦合的干扰；也可能与该板的时钟信号完整性有关。**尚未用示波器或射频仪器确定具体耦合路径，不能断言板子损坏。**[OV2640 数据手册](https://files.waveshare.com/wiki/common/OV2640DS_en.pdf)给出的 XVCLK 输入范围为 6–24 MHz，最高图像传输率为 SVGA 30 FPS、UXGA 15 FPS。当前 12 MHz 外部时钟配内部倍频已接近这两档的标称上限，继续提高外部时钟或过驱传感器不保证增加完整接收帧率。

## 编译与刷入

当前源码使用 12 MHz XCLK、OV2640 内部倍频及 GPIO15 最低输出驱动强度。

工程使用 ESP-IDF 5.3.2 和锁定版本的 `espressif/esp32-camera` 2.1.7。可用 ESP-IDF 5.3 及以上版本执行 `idf.py build`、`idf.py -p COM8 flash monitor`；本机也能用 PlatformIO：

```powershell
$env:PLATFORMIO_CORE_DIR = Join-Path (Get-Location) '.pio-core'
pio run -j 2
pio run -t upload --upload-port COM8
pio device monitor -p COM8 -b 115200
```

刷入前确认实际串口号。本次刷入前已将原固件备份到工作目录中的 `original-flash-backup.bin`；文件被 Git 忽略。修改 `main/index.html` 后，先运行 `python tools/embed_html.py` 更新内嵌页面头文件。

性能设置：OV2640 原生 JPEG、12 MHz XCLK 和传感器内部倍频、GPIO15 最低输出驱动强度、双 PSRAM 帧缓冲、仅抓最新帧、PSRAM DMA、独立视频 HTTP 服务、Wi-Fi 省电关闭、2 秒发送超时。低光照、JPEG 大小和无线链路都会影响实测帧率。
