# Snapshot 系统安装工具

绿色单文件 Win32 GUI 程序，用于 Windows PE 环境下安装 SNA 格式系统镜像。

## 功能

- ✅ 自动列出所有可用分区（盘符、大小、可用空间、文件系统）
- ✅ 浏览选择 .sna 镜像文件
- ✅ 下拉列表选择目标分区
- ✅ 支持还原密码输入
- ✅ 调用 Snapshot64.exe 静默还原
- ✅ 可选修复 UEFI 引导（bcdboot）
- ✅ 漂亮的程序图标

## 使用方法

1. 将 `SnapshotInstaller.exe` 和 `Snapshot64.exe` 放在同目录下
2. 在 Windows PE 中运行 `SnapshotInstaller.exe`
3. 选择 .sna 镜像和目标分区
4. 可选：输入还原密码、勾选修复 UEFI 引导
5. 点击「开始安装系统」

## 编译

本项目使用 GitHub Actions 自动编译，提交代码后自动构建 exe。

### 手动编译

```bash
windres snapshot.rc -O coff -o snapshot.res
gcc -mwindows -O2 -s -o SnapshotInstaller.exe main.c snapshot.res -lcomctl32 -lshlwapi -lcomdlg32
```

## 技术栈

- C 语言 + Win32 API
- MinGW-w64 交叉编译
- GitHub Actions CI/CD
