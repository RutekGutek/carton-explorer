#!/bin/sh
set -e
cd "$(dirname "$0")"
PREFIX="${PREFIX:-$HOME/.local}"
[ -x build/carton ] || ./build.sh
install -Dm755 build/carton "$PREFIX/bin/carton"
install -Dm644 data/carton.desktop "$PREFIX/share/applications/carton.desktop"
install -Dm644 data/carton.svg "$PREFIX/share/icons/hicolor/scalable/apps/carton.svg"
echo "Zainstalowano w $PREFIX"
sudo apt install libqt6widgets6 libqt6dbus6
echo "Zainstalowano qt6"
