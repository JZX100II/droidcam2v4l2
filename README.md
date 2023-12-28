# droidcam2v4l2

Daemon that bridges Android cameras to V4L2, using droidmedia and
v4l2loopback. In early development.

## Requirements

- droidmedia
- v4l2loopback
- libyuv (if rotation support is enabled)

## Build

    gcc -I/usr/include/libyuv/ -I../v4l2loopback/ -Wall -o droid2v4l2 droid2v4l2.c -ldroidmedia -lyuv

You will need to specify your include path for `v4l2loopback.h`. If you can't
(or don't want to) use libyuv, you can disable `SUPPORT_ROTATION`.
