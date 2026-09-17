# The video writer

`snaggletooth::video::AviRecording` records the machine's pictures as an uncompressed AVI. It is
the video twin of the [WAV writer](../spc/wav_writer.h): this project writes its own container
rather than linking an encoder, so nothing but the standard library stands between a frame and the
file.

```cpp
#include "video/avi_writer.h"

AviRecording recording(path, 256u, 224u, FrameRate{.rate = 236250000u, .scale = 3931026u});
if (!recording.open()) { /* the file could not be opened */ }
recording.add(picture);   // a VideoFrame, as the machine hands it over
recording.finish();       // the index and the sizes only the whole recording knows
```

A recording is written a picture at a time — the headers as it opens, each picture as it arrives,
and the index when it closes — so a minute of video costs one picture of memory rather than six
hundred megabytes. A recording nobody finishes is finished when it goes away.

**The frame rate is a ratio**, because the console's is not a whole number of frames a second: the
values above are the NTSC master clock over the master cycles a frame takes, which the file stores
as the ratio it is.

**A picture is stored exactly as the machine drove it** — blue, green and red a byte each, rows
from the bottom up, each row run out to a four-byte boundary. That is what makes a recording an
oracle to set beside a capture from the console itself rather than an approximation of one. The
file plays in VLC and QuickTime and converts with ffmpeg.

**A recording takes the largest shape its run produced.** A run can change shape — a frame drawn in
half-pixels is 512 wide, the taller picture 239 lines — so every picture is written at its own shape
as it arrives, and a recording that saw more than one is laid out again as it finishes: at the widest
and tallest it saw, each smaller picture centred in black, the odd spare column on the right and the
odd spare line below. It is written beside the file and takes its name when complete, one picture in
memory at a time, and a recording of one shape is left exactly as it was written.

## Why not a real encoder

A compressed stream would mean libx264 or libav inside an MIT-licensed product aimed at rights
holders. That is a licence question before it is a size question, and the answer is no.
