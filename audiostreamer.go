package main

import (
	"bufio"
	"flag"
	"fmt"
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
	InputFormat string
	InputURL    string
	Verbose     bool
}

// HTTPHandler allows us to pass information to our request handlers.
type HTTPHandler struct {
	Verbose          bool
	ClientChangeChan chan<- int
	ClientChan       chan<- Client
}

// A Client is servicing one HTTP client. It receives audio data from the
// reader.
type Client struct {
	Audio chan Frame
	Done  chan struct{}
}

// Frame is an audio frame (compressed and encoded).
type Frame struct {
	Audio []byte
}

func main() {
	args, err := getArgs()
	if err != nil {
		log.Fatalf("Invalid argument: %s", err)
	}

	C.as_setup()

	// Encoder writes to out pipe. Reader reads from in pipe.
	in, out, err := os.Pipe()
	if err != nil {
		log.Fatalf("pipe: %s", err)
	}

	// Changes in clients announce on this channel. +1 for new client, -1 for
	// losing a client.
	clientChangeChan := make(chan int)

	// Clients provide reader a channel to receive on.
	//
	// The reader acts as a publisher and clients act as subscribers. One
	// publisher, potentially many subscribers.
	clientChan := make(chan Client)

	// When encoder writes a frame, we know how large it is by a message on this
	// channel. The encoder sends messages on this channel to the reader to
	// inform it of this. The reader then knows how much to read and allows it to
	// always read a single frame at a time, which is valid for a client to
	// receive. Otherwise if it reads without knowing frame boundaries, it is
	// difficult for it to know when it is valid to start sending data to a
	// client that enters mid-encoding.
	frameChan := make(chan int)

	go encoderSupervisor(out, args.InputFormat, args.InputURL, args.Verbose,
		clientChangeChan, frameChan)
	go reader(args.Verbose, in, clientChan, frameChan)

	hostPort := fmt.Sprintf("%s:%d", args.ListenHost, args.ListenPort)

	handler := HTTPHandler{
		Verbose:          args.Verbose,
		ClientChangeChan: clientChangeChan,
		ClientChan:       clientChan,
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
	input := flag.String("input", "", "Input URL valid for the given format. For MP3 you can give this as a path to a file. For PulseAudio you can give a value such as alsa_output.pci-0000_00_1f.3.analog-stereo.monitor to take input from a monitor. Use 'pactl list sources' to show the available PulseAudio sources.")
	verbose := flag.Bool("verbose", false, "Enable verbose logging output.")

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
		Verbose:     *verbose,
	}, nil
}

// The encoder supervisor deals with stopping and starting the encoder.
//
// We want there to be at most a single encoder goroutine active at any one
// time no matter how many clients there are. If there are zero clients, there
// should not be any encoding going on.
func encoderSupervisor(outPipe *os.File, inputFormat, inputURL string,
	verbose bool, clientChangeChan <-chan int, frameChan chan<- int) {
	// A count of how many clients are actively subscribed listening for audio.
	// We start the encoder when this goes above zero, and stop it if it goes to
	// zero.
	clients := 0

	// We close this channel to tell the encoder to stop. It receives no values.
	var encoderStopChan chan struct{}

	// The encoder tells us when it stops by sending a message on this channel.
	// Note I use sending a message as if this channel closes then the below loop
	// will be busy until it is re-opened.
	encoderDoneChan := make(chan struct{})

	for {
		select {
		// A change in the number of clients.
		case change := <-clientChangeChan:
			if verbose {
				log.Printf("encoder supervisor: client change: %d", change)
			}

			clients += change
			if clients == 0 {
				// Tell encoder to stop.
				close(encoderStopChan)
				if verbose {
					log.Printf("encoder supervisor: stopping encoder")
				}
				continue
			}

			if clients != 1 {
				continue
			}

			if verbose {
				log.Printf("encoder supervisor: starting encoder")
			}

			encoderStopChan = make(chan struct{})

			go encoder(outPipe, inputFormat, inputURL, encoderStopChan,
				encoderDoneChan, frameChan)

		// Encoder stopped for some reason. Restart it if appropriate.
		case <-encoderDoneChan:
			if verbose {
				log.Printf("encoder supervisor: encoder stopped")
			}

			if clients == 0 {
				// No clients. We don't need to restart it.
				continue
			}

			if verbose {
				log.Printf("encoder supervisor: starting encoder")
			}

			encoderStopChan = make(chan struct{})

			go encoder(outPipe, inputFormat, inputURL, encoderStopChan,
				encoderDoneChan, frameChan)
		}
	}
}

// encoder opens an audio input and begins decoding. It re-encodes the audio
// out and writes it to a pipe. It informs the reader goroutine how large each
// audio frame it writes is.
func encoder(outPipe *os.File, inputFormat, inputURL string,
	stopChan <-chan struct{}, doneChan chan<- struct{}, frameChan chan<- int) {
	inputFormatC := C.CString(inputFormat)
	inputURLC := C.CString(inputURL)
	verbose := C.bool(false)

	input := C.as_open_input(inputFormatC, inputURLC, verbose)
	if input == nil {
		log.Printf("Unable to open input")
		C.free(unsafe.Pointer(inputFormatC))
		C.free(unsafe.Pointer(inputURLC))
		doneChan <- struct{}{}
		return
	}
	C.free(unsafe.Pointer(inputFormatC))
	C.free(unsafe.Pointer(inputURLC))

	outputFormat := C.CString("mp3")
	outputURL := C.CString(fmt.Sprintf("pipe:%d", outPipe.Fd()))
	outputEncoder := C.CString("libmp3lame")

	output := C.as_open_output(input, outputFormat, outputURL, outputEncoder)
	if output == nil {
		log.Printf("Unable to open output")
		C.as_destroy_input(input)
		C.free(unsafe.Pointer(outputFormat))
		C.free(unsafe.Pointer(outputURL))
		C.free(unsafe.Pointer(outputEncoder))
		doneChan <- struct{}{}
		return
	}
	C.free(unsafe.Pointer(outputFormat))
	C.free(unsafe.Pointer(outputURL))
	C.free(unsafe.Pointer(outputEncoder))

	audiostreamer := C.as_init_audiostreamer(input, output)
	if audiostreamer == nil {
		log.Printf("Unable to initialize audiostreamer")
		C.as_destroy_output(output)
		C.as_destroy_input(input)
		doneChan <- struct{}{}
		return
	}
	defer C.as_destroy_audiostreamer(audiostreamer)

	for {
		select {
		// If stop channel is closed then we stop what we're doing.
		case <-stopChan:
			log.Printf("Stopping encoder")
			doneChan <- struct{}{}
			return
		default:
		}

		frameSize := C.int(0)
		res := C.as_read_write(audiostreamer, &frameSize)
		if res == -1 {
			log.Printf("Failure decoding/encoding")
			doneChan <- struct{}{}
			return
		}

		// EOF. Typical usage will never have EOF. However if we run with a file as
		// input then this may happen.
		if res == 0 {
			doneChan <- struct{}{}
			return
		}

		if frameSize > 0 {
			frameChan <- int(frameSize)
		}
	}
}

// reader reads the pipe containing the re-encoded audio.
//
// We send the audio to each client.
//
// We hear about new clients on the clients channel.
//
// We expect the pipe to never close. The encoder may stop sending for a while
// but when a new client appears, it starts again.
func reader(verbose bool, inPipe *os.File, clientChan <-chan Client,
	frameChan <-chan int) {
	reader := bufio.NewReader(inPipe)
	clients := []Client{}

	for {
		select {
		case client := <-clientChan:
			if verbose {
				log.Printf("reader: accepted new client")
			}

			clients = append(clients, client)
		case frameSize := <-frameChan:
			if verbose {
				//log.Printf("reader: reading new audio frame (%d bytes)", frameSize)
			}

			frame, err := readFrame(reader, frameSize)
			if err != nil {
				log.Printf("reader: %s", err)
				return
			}

			if verbose {
				//log.Printf("reader: read audio frame (%d bytes)", frameSize)
			}

			clients = sendFrameToClients(clients, frame)
		}
	}
}

// Read an audio frame.
func readFrame(reader *bufio.Reader, size int) (Frame, error) {
	buf := []byte{}
	bytesNeeded := size

	for {
		if bytesNeeded == 0 {
			return Frame{Audio: buf}, nil
		}

		readBuf := make([]byte, bytesNeeded)

		n, err := reader.Read(readBuf)
		if err != nil {
			return Frame{}, fmt.Errorf("read: %s", err)
		}

		buf = append(buf, readBuf[0:n]...)

		bytesNeeded -= n
	}
}

// Try to send the given block of audio to each client.
//
// If sending would block, cut the client off.
func sendFrameToClients(clients []Client, frame Frame) []Client {
	clients2 := []Client{}

	for _, client := range clients {
		select {
		case client.Audio <- frame:
			clients2 = append(clients2, client)
		case <-client.Done:
			close(client.Audio)
		default:
			close(client.Audio)
		}
	}

	return clients2
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
	c := Client{
		// We receive audio data on this channel from the reader.
		Audio: make(chan Frame, 512),

		// We close this channel to indicate to reader we're done. This is necessary
		// if we terminate, otherwise the reader can't know to stop sending us audio.
		Done: make(chan struct{}),
	}

	// Tell the reader we're here.
	h.ClientChan <- c

	// Tell the encoder we're here.
	h.ClientChangeChan <- 1

	rw.Header().Set("Content-Type", "audio/mpeg")
	rw.Header().Set("Cache-Control", "no-cache, no-store, must-revalidate")

	// We send chunked by default

	for {
		frame, ok := <-c.Audio

		// Reader may have cut us off.
		if !ok {
			log.Printf("reader closed audio channel")
			break
		}

		n, err := rw.Write(frame.Audio)
		if err != nil {
			log.Printf("write: %s", err)
			break
		}

		if n != len(frame.Audio) {
			log.Printf("short write")
			break
		}

		if h.Verbose {
			//log.Printf("%s: Sent %d bytes to client", r.RemoteAddr, n)
		}
	}

	h.ClientChangeChan <- -1

	close(c.Done)

	// Drain audio channel.
	for range c.Audio {
	}

	log.Printf("%s: Client cleaned up", r.RemoteAddr)
}
