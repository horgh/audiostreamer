//
// Read PulseAudio input and encode to MP3.
//

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
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

bool
as_read_write_loop(const struct Input * const,
		const struct Output * const, const int);
