//
// Read PulseAudio input and encode to MP3.
//

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libswresample/swresample.h>
#include <stdbool.h>

struct Input {
	AVFormatContext * format_ctx;
	AVCodecContext * codec_ctx;
};

struct Output {
	AVFormatContext * format_ctx;
	AVCodecContext * codec_ctx;
	SwrContext * resample_ctx;
};

struct Audiostreamer {
	struct Input * input;
	struct Output * output;

	// Audio samples FIFO. Because decoder/encoder may provide/expect differing
	// numbers of samples, we buffer samples here.
	AVAudioFifo * af;

	// Presentation timestamp. Increases as we output samples.
	int64_t pts;

	// Number of frames written.
	uint64_t frames_written;
};

void
as_setup(void);

struct Input *
as_open_input(const char * const,
		const char * const, const bool);

void
as_destroy_input(struct Input * const);

struct Output *
as_open_output(const struct Input * const,
		const char * const, const char * const,
		const char * const);

void
as_destroy_output(struct Output * const);

struct Audiostreamer *
as_init_audiostreamer(struct Input * const, struct Output * const);

int
as_read_write(struct Audiostreamer * const, int * const);

void
as_destroy_audiostreamer(struct Audiostreamer * const);
