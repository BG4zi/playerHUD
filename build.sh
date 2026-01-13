#!/usr/bin/env sh

set -xe
cc main.c -o playerhud $(pkg-config --cflags --libs gtk+-3.0 gio-2.0 gdk-pixbuf-2.0)
./playerhud
