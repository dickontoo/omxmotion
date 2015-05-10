omxmotion
=========

(c) 2015 Dickon Hood <dickon@fluff.org>

omxmotion is a simple motion detection program, which uses the Pi's camera
module to capture images, and abuses the H.264 encoder to look for motion.
It's capable of 1080p25 on an original 256MB Model B in about 40% CPU
(non-overclocked).

It has support for either a flat-field of trigger thresholds, or you can
supply a 'heatmap'.  This is a PNG file of trigger thresholds -- see Usage.


Building
--------

'make' should do it.  If you have issues with ffmpeg / libav, download and
install the version I built some time ago at
<http://newsplodge.fluff.org/~dickon/ffmpeg.tar.bz2>.  Apologies; it's a bit
big.

bunzip2 < ffmpeg.tar.bz2 | tar xvf -; cd ffmpeg; make install

should do it.  It'll install into /usr/local, and you may need to uninstall
the packaged versions if you have linking errors.


Usage
-----

Trivially:

\# ```./omxmotion```

Usage: ./omxmotion [-b bitrate] [-d outputdir] [-r framerate ] [ etc ]

Where:
        -b bitrate      Target bitrate (Mb/s)

        -c url  Continuous streaming URL

        -d outputdir    Recordings directory

        -e command      Execute $command on state change

        -h              This help

        -m mapfile.png  Heatmap image

                OR:

        -s 0..255       Macroblock sensitivity

        -o outro        Frames to record after motion has ceased

        -r rate         Encoding framerate

        -t 0..100       Macroblocks over threshold to trigger (raw)

        -v              Verbose

        -z pattern      Dump motion vector images (debug)

        
```-b```, ```-d```, ```-h``` and ```-r``` should be obvious.

```-c``` is a URL to stream the H.264 to, continuously.  Try
udp://@224.0.0.40:5554 or similar; view in mplayer or vlc with the same URL.

```-e``` executes the nominated command when recording starts or stops.  It's
passed either 'start' or 'stop' in $1, with the filename of the newly-opened
output file in $2 on start.

```-t``` is the number of above-trigger-threshold blocks to trigger recording on, as a percentage.

```-z``` is best ignored for now.

```-s``` is the threshold above which motion is assumed for this macroblock.  This
isn't as flexible as the mapfile option -- it simply sets the internal
structure to the flat figure you specify -- and passing both ```-s``` and ```-m``` isn't
an error, but ```-m``` takes precidence.

```-m``` mapfile is the fun one.  In most uses of software like this, there are
regions of the frame which you aren't interested in.  Passing a mapfile
allows you to mark regions of the frame in which you are interested, and
regions which should be disregarded when detecting motion.

omxmotion expects an 8-bit non-alpha greyscale PNG.  To create one, install
ImageMagick (for the 'convert' utility) and an image editor (the GIMP works
well):

 * install the camera where you want it to live,

 * /opt/vc/bin/raspistill -w 1920 -h 1088 -o snapshot.jpg -awb auto

 * convert snapshot.jpg -colorspace gray -colors 256 -resize 120x68 thumb.png

 * load thumb.png into your favourite editor, and mask over the bits you
   want detection on.  Export it back as an 8-bit greyscale PNG.  You're
   after something that file(1) reports as:

   root@camera:/home/dickon/src/omxmotion# file heatmap.png 
   heatmap.png: PNG image data, 120 x 68, 8-bit grayscale, non-interlaced

When editing the heatmap, you may find it easier to display the source
snapshot alongside it.  When you've compressed the image to a small
thumbnail and turned it greyscale, it can be surprisingly difficult to see
what each pixel represents.  See the examples directory for what I mean.

The camera only supports vertical resolutions that are a multiple of 32
pixels.  1080 is not, so capture 1088.  Unfortunately, the macroblocks at
the bottom of the screen get very noisy when scaled back to 1080, so make
them white.

```-z``` is a debugging tool.  If you find it triggering more than you expect,
it's probably worth trying this:

 * mkdir vo
 
 * Start omxmotion as usual, with "-z 'vo/img%05d.png'"

 * Once some inappropriate motion has been detected, let it run for a few
   seconds.

 * ffmpeg -i 'vo/img%05d.png' vo.mp4

 * mplayer vo.mp4

You should now have a small video playing, showing the magnitude of the
motion vectors on each frame.  Where there is unexpected movement showing,
increase the threshold in your heatmap image.


Internals
---------

Internally, it looks a fair bit like my previous Pi video plaything, omxtx.
It's a bit of a hairball -- sorry about that -- but it should be fairly
understandable.  OpenMAX isn't my favourite API, but I'd rather learn and
use that, than some Broadcom-proprietary abstraction layer (I'm looking at
you, MMAL) as at least if I ever come across any other embedded media
devices I'll stand a chance of being able to do something with it.

ATM, it creates a ringbuffer of INMEMFRAMES (currently 64) AVPackets, and
keeps markers of where in that buffer the last two I-frames were.  When
motion is detected and debounced, frames from the earliest I-frame are
copied to the output file.  The encoder has been configured to avoid
B-frames, and only use one reference frame, which means this should be
sensible.


Bugs
----

The continuous streaming mode is a bit experimental at present, and has
some issues with large frames: packets are apparently dropped.  If anyone
has any idea what to do about that, please let me know.  I may write a
custom muxer.  Similarly, ffmpeg's rtp support is novel and interesting and
doesn't do much that's actually useful.  Stick to UDP for the time being.
The stream itself seems to be slightly corrupt, most likely due to SPS and
PPS packets not being handled correctly (they're inline at present to make
it work at all, and I suspect they're not being handled correctly by the TS
muxer).  mplayer will repeatedly whinge:

	V:   0.0   0/  0 ??% ??% ??,?% 0 0 
	[h264 @ 0x7f19c3daf900]no frame!
	V:   0.0   0/  0 ??% ??% ??,?% 0 0 
	[h264 @ 0x7f19c3daf900]no frame!
	V:   0.0   0/  0 ??% ??% ??,?% 0 0 
	Error while decoding frame!
	Error while decoding frame!

etc.

A word of warning: high(ish)-bitrate multicast streams can do Bad Things
(tm) to cheap wifi kit.  If you find your wifi network dropping out,
firewall the packets from it.

There are still no timings present in the streams.  This needs to change.


Licence guff:
-------------

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the Free
Software Foundation; either version 2 of the License, or (at your option)
any later version.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 51
Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.


