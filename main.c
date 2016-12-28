#include "audiostreamer.h"
#include <stdbool.h>

int
main(void)
{
	as_setup();


	// Open input and decoder.

	// Input from PulseAudio. Use `pactl list sources` to show available sources.
	const char * const input_format = "pulse";
	const char * const input_url
		= "alsa_output.pci-0000_00_1f.3.analog-stereo.monitor";

	// Input from an MP3
	//const char * const input_format = "mp3";
	//const char * const input_url = "file:/tmp/test.mp3";

	const bool verbose = true;
	struct Input * const input = as_open_input(input_format, input_url, verbose);
	if (!input) {
		return 1;
	}


	// Open output and encoder.

	// Output as MP3.
	struct Output * const output = as_open_output(input, "mp3", "file:out.mp3",
			"libmp3lame");

	// Output as webm+vorbis
	//struct Output * const output = as_open_output(input, "webm", "file:out.webm",
	//		"libvorbis");

	if (!output) {
		as_destroy_input(input);
		return 1;
	}


	// Read, decode, encode, write loop.

	// For testing purposes it is useful to limit how many frames we write before
	// exiting. Use -1 for no limit.
	const int max_frames = 300;

	if (!as_read_write_loop(input, output, max_frames)) {
		as_destroy_input(input);
		as_destroy_output(output);
		return 1;
	}


	// Clean up.

	as_destroy_input(input);
	as_destroy_output(output);

	return 0;
}
