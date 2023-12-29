# droidcam2v4l2

Daemon that bridges Android cameras to V4L2, using droidmedia and
v4l2loopback. In early development.

## Build dependencies

- libdroidmedia-dev
- libyuv-dev (if rotation support is enabled)

## Runtime dependencies

- v4l2loopback (kernel driver)
- libyuv0

## Build

	gcc -o droidcam2v4l2 src/droidcam2v4l2.c -ldroidmedia -lyuv

If you can't (or don't want to) use libyuv, you can set `SUPPORT_ROTATION` to 0.
