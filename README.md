# Modified version of the `nvunixfd` GStreamer plugin for JetPack 5

See https://docs.nvidia.com/jetson/archives/r36.4.4/DeveloperGuide/SD/Multimedia/AcceleratedGstreamer.html#ipc-plugins for what the plugin does.

## Building

```
make -j$(nproc --all)
```

## Usage
Copy to /usr/lib/aarch64-linux-gnu/gstreamer-1.0/

Then run your GStreamer application that uses `nvunixfdsink` or `nvunixfdsrc`


Example pipelines:

```
gst-launch-1.0 -v videotestsrc is-live=true ! video/x-raw,width=640,height=480,framerate=30/1,format=NV12 ! nvvidconv ! 'video/x-raw(memory:NVMM),format=NV12' ! nvunixfdsink socket-path=/tmp/nvfd.sock
gst-launch-1.0 -v nvunixfdsrc socket-path=/tmp/nvfd.sock ! 'video/x-raw(memory:NVMM),format=NV12' ! nvv4l2h264enc ! rtph264pay config-interval=1 ! udpsink host=127.0.0.1 port=5000
gst-launch-1.0 -v nvunixfdsrc socket-path=/tmp/nvfd.sock ! 'video/x-raw(memory:NVMM),format=NV12' ! nvv4l2h264enc ! rtph264pay config-interval=1 ! udpsink host=127.0.0.1 port=8000
```
