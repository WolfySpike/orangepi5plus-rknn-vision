# Orange Pi 5 Plus AI Vision Engine

这是一个面向 Orange Pi 5 Plus / Rockchip NPU 的 C++ 视觉推理项目。项目从 HDMI 输入采集图像，使用 OpenCV 和 RKNN 模型进行实时检测，并提供 PID 控制、目标跟踪、UDP 通信以及 Flask Web 控制界面。

> 免责声明：本项目仅用于经过授权的设备、实验室研究和计算机视觉学习。请遵守当地法律、软件服务条款以及硬件厂商许可，不要将它用于作弊、绕过安全机制或影响他人的服务。

## 主要功能

- C++17 推理引擎，支持 RKNN NPU 多核心运行
- HDMI RX / V4L2 图像采集
- OpenCV 图像处理、目标检测结果跟踪和 PID 控制
- Web UI：运行时配置、模型选择、参数调整、日志和视频流控制
- HDMI 视频流测试工具：GStreamer、JPEG UDP、H.264/MPEG-TS 等
- systemd 服务安装脚本

## 硬件和软件要求

建议在 Orange Pi 5 Plus 或兼容 Rockchip 平台上运行：

- Linux、CMake >= 3.10、支持 C++17 的 GCC
- OpenCV（含开发文件）
- Rockchip RKNN Runtime 和与目标平台匹配的 RKNN 头文件
- HDMI RX / V4L2 设备
- Python 3、Flask（运行 Web UI）
- GStreamer 及对应的 Rockchip MPP 插件（使用视频流脚本时）
- 可选：支持的 KMBox/输入输出设备

RKNN SDK、模型文件、`librknnrt.so` 和厂商私有头文件不包含在本仓库中。请从合法、可信的来源获取，并放置到本地构建环境中。

## 编译

```bash
mkdir -p build
cd build
cmake ..
make -j$(nproc)
```

生成的程序包括：

- `aimbot_main`：主推理引擎
- `hdmirx_jpeg_udp`：C++ HDMI JPEG UDP 测试程序
- `hdmirx_v4l2_jpeg_udp`：V4L2 HDMI JPEG UDP 测试程序

主程序支持以下参数：

```text
./build/aimbot_main --model <model.rknn> --classes <数量> --family <8|11> --npu-cores <1|2|3>
```

## Web UI

在项目根目录运行：

```bash
python3 -m pip install Flask
python3 webui.py
```

然后访问：`http://<OrangePi地址>:5000`

生产环境可使用：

```bash
sudo bash scripts/install-webui-service.sh
sudo systemctl status aimbot-webui --no-pager
```

Web UI 会通过 UDP `9999` 向 C++ 引擎发送配置，并从 UDP `9998` 接收遥测数据。请根据实际网络环境配置防火墙和地址。

## KMBox 配置

连接参数通过环境变量提供，不要把真实密钥写入源码：

```bash
export KMBOX_HOST=192.168.2.98
export KMBOX_PORT=26947
export KMBOX_KEY=replace-with-your-key
./build/aimbot_main --model /path/to/model.rknn
```

如果未设置环境变量，程序会使用源码中的安全占位默认值；正式使用前请显式设置正确参数。

## HDMI 视频流脚本

脚本需要在 Linux/Orange Pi 上运行：

```bash
HOST=<电脑IP> PORT=9999 bash scripts/hdmirx_net_stream.sh diagnose
HOST=<电脑IP> PORT=9999 bash scripts/hdmirx_net_stream.sh v4l2-jpeg-udp 416 416 60 70
HOST=<电脑IP> PORT=9999 bash scripts/hdmirx_net_stream.sh h264 1280 720 60 20000000
```

可执行 `bash scripts/hdmirx_net_stream.sh` 查看完整命令列表和接收端示例。

## 推送到 GitHub

先在 GitHub 新建一个空仓库，建议不要自动添加 README、`.gitignore` 或 License，然后在本地项目目录执行：

```bash
git init
git add .
git status
git commit -m "Initial commit"
git branch -M main
git remote add origin https://github.com/<你的用户名>/<仓库名>.git
git push -u origin main
```

以后更新：

```bash
git add .
git commit -m "Describe your change"
git push
```

推送前务必确认 `git status` 和 `git diff --cached`，确保没有模型、私有 SDK 文件、日志、真实密钥或个人配置。若这些文件之前已经被 `git add`，仅修改 `.gitignore` 不会取消暂存，需要执行 `git restore --staged <文件>`；若已经推送过真实密钥，应立即在对应设备/服务端轮换密钥。

## 目录结构

```text
src/              C++ 主程序和 HDMI 采集程序
include/          项目头文件（厂商 SDK 文件需本地提供）
configs/          检测配置模板
convert/          ONNX/RKNN 转换及检查脚本
scripts/          systemd、HDMI 流和诊断脚本
templates/        Web 页面模板
static/           Web UI 静态资源
webui_backend/    Flask 后端和运行时配置逻辑
```
