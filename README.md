# 拾光 — Linux 桌面客户端

Linux 第三方桌面客户端，仅是对 [拾光 Windows UWP 版本](https://gallery.timeline.ink/) 的拙劣模仿。

## 声明

**本程序与拾光原作者（南瓜多糖）没有任何关系。**

拾光品牌、图片内容、API 服务、官方客户端的一切版权归原作者所有。本项目仅为一个社区 Linux 移植，不包含任何原作者的代码、图片资源或品牌资产。

请访问 [https://gallery.timeline.ink/](https://gallery.timeline.ink/) 支持原作者。

## 功能

- 37 个图源，覆盖官方精选与三方壁纸源
- 图片预加载队列，前后各缓存 5 张，流畅切换
- 交叉淡入淡出过渡动画
- 收藏到本地 `~/Pictures/Shiguang/`
- 一键设为壁纸（支持 GNOME / KDE / XFCE）
- 一键下载原图
- 「一言」每日一句

## 安装依赖

### Arch Linux
```bash
sudo pacman -S gtk4 curl json-glib cmake gcc pkgconf
```

### Debian / Ubuntu
```bash
sudo apt install libgtk-4-dev libcurl4-openssl-dev libjson-glib-dev cmake gcc pkgconf
```

### Fedora
```bash
sudo dnf install gtk4-devel libcurl-devel json-glib-devel cmake gcc pkgconf
```

### openSUSE
```bash
sudo zypper install gtk4-devel libcurl-devel json-glib-devel cmake gcc pkgconf
```

## 编译

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/shiguang
```

## 安装

```bash
# 通用方式
sudo cmake --install build

# Arch Linux 也可用 makepkg
makepkg -si
```

## 许可

本项目客户端代码以 MIT 协议开源。

以下内容的版权不在此协议范围内，归原作者（南瓜多糖）所有：
- 拾光品牌名称
- 应用图标（icon.png，截取自拾光官网）
- 图片内容及 API 数据

若需重新分发，请替换 `icon.png` 为自有图标。

## 鸣谢

- [南瓜多糖](https://gallery.timeline.ink/) — 拾光原作者，提供图片与 API 服务
- [glitter](https://glitter.timeline.ink/) — 一言 API
