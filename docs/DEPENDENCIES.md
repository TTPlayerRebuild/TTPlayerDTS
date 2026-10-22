# 依赖和发行

2026-10-23。固定 FFmpeg **9.0.2**，不自动跟随 master：

- 来源：<https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz>
- SHA-256：`8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e`
- GNU make 4.4.1：`https://ftp.gnu.org/gnu/make/make-4.4.1.tar.gz`，SHA-256 `dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3`。
- VC-LTL 5.3.1、YY-Thunks 1.2.2，下载来源和哈希在 `cmake/legacy_windows.cmake`。

DTS profile 只启用 DCA 解码/解析、DTS/WAV/SPDIF 解复用及必要的 avutil、swresample。禁用外部依赖自动探测、程序、网络、视频滤镜、共享库、GPL/nonfree/version3 配置。构建检查 `config.h` 以及 XP/Win7 静态导入表。

仓库自有代码采用 MIT；静态 FFmpeg 后端遵循 LGPL-2.1-or-later，相关完整文本见 `LICENSES/`。生成器不修改 FFmpeg 源码。GNU make 是构建工具，不链接进插件。

运行压缩包只含 DLL 和校验文件。发布 Action 同时创建 `-source.zip`，包含插件源码、完整 FFmpeg 源码、make 源码包、固定版本及构建脚本，以便重新构建/替换后端。完整第三方源码仅保存在构建缓存和对应源码附件，不提交到仓库；许可证不嵌入 DLL。

运行时不需要安装 VC++ 2012、VC-LTL、YY-Thunks 或 FFmpeg；VC-LTL/YY-Thunks 是构建依赖，目标系统需有正常的系统 CRT 和 Windows 基础 DLL。CPU 基线为 SSE2。
