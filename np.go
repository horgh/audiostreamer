package main

import (
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"unsafe"
)

// #include "audiostreamer.h"
// #include <stdlib.h>
// #cgo LDFLAGS: -lavformat -lavdevice -lavcodec -lavutil -lswresample
import "C"

// Args holds command line arguments.
type Args struct {
	ListenHost  string
	ListenPort  int
	Verbose     bool
	InputFormat string
	InputURL    string
}

// HTTPHandler allows us to pass information to our request handlers.
type HTTPHandler struct {
	Verbose     bool
	InputFormat string
	InputURL    string
}

func main() {
	args, err := getArgs()
	if err != nil {
		log.Fatalf("Invalid argument: %s", err)
	}

	C.as_setup()

	hostPort := fmt.Sprintf("%s:%d", args.ListenHost, args.ListenPort)

	handler := HTTPHandler{
		Verbose:     args.Verbose,
		InputFormat: args.InputFormat,
		InputURL:    args.InputURL,
	}
	s := &http.Server{
		Addr:    hostPort,
		Handler: handler,
	}

	log.Printf("Starting to serve requests on %s (HTTP)", hostPort)

	err = s.ListenAndServe()
	if err != nil {
		log.Fatalf("Unable to serve: %s", err)
	}
}

// getArgs retrieves and validates command line arguments.
func getArgs() (Args, error) {
	listenHost := flag.String("host", "localhost", "Host to listen on.")
	listenPort := flag.Int("port", 8080, "Port to listen on.")
	format := flag.String("format", "pulse", "Input format. pulse for PulseAudio or mp3 for MP3.")
	input := flag.String("input", "", "Input URL valid for the given format. For MP3 you can give this as a path to a file. For PulseAudio you can give a value such as alsa_output.pci-0000_00_1f.3.analog-stereo.monitor to take input from a monitor. Use `pactl list sources` to show the available PulseAudio sources.")

	flag.Parse()

	if len(*listenHost) == 0 {
		flag.PrintDefaults()
		return Args{}, fmt.Errorf("you must provide a host")
	}

	if len(*format) == 0 {
		flag.PrintDefaults()
		return Args{}, fmt.Errorf("you must provide an input format")
	}

	if len(*input) == 0 {
		flag.PrintDefaults()
		return Args{}, fmt.Errorf("you must provide an input URL")
	}

	return Args{
		ListenHost:  *listenHost,
		ListenPort:  *listenPort,
		InputFormat: *format,
		InputURL:    *input,
	}, nil
}

// ServeHTTP handles an HTTP request.
func (h HTTPHandler) ServeHTTP(rw http.ResponseWriter, r *http.Request) {
	log.Printf("Serving [%s] request from [%s] to path [%s] (%d bytes)",
		r.Method, r.RemoteAddr, r.URL.Path, r.ContentLength)

	if r.Method == "GET" && r.URL.Path == "/audio" {
		h.audioRequest(rw, r)
		return
	}

	log.Printf("Unknown request.")
	rw.WriteHeader(http.StatusNotFound)
	_, _ = rw.Write([]byte("<h1>404 Not found</h1>"))
}

func (h HTTPHandler) audioRequest(rw http.ResponseWriter, r *http.Request) {
	// Encoder writes to writer. Reader reads from reader.
	in, out, err := os.Pipe()
	if err != nil {
		log.Printf("pipe: %s", err)
		return
	}

	// We receive audio data on this channel from the reader.
	ch := make(chan []byte)

	go encoder(out, h.InputFormat, h.InputURL)
	go reader(in, ch)

	rw.Header().Set("Content-Type", "audio/mpeg")
	rw.Header().Set("Cache-Control", "no-cache, no-store, must-revalidate")

	// We send chunked by default

	for {
		b := <-ch

		if len(b) == 0 {
			if h.Verbose {
				log.Printf("%s: Done sending audio to client", r.RemoteAddr)
			}
			return
		}

		n, err := rw.Write(b)
		if err != nil {
			log.Printf("write: %s", err)
			return
		}
		if n != len(b) {
			log.Printf("short write")
			return
		}

		if h.Verbose {
			log.Printf("%s: Sent %d bytes to client", r.RemoteAddr, len(b))
		}
	}
}

// encoder opens an audio input and begins decoding. It re-encodes the audio
// out to a file.
func encoder(out *os.File, inputFormat, inputURL string) {
	inputFormatC := C.CString(inputFormat)
	defer C.free(unsafe.Pointer(inputFormatC))
	inputURLC := C.CString(inputURL)
	defer C.free(unsafe.Pointer(inputURLC))
	verbose := C.bool(false)

	input := C.as_open_input(inputFormatC, inputURLC, verbose)
	if input == nil {
		log.Printf("Unable to open input")
		return
	}
	defer C.as_destroy_input(input)

	outputFormat := C.CString("mp3")
	defer C.free(unsafe.Pointer(outputFormat))
	outputURL := C.CString(fmt.Sprintf("pipe:%d", out.Fd()))
	defer C.free(unsafe.Pointer(outputURL))
	outputEncoder := C.CString("libmp3lame")
	defer C.free(unsafe.Pointer(outputEncoder))

	output := C.as_open_output(input, outputFormat, outputURL, outputEncoder)
	if output == nil {
		log.Printf("Unable to open output")
		return
	}
	defer C.as_destroy_output(output)

	if !C.as_read_write_loop(input, output, -1) {
		log.Printf("Failure decoding/encoding")
		return
	}

	err := out.Close()
	if err != nil {
		log.Printf("Failure closing write pipe: %s", err)
	}
}

// reader reads the file containing the re-encoded audio. It sends the data to
// the given channel.
func reader(in *os.File, dest chan<- []byte) {
	buf := []byte{}

	// 512 KiB. I buffer as I saw stuttering when sending too little at a time.
	size := 1024 * 512

	for {
		if len(buf) >= size {
			sendBuf := buf[:size]
			dest <- sendBuf
			buf = buf[size:]
			continue
		}

		readBuf := make([]byte, size)

		n, err := in.Read(readBuf)
		if err != nil {
			if err == io.EOF {
				log.Printf("reader: Hit EOF")
				if len(buf) > 0 {
					dest <- buf
				}
				dest <- []byte{}
				return
			}

			log.Printf("reader: Read: %s", err)
			return
		}

		buf = append(buf, readBuf[:n]...)
	}
}
