# 开始菜单网格 · StartMenuGrid

调整 Windows 11 原生开始菜单中固定应用的列数和每页行数。「全部」列表的滚动区域随菜单高度调整。菜单保留搜索框、推荐项目、账户和电源菜单，以及任务栏上方的留白。

## 截图

**横向 8 列**

![原生开始菜单的搜索框和一排八个固定应用](docs/screenshots/start-menu.png)

**设置面板**

![设置面板中的横向列数、纵向行数、应用网格和恢复默认](docs/screenshots/settings.jpg)

## 下载和使用

1. 下载 ZIP 并完整解压，运行 `StartMenuGrid.exe`。[GitHub Releases](https://github.com/Zc4777/StartMenuGrid/releases/latest)
2. 选择横向列数和纵向行数，点击「应用网格」。首次应用需要管理员权限，程序会为当前账户登记登录启动项。
3. 之后每次登录 Windows 都会自动生效。控制面板可以关闭，运行时无需 Windhawk。

从旧版独立版更新时，打开新版并点击「应用网格」。更新期间，当前会话的开始菜单会短暂重新加载。点击「恢复默认」可还原系统布局并取消登录时加载。

## 兼容性

已验证 Windows 11 23H2 x64（22631.7517）。应用时会检查 `StartMenu.dll` 的版本指纹。Windows 更新若替换了该组件，需要先验证兼容性才能应用。行列上限也受当前显示器的可用空间和缩放比例限制。

## 构建

在 Windows 上准备带 C++/WinRT 头文件的 LLVM-MinGW x64。构建脚本同时使用系统自带的 .NET Framework C# 编译器。

```powershell
.\source\build.ps1 -Clang 'C:\path\to\clang++.exe' -Output .\out
```

本项目以 GPL-3.0-only 发布。原生组件的 UI 线程接入参考 Michael Maltsev 的 Start Menu Size。[许可证](LICENSE) · [上游源码](https://github.com/ramensoftware/windhawk-mods/blob/main/mods/start-menu-size.wh.cpp)
