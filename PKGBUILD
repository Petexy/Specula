# Maintainer: Piotr 'Linexy' Lewandowski <piotr.petexiness@gmail.com>

pkgname=specula-git
pkgver=0
pkgrel=1
pkgdesc="Wire-free Android screen mirroring and control for the Linux desktop."
arch=('x86_64' 'aarch64')
url="https://github.com/Petexy/specula"
license=('GPL-3.0-or-later')

depends=(
  'gtk4'          # UI toolkit (>= 4.10)
  'libadwaita'    # GNOME platform library (>= 1.4)
  'ffmpeg'
  'libpulse'      # libpulse-simple, audio playback (PipeWire's shim works too)
  'glib2'
  'avahi'
  'android-tools'
  'scrcpy'
)

makedepends=(
  'meson'
  'ninja'
  'pkgconf'
  'git'
)

optdepends=(
  'desktop-file-utils: desktop-file-validate test + desktop database refresh'
  'appstream: metainfo validation test'
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
