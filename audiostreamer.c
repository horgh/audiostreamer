//
// Read PulseAudio input and encode to MP3.
//

#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libswresample/swresample.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct Input {
	AVFormatContext * format_ctx;
	AVCodecContext * codec_ctx;
};

struct Output {
	AVFormatContext * format_ctx;
	AVCodecContext * codec_ctx;
	SwrContext * resample_ctx;
};

static void
__setup(void);
static struct Input *
__open_input(const char * const, const char * const,
		bool);
static void
__destroy_input(struct Input * const);
static struct Output *
__open_output(const struct Input * const,
		const char * const, const char * const,
		const char * const);
static void
__destroy_output(struct Output * const);
static bool
__read_write_loop(const struct Input * const,
		const struct Output * const);
static int
__decode_and_store_frame(const struct Input * const,
		const struct Output * const, AVAudioFifo * const);
static bool
__encode_and_write_frame(const struct Output * const,
		AVAudioFifo * const, int64_t * const);
static char *
__get_error_string(const int);

int
main(void)
{
	__setup();


	// Open input and decoder.

	// PulseAudio input format.
	const char * const input_format = "pulse";
	const char * const input_url
		= "alsa_output.pci-0000_00_1f.3.analog-stereo.monitor";
	const bool verbose = true;
	struct Input * const input = __open_input(input_format, input_url, verbose);
	if (!input) {
		return 1;
	}


	// Open output and encoder.

	struct Output * const output = __open_output(input, "mp3", "out.mp3",
			"libmp3lame");
	if (!output) {
		__destroy_input(input);
		return 1;
	}


	// Read, decode, encode, write loop.

	if (!__read_write_loop(input, output)) {
		__destroy_input(input);
		__destroy_output(output);
		return 1;
	}


	// Clean up.

	__destroy_input(input);
	__destroy_output(output);

	return 0;
}

static void
__setup(void)
{
	// Set up library.

	// Register muxers, demuxers, and protocols.
	av_register_all();

	// Make formats available. Specifically, pulse (pulseaudio).
	avdevice_register_all();
}

// Open input and set up decoder.
static struct Input *
__open_input(const char * const input_format_name, const char * const input_url,
		bool verbose)
{
	if (!input_format_name || strlen(input_format_name) == 0 ||
			!input_url || strlen(input_url) == 0) {
		printf("%s\n", strerror(EINVAL));
		return NULL;
	}

	struct Input * const input = calloc(1, sizeof(struct Input));
	if (!input) {
		printf("%s\n", strerror(errno));
		return NULL;
	}

	// Find the format.
	AVInputFormat * input_format = av_find_input_format(input_format_name);
	if (!input_format) {
		printf("input format not found\n");
		__destroy_input(input);
		return NULL;
	}

	// Open the input stream.
	if (avformat_open_input(&input->format_ctx, input_url, input_format,
				NULL) != 0) {
		printf("open input failed\n");
		__destroy_input(input);
		return NULL;
	}

	// Read packets to get stream info.
	if (avformat_find_stream_info(input->format_ctx, NULL) < 0) {
		printf("failed to find stream info\n");
		__destroy_input(input);
		return NULL;
	}

	// Dump info about the input format.
	if (verbose) {
		av_dump_format(input->format_ctx, 0, input_url, 0);
	}

	// Find codec for the input stream.
	AVCodec * input_codec = avcodec_find_decoder(
			input->format_ctx->streams[0]->codecpar->codec_id);
	if (!input_codec) {
		printf("codec not found\n");
		__destroy_input(input);
		return NULL;
	}

	// Set up decoding context (demuxer).
	input->codec_ctx = avcodec_alloc_context3(input_codec);
	if (!input->codec_ctx) {
		printf("could not allocate codec context\n");
		__destroy_input(input);
		return NULL;
	}

	// Set decoder attributes (channels, sample rate, etc). I think we could set
	// these manually, but I copy from the input stream.
	if (avcodec_parameters_to_context(input->codec_ctx,
				input->format_ctx->streams[0]->codecpar) < 0) {
		printf("unable to initialize input codec parameters\n");
		__destroy_input(input);
		return NULL;
	}

	// Initialize the codec context to use the codec. This is needed even though
	// we passed the codec to avcodec_alloc_context3().
	if (avcodec_open2(input->codec_ctx, input_codec, NULL) != 0) {
		printf("unable to initialize codec context\n");
		__destroy_input(input);
		return NULL;
	}

	return input;
}

static void
__destroy_input(struct Input * const input)
{
	if (!input) {
		return;
	}

	if (input->format_ctx) {
		avformat_close_input(&input->format_ctx);
		avformat_free_context(input->format_ctx);
	}

	if (input->codec_ctx) {
		avcodec_free_context(&input->codec_ctx);
	}

	free(input);
}

// Open output and set up encoder.
//
// output_url: For stdout use 'pipe:1'. For output to a file use 'file:out.mp3'
// (to name the file out.mp3).
static struct Output *
__open_output(const struct Input * const input,
		const char * const output_format, const char * const output_url,
		const char * const output_encoder)
{
	if (!output_format || strlen(output_format) == 0 ||
			!output_url || strlen(output_url) == 0 ||
			!output_encoder || strlen(output_encoder) == 0) {
		printf("%s\n", strerror(EINVAL));
		return NULL;
	}

	struct Output * const output = calloc(1, sizeof(struct Output));
	if (!output) {
		printf("%s\n", strerror(errno));
		return NULL;
	}

	// Set up muxing context.
	// AVFormatContext is used for muxing (as well as demuxing).
	// In addition to allocating the context, this sets up the output format which
	// sets which muxer to use.
	if (avformat_alloc_output_context2(&output->format_ctx, NULL, output_format,
				NULL) < 0) {
		printf("unable to allocate AVFormatContext\n");
		__destroy_output(output);
		return NULL;
	}


	// Open IO context - open output file.
	if (avio_open(&output->format_ctx->pb, output_url, AVIO_FLAG_WRITE) < 0) {
		printf("unable to open output\n");
		__destroy_output(output);
		return NULL;
	}


	// Create output stream.

	AVCodec * output_codec = avcodec_find_encoder_by_name(output_encoder);
	if (!output_codec) {
		printf("output codec not found\n");
		__destroy_output(output);
		return NULL;
	}

	// There is a codec member in the stream, but it's deprecated and says we
	// should use its codecpar member instead.
	AVStream * avs = avformat_new_stream(output->format_ctx, output_codec);
	if (!avs) {
		printf("unable to add stream\n");
		__destroy_output(output);
		return NULL;
	}

	avs->time_base.den = input->codec_ctx->sample_rate;
	avs->time_base.num = 1;


	// Set up output encoder

	output->codec_ctx = avcodec_alloc_context3(output_codec);
	if (!output->codec_ctx) {
		printf("unable to allocate output codec context\n");
		__destroy_output(output);
		return NULL;
	}

	output->codec_ctx->channels       = input->codec_ctx->channels;
	output->codec_ctx->channel_layout = AV_CH_LAYOUT_STEREO;
	output->codec_ctx->sample_rate    = input->codec_ctx->sample_rate;
	output->codec_ctx->sample_fmt     = output_codec->sample_fmts[0];
	// 96 Kb/s
	output->codec_ctx->bit_rate       = 96000;

	// Initialize the codec context to use the codec.
	if (avcodec_open2(output->codec_ctx, output_codec, NULL) != 0) {
		printf("unable to initialize output codec context to use codec\n");
		__destroy_output(output);
		return NULL;
	}

	// Set AVStream.codecpar (stream codec parameters).
	if (avcodec_parameters_from_context(avs->codecpar, output->codec_ctx) < 0) {
		printf("unable to set output codec parameters\n");
		__destroy_output(output);
		return NULL;
	}

	// Write file header
	if (avformat_write_header(output->format_ctx, NULL) < 0) {
		printf("unable to write header\n");
		__destroy_output(output);
		return NULL;
	}


	// Set up resampler. To be able to convert audio sample formats, we need a
	// resampler. See transcode_aac.c

	output->resample_ctx = swr_alloc_set_opts(
			NULL,
			av_get_default_channel_layout(output->codec_ctx->channels),
			output->codec_ctx->sample_fmt,
			output->codec_ctx->sample_rate,
			av_get_default_channel_layout(input->codec_ctx->channels),
			input->codec_ctx->sample_fmt,
			input->codec_ctx->sample_rate,
			0,
			NULL);
	if (!output->resample_ctx) {
		printf("unable to allocate resample context\n");
		__destroy_output(output);
		return NULL;
	}

	if (swr_init(output->resample_ctx) < 0) {
		printf("unable to open resample context\n");
		__destroy_output(output);
		return NULL;
	}

	return output;
}

static void
__destroy_output(struct Output * const output)
{
	if (!output) {
		return;
	}

	if (output->format_ctx) {
		// Write file trailer.
		if (av_write_trailer(output->format_ctx) != 0) {
			printf("unable to write trailer\n");
		}

		avformat_free_context(output->format_ctx);
	}

	if (output->codec_ctx) {
		avcodec_free_context(&output->codec_ctx);
	}

	if (output->resample_ctx) {
		swr_free(&output->resample_ctx);
	}

	free(output);
}

// Begin read, decode, encode, write process.
//
// We repeatedly read in data (av_read_frame()). This gives us an AVPacket with
// encoded data. We decode each packet with avcodec_send_packet(). Then we get
// the decoded data out using avcodec_receive_frame().
//
// Writing: avformat_write_header() to write the file header, then
// av_write_frame() repeatedly to write each encoded packet, and finally
// av_write_trailer() to finalize the file.
//
// For some more info on this, read top explanatory comment in avcodec.h.
// avformat.h also has some information.
static bool
__read_write_loop(const struct Input * const input,
		const struct Output * const output)
{
	if (!input || !output) {
		printf("%s\n", strerror(EINVAL));
		return false;
	}

	// The number of samples read in a frame can be larger or smaller than what
	// the encoder wants. We need to give it the exact number of frames it wants.
	//
	// This means we can't simply feed a single frame at a time from the input to
	// the encoder.
	//
	// We use a AvAudioFifo for buffering. Note AudioFrameQueue looks like
	// something similar but is apparently not available in my version of ffmpeg.
	// Also, it appears to not hold raw data either, so I'm not sure how to use
	// it.

	// We must have an initial allocation size of at least 1.
	AVAudioFifo * af = av_audio_fifo_alloc(output->codec_ctx->sample_fmt,
			output->codec_ctx->channels, 1);
	if (!af) {
		printf("unable to allocate audio fifo\n");
		return false;
	}

	// Presentation timestamp (PTS). This needs to increase for each sample we
	// output.
	int64_t pts = 1;

	while (1) {
		// Find how many samples are in the FIFO.
		const int available_samples = av_audio_fifo_size(af);

		// Do we need to read & decode another frame from the input?
		if (available_samples < output->codec_ctx->frame_size) {
			const int read_res = __decode_and_store_frame(input, output, af);
			if (read_res == -1) {
				av_audio_fifo_free(af);
				return false;
			}

			if (read_res == 0) {
				break;
			}

			continue;
		}

		// We have enough samples to encode and write.
		if (!__encode_and_write_frame(output, af, &pts)) {
			av_audio_fifo_free(af);
			return false;
		}
	}

	av_audio_fifo_free(af);
	return true;
}

// Read an encoded frame (packet) from the input. Decode it (frame). Store the
// frame's samples into the FIFO.
//
// Returns:
// 1 if read a frame
// 0 if EOF
// -1 if error
static int
__decode_and_store_frame(const struct Input * const input,
		const struct Output * const output, AVAudioFifo * const af)
{
	if (!input || !af) {
		printf("%s\n", strerror(EINVAL));
		return -1;
	}

	// Read an encoded frame as a packet.
	AVPacket input_pkt;
	memset(&input_pkt, 0, sizeof(AVPacket));

	if (av_read_frame(input->format_ctx, &input_pkt) != 0) {
		printf("unable to read frame\n");
		// EOF.
		return 0;
	}


	// Send encoded packet to the input's decoder.
	if (avcodec_send_packet(input->codec_ctx, &input_pkt) != 0) {
		printf("send_packet failed\n");
		av_packet_unref(&input_pkt);
		return -1;
	}

	av_packet_unref(&input_pkt);


	// Get decoded data out as a frame.
	AVFrame * input_frame = av_frame_alloc();
	if (!input_frame) {
		printf("av_frame_alloc\n");
		return -1;
	}

	if (avcodec_receive_frame(input->codec_ctx, input_frame) != 0) {
		printf("avcodec_receive_frame failed\n");
		av_frame_free(&input_frame);
		return -1;
	}


	// Convert the samples

	uint8_t * * converted_input_samples = calloc(
			(size_t) output->codec_ctx->channels, sizeof(uint8_t *));
	if (!converted_input_samples) {
		printf("%s\n", strerror(errno));
		av_frame_free(&input_frame);
		return -1;
	}

	if (av_samples_alloc(converted_input_samples, NULL,
				output->codec_ctx->channels, input_frame->nb_samples,
				output->codec_ctx->sample_fmt, 0) < 0) {
		printf("av_samples_alloc\n");
		av_frame_free(&input_frame);
		free(converted_input_samples);
		return -1;
	}

	if (swr_convert(output->resample_ctx, converted_input_samples,
				input_frame->nb_samples,
				(uint8_t * *) input_frame->extended_data,
				//input_data,
				//raw_input_samples,
				input_frame->nb_samples) < 0) {
		printf("swr_convert\n");
		av_frame_free(&input_frame);
		free(converted_input_samples);
		return -1;
	}


	// Add the samples to the fifo.

	// Resize fifo so it can contain old and new samples.
	if (av_audio_fifo_realloc(af,
				av_audio_fifo_size(af)+input_frame->nb_samples) != 0) {
		printf("unable to resize fifo\n");
		av_frame_free(&input_frame);
		free(converted_input_samples);
		return -1;
	}

	if (av_audio_fifo_write(af, (void * *) converted_input_samples,
				input_frame->nb_samples) != input_frame->nb_samples) {
		printf("could not write all samples to fifo\n");
		av_frame_free(&input_frame);
		free(converted_input_samples);
		return -1;
	}

	av_frame_free(&input_frame);
	free(converted_input_samples);

	return 1;
}

// Take samples from the FIFO, encode them, and write them to the encoder. We
// try to pull out a fully encoded frame from the encoder, which may or may not
// succeed, depending on whether there is sufficient data present. If there is,
// we write the frame to the output.
//
// We update the pts.
static bool
__encode_and_write_frame(const struct Output * const output,
		AVAudioFifo * const af, int64_t * const pts)
{
	if (!output || !af || !pts) {
		printf("%s\n", strerror(EINVAL));
		return false;
	}

	// Get frame out of fifo.

	AVFrame * output_frame = av_frame_alloc();
	if (!output_frame) {
		printf("unable to allocate output frame\n");
		return false;
	}

	output_frame->nb_samples     = output->codec_ctx->frame_size;
	output_frame->channel_layout = output->codec_ctx->channel_layout;
	output_frame->format         = output->codec_ctx->sample_fmt;
	output_frame->sample_rate    = output->codec_ctx->sample_rate;

	if (av_frame_get_buffer(output_frame, 0) < 0) {
		printf("unable to allocate output frame buffer\n");
		av_frame_free(&output_frame);
		return false;
	}

	if (av_audio_fifo_read(af, (void * *) output_frame->data,
				output->codec_ctx->frame_size) < output->codec_ctx->frame_size) {
		printf("short read from fifo\n");
		av_frame_free(&output_frame);
		return false;
	}

	output_frame->pts = *pts;
	*pts += output_frame->nb_samples;


	// Send the raw frame to the encoder.
	int error = avcodec_send_frame(output->codec_ctx, output_frame);
	if (error != 0) {
		printf("avcodec_send_frame failed: %s\n", __get_error_string(error));
		av_frame_free(&output_frame);
		return false;
	}

	av_frame_free(&output_frame);


	// Read encoded data from the encoder.

	AVPacket output_pkt;
	memset(&output_pkt, 0, sizeof(AVPacket));

	error = avcodec_receive_packet(output->codec_ctx, &output_pkt);
	if (error != 0) {
		// We expect that we will not always have enough data to get a fully encoded
		// frame out.
		if (error == AVERROR(EAGAIN)) {
			return true;
		}

		printf("avcodec_receive_packet failed: %s\n", __get_error_string(error));
		return false;
	}


	// Write encoded data packet out using av_write_frame().
	if (av_write_frame(output->format_ctx, &output_pkt) < 0) {
		printf("av_write_frame failed\n");
		av_packet_unref(&output_pkt);
		return false;
	}

	av_packet_unref(&output_pkt);
	return true;
}

// Take an error code from the ffmpeg libraries and translate it into a string.
//
// Do not free the returned buffer.
static char *
__get_error_string(const int error)
{
	static char buf[255];
	memset(buf, 0, 255);

	av_strerror(error, buf, 255);

	return buf;
}
