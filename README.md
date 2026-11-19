# xsync

轻量级、零内存泄漏的原生 C++17 / Qt6 Widgets 文件传输工具，专为替代容易内存膨胀变卡的 Xftp 而设计。

A lightweight, leak-free native C++17 / Qt6 Widgets alternative to Xftp supporting Local ↔ Remote and Remote ↔ Remote transfers with automatic `rsync → SFTP → SCP` protocol negotiation.

## 核心特性 (Key Features)

- **智能协议协商与自动降级 (`rsync → SFTP → SCP`)**
  - **本地 ↔ 远端**：自动探测两端能力，双方均有 `rsync` 时优先使用 `rsync`（支持断点续传 `--partial`、增量同步、压缩、Windows cwRsync/MSYS2/WSL 路径自动转换与密码 `SSH_ASKPASS`），否则无缝降级至内置 `libssh2` SFTP，若远端禁用了 SFTP 子系统则进一步降级至原生 SCP。
  - **远端 ↔ 远端 (Remote ↔ Remote)**：默认优先尝试**远端直连**（源服务器直接 `rsync` / `scp` 到目标服务器，走内网不绕本地），若网络不通或未配置互信免密，自动降级为**本地零落盘流式中转**（使用固定 256 KiB 内存缓冲区边读边写，不产生临时文件、传 50 GB 内存也保持平稳）。
- **彻底解决 Xftp 内存泄漏痛点**
  - 扁平 `std::vector<FileEntry>` 虚拟表格模型（无 `QStandardItem` 对象爆炸），离开大目录自动回收堆内存。
  - 静态 SVG 图标缓存（无 Windows Shell COM / GDI 句柄泄漏）。
  - 传输队列使用自定义 `QStyledItemDelegate` 绘制进度条（无行级 `QProgressBar` 控件泄漏），完成历史严格有界自动淘汰（默认上限 500 条），日志严格限制 1000 行。
  - 所有 `libssh2` 会话、通道、SFTP 句柄均为严格 RAII 管理；状态栏实时显示进程 RSS 内存并支持一键压缩缓存。
- **Xftp 风格高效交互**
  - **双栏多标签 + 多窗口传输**：左右任意一侧均可开多个标签（每个标签独立为本地或任意远端机器），`Ctrl+N` 随时开新独立窗口，共享机器配置与并发传输队列。
  - **注册远端机器 (Session Manager)**：分组管理、关键词搜索、一键导入 `~/.ssh/config`、支持密码（Windows DPAPI / Linux AES-256-GCM 加密落盘）、私钥（ED25519 / ECDSA / RSA）、SSH Agent、JumpHost 跳板机与实时协议探测测试。
  - **直接拖拽 (Drag & Drop)**：支持从 Windows 资源管理器直接拖文件/文件夹到远端窗口上传，支持左右栏/跨标签/跨窗口直接互拖传输。
  - **本地 `Ctrl+C` / `Ctrl+V`**：在 Windows 资源管理器中 `Ctrl+C` 复制文件/文件夹后，切到 `xsync` 任意远端窗口按 `Ctrl+V` 即可直接上传；也支持在 `xsync` 窗口之间 `Ctrl+C` / `Ctrl+X` / `Ctrl+V`（含远端到远端）。

## 常用快捷键 (Shortcuts)

| 快捷键 | 功能 |
|---|---|
| `Ctrl+O` | 打开会话管理器（注册/连接远端机器） |
| `Ctrl+N` | 打开新的独立传输窗口 |
| `Ctrl+T` | 在当前侧新建本地标签页 |
| `Ctrl+C` / `Ctrl+X` / `Ctrl+V` | 复制 / 剪切 / 粘贴文件（完全兼容 Windows 资源管理器） |
| `Ctrl+Enter` | 传输选中文件到对侧窗口 |
| `F5` | 刷新当前目录 |
| `F2` / `Del` / `Backspace` | 重命名 / 删除 / 返回上级目录 |
| `Ctrl+H` | 显示或隐藏点文件（隐藏文件） |

## Windows `.exe` 获取与本地构建

### 方式一：从 GitHub Actions / Releases 直接下载
推送至 GitHub 后，`.github/workflows/build-windows.yml` 会自动在 `windows-latest` 上使用 MSVC 2022 + Qt 6.8.3 + OpenSSL 3 + libssh2 编译并产出：
- `xsync-windows-x64-portable.zip`（解压即用，双击 `xsync.exe` 运行）
- `xsync-windows-x64-setup.exe`（Windows 安装包）

> **Windows 启用本地 `rsync` 提示**：如果系统 PATH 中已有 `rsync.exe`（如 Git Bash / MSYS2 / cwRsync），或将 `cwRsync` 放到 `xsync.exe` 同级的 `bin\rsync.exe`，或已安装 WSL 并装有 `rsync`，`xsync` 会自动识别并优先启用；否则自动无缝降级为高速内置 SFTP。

### 方式二：在 Windows 本地一键编译
前置条件：Visual Studio 2022 (C++ Desktop), CMake, Ninja, Qt 6.8 (MSVC2022 x64), OpenSSL 3 x64。
```powershell
.\scripts\build_windows.ps1 -QtDir "C:\Qt\6.8.3\msvc2022_64"
```

### Linux 本地编译与测试
```bash
./scripts/build_linux.sh
```
