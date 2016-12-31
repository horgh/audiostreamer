This is a daemon and companion website to stream audio. I use it to stream from
my system's audio to an `<audio>` element. It outputs MP3 audio.

I thought it would be fun to be able to share what I am playing on my local
media player on a simple website. I wanted to take the audio directly from
PulseAudio and make it available in an `<audio>` element.

To do this, I've written a C library to read/decode audio from PulseAudio and
then encode/write it out as MP3. I use the library in Go with cgo.

Essentially this gives me a simple daemon I can start that will livestream my
system's audio.


# Requirements
  * ffmpeg (libavcodec, libavformat, libavdevice, libavutil, libswresample). I
    developed using 3.2.2. On Debian this is in the package libavutil-dev.
  * C compiler. I developed using gcc 6.2.1.
  * Go. I developed using 1.7.3.
  * The track display feature depends on accessing a
    [song_tracker](https://github.com/horgh/song_tracker) API. This is optional
    however.


# Installation
  * go get github.com/horgh/audiostreamer
  * go build
  * Place index.html somewhere accessible. Update the `song_tracker_url` and the
    `<audio>` element src attribute. If you don't want to use the song tracker,
    then set `song_tracker_url` to a blank string.
  * Run the daemon. Its usage output shows the possible flags. There is no
    configuration file.


# Components
  * audiostreamer: A Go daemon that serves HTTP requests to stream audio as
    MP3.
  * audiostreamer.h: A library that uses ffmpeg to read/decode from PulseAudio
    and encode/write to MP3.
  * index.html: A website containing an `<audio>` element that lets us stream
    audio from the daemon. It also displays the currently playing track (by
    polling a [song_tracker](https://github.com/horgh/song_tracker) API). In the
    case of daemon restart, clients try to reconnect endlessly.
  * transcode_example: A sample C program that uses audiostreamer.h to transcode
    to a file.


# Notes
  * In general MP3 is not a streamable format. To make it possible to slice and
    start streaming from anywhere (if an encode is already in progress), I
    disable the MP3 bit reservoir. This means I can have just one encoded
    stream for any number of streaming clients.
  * In theory output can be any audio format/codec. In a few places I have
    hardcoded use of MP3. To switch to a different output format/codec, it is
    likely sufficient to change the `outputFormat` and `outputEncoder` in
    `encoder()`, and adjust the `Content-Type` in `audioRequest()`. You must
    make sure the format/codec is streamable and that it is valid to send audio
    frames starting from any point, as that is the current behaviour.
  * In theory input can be from a file. In fact this does work, for a given
    value of work. Right now the daemon decodes as quickly as it can. When taken
    from a PulseAudio input the daemon is throttled as the audio is real time.
    From a file however there is no such limit, and it will consume 100% CPU
    (one thread). As well, it decodes the same file over and over. As my main
    use case is to stream from PulseAudio I have chosen to leave this as is as
    adjusting it introduces complexity.
