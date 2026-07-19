# Maintainer: Populus Yang <populus@example.com>
pkgname=shiguang
pkgver=0.1.0
pkgrel=1
pkgdesc="拾光 - 每日一图桌面客户端（第三方 Linux 移植）"
arch=('x86_64')
url="https://gallery.timeline.ink/"
license=('MIT')
depends=('gtk4' 'curl' 'json-glib')
makedepends=('cmake' 'gcc' 'pkgconf' 'glib2')

build() {
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build build
}

package() {
    # 安装二进制
    install -Dm755 build/shiguang "$pkgdir/usr/bin/shiguang"

    # 安装桌面入口
    install -Dm644 io.github.populusyang.shiguang.desktop \
        "$pkgdir/usr/share/applications/io.github.populusyang.shiguang.desktop"

    # 安装图标
    install -Dm644 icon.png \
        "$pkgdir/usr/share/icons/hicolor/256x256/apps/io.github.populusyang.shiguang.png"
}
