This project provides a way to stream audio from a PulseAudio source over HTTP.

I thought it would be fun to be able to share what I am playing on my local
media player on a simple website. I wanted to take the audio directly from
PulseAudio and make it available in an <audio> element.

To do this, I've written a C library to read and decode audio from PulseAudio
and then encode and write it as MP3/Webm+Vorbis. I use it in Go with cgo.

Essentially this gives me a simple daemon I can start that will livestream my
system's audio.


# Requirements
  * ffmpeg (libavcodec, libavformat, libavdevice, libavutil, libswresample)
  * C compiler
  * Go


# Components
  * np: A Go daemon that serves HTTP requests to stream audio as
    MP3/Webm+Vorbis.
  * audiostreamer.h: A library that uses ffmpeg to read/decode from PulseAudio
    and encode/write to MP3/Webm+Vorbis.
  * main.c: A sample C program that uses audiostreamer.h to transcode to a file.
