# AX-VideoCapture
Video Capture Engine for Cinder

Check out into `{your_cinder_path}/blocks/` folder.

```
$ cd ${your_cinder_path}/blocks && git clone https://github.com/axjxwright/AX-VideoCapture
$ cd AX-VideoCapture/samples/SimpleCapture
$ mkdir build && cd build
$ cmake ..\proj\cmake
$ start SimpleCaptureApp.sln
```

Currently windows only, runs in both hardware accelerated mode where the frame texture is never pulled down to the CPU, for when you just want to draw the texture, but can also run in software mode which allows you to get a `ci::Surface` for when you need access to the pixel data on the CPU side.

The `SimpleCapture` sample shows basically all of the API features.

\- @axjxwright
