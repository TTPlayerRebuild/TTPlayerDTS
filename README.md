# ttp_dts

TTPlayer 原版 AddIn ABI 的独立 x86 DTS 解码插件。面向原版播放器及重建版；同一 DLL 使用 VC-LTL / YY-Thunks 兼容 XP SP3、Win7 和现代 Windows，CPU 要求 SSE2。

## 功能

- 原始 DTS（`.dts`、`.dtshd`）、DTS-WAV（`.wav`、`.wave`、`.dtswav`）。支持 DTS core 的 16/14 位、大/小端封装。
- 静态裁剪 FFmpeg 9.0.2 DCA 解码器，不依赖额外 FFmpeg DLL。
- 按宿主 Open 标志返回 float64 或 24-bit PCM，保留声道布局；输出可进入宿主均衡器、回放增益及设备转换链路。
- IStream 读取、容量对齐、取消、EOF、准确定位；可选 Probe / SourceInfo 接口。DTS-WAV 的实际容器仍是 WAVE，不建议改名为 `.dts`。
- 标签与可识别的附图只读；不声明标签保存或编码能力。

原版宿主没有新增的 Probe 协议，其内置 WAV Reader 与插件的优先级不能由本 DLL 改写。原版的 DTS-WAV 文件选择需实际检查；重建版已同时修正路由和缺失插件时的杂音保护。DCA 后端包括 DTS-HD 分支，但本次生成样本只验收 core；不宣称 DTS-HD MA/HRA/Express 已完成实测。

## 构建与发行

安装现代 Visual Studio C++ x86 工具链、CMake、Python 3.12+、Git for Windows，运行：

```powershell
./build.ps1 -Package -SourcePackage -PackageVersion 2026.10.23
```

依赖固定版本及 SHA-256，首次构建下载源代码。结果在 `build/Release/`。运行包仅含 `AddIn/ttp_dts.dll` 和 `SHA256SUMS.txt`；对应源码附件包含静态链接后端的完整源代码及重建脚本。许可证在仓库和源码附件中，不嵌入 DLL。

Action 独立构建；勾选 **Release a Version** 时使用北京时间日期版本，产生对应源码附件。测试位于工作区 `rebuild/tests/wav_feedback_20261023/`，不上传、不在 Action 执行、不进发行包。尚未创建或发布远程仓库；集成到播放器在线 CodecAddIn 包前，应登记实际仓库及已发布资产。

详见 [实现及验证](docs/IMPLEMENTATION.md)、[依赖](docs/DEPENDENCIES.md)。
