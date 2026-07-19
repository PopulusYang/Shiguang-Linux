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
    cd "$startdir"
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build build
}

package() {
    cd "$startdir"

    install -Dm755 build/shiguang "$pkgdir/usr/bin/shiguang"

    install -Dm644 io.github.populusyang.shiguang.desktop \
        "$pkgdir/usr/share/applications/io.github.populusyang.shiguang.desktop"

    install -Dm644 icon.png \
        "$pkgdir/usr/share/icons/hicolor/256x256/apps/io.github.populusyang.shiguang.png"
}
