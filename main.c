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


	// Read, decode, encode, write in a loop until we either hit input EOF or
	// reach the maximum number of frames we want to output right now.
	struct Audiostreamer * const as = as_init_audiostreamer(input, output);
	if (!as) {
		as_destroy_input(input);
		as_destroy_output(output);
		return 1;
	}

	// For testing purposes it is useful to limit how many frames we write before
	// exiting. Use -1 for no limit.
	const uint64_t max_frames = 100;

	while (1) {
		int frame_sz = 0;
		const int res = as_read_write(as, &frame_sz);
		if (res == -1) {
			printf("error\n");
			as_destroy_audiostreamer(as);
			return 1;
		}

		if (res == 0) {
			break;
		}

		if (frame_sz > 0) {
			printf("wrote frame size %d\n", frame_sz);
		} else {
			printf("didn't write frame\n");
		}

		if (as->frames_written == max_frames) {
			printf("hit max frames written\n");
			break;
		}
	}


	// Clean up.

	as_destroy_audiostreamer(as);

	return 0;
}
