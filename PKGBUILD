# Maintainer: Piotr 'Linexy' Lewandowski <piotr.petexiness@gmail.com>

pkgname=specula-git
pkgver=1
pkgrel=1
pkgdesc="Wire-free Android screen mirroring and control for the Linux desktop."
arch=('x86_64' 'aarch64')
url="https://github.com/Petexy/specula"
license=('GPL-3.0-or-later')

depends=(
  'gtk4>=4.10'
  'libadwaita>=1.5'
  'ffmpeg'
  'libpulse'
  'glib2'
  'avahi'
  'android-tools'
  'scrcpy'
  'hicolor-icon-theme'
  'desktop-file-utils'
)

makedepends=(
  'meson'
  'ninja'
  'pkgconf'
  'git'
)

checkdepends=(
  'desktop-file-utils'
  'appstream'
)

provides=('specula')
conflicts=('specula')
install=specula.install
source=("$pkgname::git+$url.git")
sha256sums=('SKIP')

pkgver() {
  cd "$srcdir/$pkgname"
  git describe --long --tags | sed 's/^v//;s/\([^-]*-g\)/r\1/;s/-/./g'
}

build() {
  arch-meson "$pkgname" build
  meson compile -C build
}

check() {
  meson test -C build --print-errorlogs
}

package() {
  meson install -C build --destdir "$pkgdir"
}
