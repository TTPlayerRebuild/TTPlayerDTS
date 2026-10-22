# DTS 插件实现与验收

2026-10-23，按 `rebuild/docs/AUDIO_PLUGIN_DEVELOPMENT_GUIDE.md` 创建独立项目。

## ABI 和数据路径

- ordinal 1：`ttpGetSoundAddIn`；一个 ReaderCreator，保留原版 IUnknown、AddIn、Reader、Metadata、Thumbnail 虚表和 GUID。
- COM 身份统一；字符串和完整 WAVEFORMATEXTENSIBLE 用 CoTaskMemAlloc；异常转 HRESULT，不跨 DLL 传播。
- 可选 Probe 与 SourceInfo 使用 ttpcomm SDK ABI 1。DTS-WAV 的 `container=wave`、`codec=dts`、建议扩展名 `.wav`；原始流为 `container=dts`。
- 自定义 AVIO 只使用宿主 IStream，不按 Stat 路径重新打开文件。解码后通过 swresample 交错化，float64 或 PCM24 返回宿主。
- Read 检查容量和 block align；Stop 发出取消事件；正常 EOF 返回 S_FALSE；输入读取失败不会改写成正常 EOF。
- Seek 向前解码并丢弃，向后重新打开并重放到目标帧，保留解码器历史和滤波状态。代价是大文件后退定位较慢；尚未实现带预卷的稀疏索引优化。
- 标签和封面接口只读，不设置写入能力，不伪造 Commit 成功。

## 本地验证

私有记录：`rebuild/tests/wav_feedback_20261023/`。

- 自行编码的六声道 DTS core，原始流及 WAVE 包装；16/14 位、大/小端均通过。
- 131072 字节 JUNK、`.dtswav`、错误 `.dts` 后缀中的 WAVE 仍能路由到插件。
- 浮点和整数输出，容量小于一帧、4093 字节非整帧容量、缓冲哨兵、EOF 重读、Stop/Start、前后定位均通过。
- 对照独立 FFmpeg CLI，浮点最大误差约 `5.96e-8`；PCM24 量化误差不超过半个最低有效位。未声明不同编译配置逐位一致。
- XP 和 Win7 实际执行同一 DLL，六声道 DTS-WAV 解码结果与 Win11 SHA-256 完全一致。
- 原版 5.7.9 和重建版的隔离安装启动、插件加载、无错误对话框及正常退出 smoke 通过。此项不是原版宿主完整 PCM 输出逐样本对照，也不是实际多声道硬件验收。
- 长 DTS 样本在两个宿主中播放进度超过 3 秒；7 字节短读以及注入 STG_E_READFAULT 均按预期处理。
- 生成的普通 WAV 24 种组合和 4 组宿主输出测试通过；缺插件时拒绝 DTS-WAV，单一偶然同步头不误判成 DTS。

## 仍需验收

DTS-HD MA/HRA/Express 的真实样本、原版内置 WAV Reader 与新工厂的优先级、真实多声道声卡、Win10 独立系统。本次 Win11 用于覆盖现代系统测试，不称为实际运行过 Win10。
