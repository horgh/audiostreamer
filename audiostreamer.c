#include "audiostreamer.h"
#include <errno.h>
#include <libavdevice/avdevice.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int
__decode_and_store_frame(const struct Audiostreamer * const);
static int
__decode_and_store_samples(const struct Input * const,
		const struct Output * const, AVAudioFifo * const);
static const uint8_t * *
__copy_samples(uint8_t * * const, const int);
static int
__encode_and_write_frame(struct Audiostreamer * const);
static int
__read_and_write_packet(struct Audiostreamer * const);
static char *
__get_error_string(const int);
static bool
__drain_codecs(struct Audiostreamer * const);

void
as_setup(void)
{
	// Set up library.

	// Register muxers, demuxers, and protocols.
	av_register_all();

	// Make formats available. Specifically, pulse (pulseaudio).
	avdevice_register_all();
}

// Open input and set up decoder.
struct Input *
as_open_input(const char * const input_format_name,
		const char * const input_url, const bool verbose)
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
	AVInputFormat * const input_format = av_find_input_format(input_format_name);
	if (!input_format) {
		printf("input format not found\n");
		as_destroy_input(input);
		return NULL;
	}

	// Open the input stream.
	if (avformat_open_input(&input->format_ctx, input_url, input_format,
				NULL) != 0) {
		printf("open input failed\n");
		as_destroy_input(input);
		return NULL;
	}

	// Read packets to get stream info.
	if (avformat_find_stream_info(input->format_ctx, NULL) < 0) {
		printf("failed to find stream info\n");
		as_destroy_input(input);
		return NULL;
	}

	// Dump info about the input format.
	if (verbose) {
		av_dump_format(input->format_ctx, 0, input_url, 0);
	}

	// Find codec for the input stream.
	AVCodec * const input_codec = avcodec_find_decoder(
			input->format_ctx->streams[0]->codecpar->codec_id);
	if (!input_codec) {
		printf("codec not found\n");
		as_destroy_input(input);
		return NULL;
	}

	// Set up decoding context (demuxer).
	input->codec_ctx = avcodec_alloc_context3(input_codec);
	if (!input->codec_ctx) {
		printf("could not allocate codec context\n");
		as_destroy_input(input);
		return NULL;
	}

	// Set decoder attributes (channels, sample rate, etc). I think we could set
	// these manually, but I copy from the input stream.
	if (avcodec_parameters_to_context(input->codec_ctx,
				input->format_ctx->streams[0]->codecpar) < 0) {
		printf("unable to initialize input codec parameters\n");
		as_destroy_input(input);
		return NULL;
	}

	// Initialize the codec context to use the codec. This is needed even though
	// we passed the codec to avcodec_alloc_context3().
	if (avcodec_open2(input->codec_ctx, input_codec, NULL) != 0) {
		printf("unable to initialize codec context\n");
		as_destroy_input(input);
		return NULL;
	}

	return input;
}

void
as_destroy_input(struct Input * const input)
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
struct Output *
as_open_output(const struct Input * const input,
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
		as_destroy_output(output);
		return NULL;
	}


	// Open IO context - open output file.
	if (avio_open(&output->format_ctx->pb, output_url, AVIO_FLAG_WRITE) < 0) {
		printf("unable to open output\n");
		as_destroy_output(output);
		return NULL;
	}


	// Create output stream.

	AVCodec * const output_codec = avcodec_find_encoder_by_name(output_encoder);
	if (!output_codec) {
		printf("output codec not found\n");
		as_destroy_output(output);
		return NULL;
	}

	// There is a codec member in the stream, but it's deprecated and says we
	// should use its codecpar member instead.
	// Apparently we don't need to pass in codec. Don't do it. There is a memory
	// leak if I do that I can't resolve. I think it may be due to the codec
	// member being deprecated.
	AVStream * const stream = avformat_new_stream(output->format_ctx, NULL);
	if (!stream) {
		printf("unable to add stream\n");
		as_destroy_output(output);
		return NULL;
	}

	// Unit of time (in seconds) in which frame timestamps are represented.

	// Numerator
	stream->time_base.num = 1;
	// Denominator
	stream->time_base.den = input->codec_ctx->sample_rate;


	// Set up output encoder

	output->codec_ctx = avcodec_alloc_context3(output_codec);
	if (!output->codec_ctx) {
		printf("unable to allocate output codec context\n");
		as_destroy_output(output);
		return NULL;
	}

	output->codec_ctx->channels       = input->codec_ctx->channels;
	output->codec_ctx->channel_layout = AV_CH_LAYOUT_STEREO;
	output->codec_ctx->sample_rate    = input->codec_ctx->sample_rate;
	output->codec_ctx->sample_fmt     = output_codec->sample_fmts[0];
	// 96 Kb/s
	output->codec_ctx->bit_rate       = 96000;

	// Turn off using bit reservoir if we're using MP3. This allows any frame to
	// be valid on its own in exchange for a potential reduction in quality. See
	// http://lame.sourceforge.net/tech-FAQ.txt and
	// http://wiki.hydrogenaud.io/index.php?title=Bit_reservoir
	//
	// My intention is it to be valid to start streaming with any frame.
	if (strcmp(output_encoder, "libmp3lame") == 0) {
		const int error = av_opt_set_int(output->codec_ctx->priv_data, "reservoir",
				0, 0);
		if (error != 0) {
			printf("unable to set option: %s\n", __get_error_string(error));
			as_destroy_output(output);
			return NULL;
		}
	}

	// Initialize the codec context to use the codec.
	if (avcodec_open2(output->codec_ctx, output_codec, NULL) != 0) {
		printf("unable to initialize output codec context to use codec\n");
		as_destroy_output(output);
		return NULL;
	}

	// Set AVStream.codecpar (stream codec parameters).
	if (avcodec_parameters_from_context(stream->codecpar,
				output->codec_ctx) < 0) {
		printf("unable to set output codec parameters\n");
		as_destroy_output(output);
		return NULL;
	}

	// Write file header
	if (avformat_write_header(output->format_ctx, NULL) < 0) {
		printf("unable to write header\n");
		as_destroy_output(output);
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
		as_destroy_output(output);
		return NULL;
	}

	if (swr_init(output->resample_ctx) < 0) {
		printf("unable to open resample context\n");
		as_destroy_output(output);
		return NULL;
	}

	return output;
}

void
as_destroy_output(struct Output * const output)
{
	if (!output) {
		return;
	}

	if (output->format_ctx) {
		// Write file trailer.
		if (av_write_trailer(output->format_ctx) != 0) {
			printf("unable to write trailer\n");
		}

		if (avio_closep(&output->format_ctx->pb) != 0) {
			printf("avio_closep failed\n");
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

// Create an initialize a new Audiostreamer struct.
struct Audiostreamer *
as_init_audiostreamer(struct Input * const input, struct Output * const output)
{
	struct Audiostreamer * const as = calloc(1, sizeof(struct Audiostreamer));
	if (!as) {
		printf("%s\n", strerror(errno));
		return NULL;
	}

	// The number of samples read in a frame from the input can be larger or
	// smaller than what the encoder wants. We need to give it the exact number
	// it wants. This means we can't reliably feed a single frame at a time from
	// the input into the output.
	//
	// To make it possible to always feed the expected number of samples to the
	// encoder, we use a AvAudioFifo for buffering samples. We read and decode
	// samples from the input, and add them to the FIFO queue. When we have
	// enough, we extract, encode, and write them to the output.
	//
	// Note AudioFrameQueue looks like something similar to AvAudioFifo, but is
	// apparently not available in my version of ffmpeg. Also, it appears to not
	// hold raw data either, so I'm not sure it is applicable.

	// We must have an initial allocation size of at least 1.
	as->af = av_audio_fifo_alloc(output->codec_ctx->sample_fmt,
			output->codec_ctx->channels, 1);
	if (!as->af) {
		printf("unable to allocate audio fifo\n");
		as_destroy_audiostreamer(as);
		return NULL;
	}

	// Presentation timestamp (PTS). This needs to increase for each sample we
	// output.
	as->pts = 1;

	as->frames_written = 0;

	as->input = input;
	as->output = output;

	return as;
}

// Take one of two actions each call:
//
// 1. If there are insufficient samples for the encoder, read more samples.
// 2. If there are sufficient samples for the encoder, feed them to the encoder
//   and try to write a frame.
//
// If we hit stream EOF on read, we drain the codecs and complete.
//
// To transcode an input to an output, call this function repeatedly until it
// returns 0 (or -1).
//
// I've chosen to make this do one unit of work so as to let the caller be able
// to decide whether to stop or not. This is as opposed to having while (1) in
// a single function and make interruption be problematic.
//
// Essentially how the decoding/encoding process works is:
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
//
// Returns:
// 1 if made progress (and more to do)
// 0 if done (hit EOF)
// -1 if error
//
// If we write a packet out, its size will be in frame_size (compressed size).
int
as_read_write(struct Audiostreamer * const as, int * const frame_size)
{
	if (!as || !as->input || !as->output || !as->af) {
		printf("%s\n", strerror(EINVAL));
		return -1;
	}

	// Find how many samples are in the FIFO.
	const int available_samples = av_audio_fifo_size(as->af);

	// Do we need to read & decode another frame from the input? We do if there
	// are insufficient number of samples for the encoder.
	if (available_samples < as->output->codec_ctx->frame_size) {
		const int read_res = __decode_and_store_frame(as);
		if (read_res == -1) {
			printf("__decode_and_store_frame error\n");
			return -1;
		}

		// EOF from input.
		if (read_res == 0) {
			if (!__drain_codecs(as)) {
				printf("unable to drain codecs\n");
				return -1;
			}

			// We're done.
			return 0;
		}

		// We did some work. Let caller call us again.
		return 1;
	}

	// We have enough samples to encode and write.

	const int write_res = __encode_and_write_frame(as);
	if (write_res == -1) {
		return -1;
	}

	if (write_res > 0) {
		if (as->frames_written == UINT64_MAX) {
			as->frames_written = 0;
		} else {
			as->frames_written += 1;
		}

		*frame_size = write_res;
	}

	// We did a unit of work. Let caller call us again.
	return 1;
}

// Destroy an audiostreamer.
//
// We clean up everything including input and output.
void
as_destroy_audiostreamer(struct Audiostreamer * const as)
{
	if (!as) {
		return;
	}

	if (as->input) {
		as_destroy_input(as->input);
	}

	if (as->output) {
		as_destroy_output(as->output);
	}

	if (as->af) {
		av_audio_fifo_free(as->af);
	}

	free(as);
}

// Read an encoded frame (packet) from the input. Decode it (frame). Store the
// frame's samples into the FIFO.
//
// Returns:
// 1 if read a frame
// 0 if EOF
// -1 if error
static int
__decode_and_store_frame(const struct Audiostreamer * const as)
{
	if (!as) {
		printf("%s\n", strerror(EINVAL));
		return -1;
	}

	// Read an encoded frame as a packet.

	AVPacket input_pkt;
	memset(&input_pkt, 0, sizeof(AVPacket));

	if (av_read_frame(as->input->format_ctx, &input_pkt) != 0) {
		// EOF.
		return 0;
	}


	// Send encoded packet to the input's decoder.

	if (avcodec_send_packet(as->input->codec_ctx, &input_pkt) != 0) {
		printf("send_packet failed\n");
		av_packet_unref(&input_pkt);
		return -1;
	}

	av_packet_unref(&input_pkt);

	return __decode_and_store_samples(as->input, as->output, as->af);
}

// Read a decoded frame out of the input's decoder. Convert the samples and
// store them in the FIFO.
//
// Prereq: We must either have sent an encoded packet to the decoder, or be in
// draining mode.
//
// Returns:
// -1 if error
// 1 if frame read and stored
// 0 if EOF/EAGAIN
static int
__decode_and_store_samples(const struct Input * const input,
		const struct Output * const output, AVAudioFifo * const af)
{
	// Get decoded data out as a frame.

	AVFrame * input_frame = av_frame_alloc();
	if (!input_frame) {
		printf("av_frame_alloc\n");
		return -1;
	}

	const int error = avcodec_receive_frame(input->codec_ctx, input_frame);
	if (error != 0) {
		if (error == AVERROR(EAGAIN) || error == AVERROR_EOF) {
			av_frame_free(&input_frame);
			return 0;
		}

		printf("avcodec_receive_frame failed: %s\n", __get_error_string(error));
		av_frame_free(&input_frame);
		return -1;
	}


	// Convert the samples in the frame.

	const uint8_t * * const raw_samples = __copy_samples(
			input_frame->extended_data, output->codec_ctx->channels);
	if (!raw_samples) {
		av_frame_free(&input_frame);
		return -1;
	}

	uint8_t * * const converted_input_samples = calloc(
			(size_t) output->codec_ctx->channels, sizeof(uint8_t *));
	if (!converted_input_samples) {
		printf("%s\n", strerror(errno));
		av_frame_free(&input_frame);
		free(raw_samples);
		return -1;
	}

	if (av_samples_alloc(converted_input_samples, NULL,
				output->codec_ctx->channels, input_frame->nb_samples,
				output->codec_ctx->sample_fmt, 0) < 0) {
		printf("av_samples_alloc\n");
		av_frame_free(&input_frame);
		free(converted_input_samples);
		free(raw_samples);
		return -1;
	}

	if (swr_convert(output->resample_ctx, converted_input_samples,
				input_frame->nb_samples, raw_samples, input_frame->nb_samples) < 0) {
		printf("swr_convert\n");
		av_frame_free(&input_frame);
		free(raw_samples);
		av_freep(&converted_input_samples[0]);
		free(converted_input_samples);
		return -1;
	}

	free(raw_samples);


	// Add the samples to the fifo.

	// Resize fifo so it can contain old and new samples.

	if (av_audio_fifo_size(af) > INT_MAX - input_frame->nb_samples) {
		printf("overflow\n");
		av_frame_free(&input_frame);
		av_freep(&converted_input_samples[0]);
		free(converted_input_samples);
		return -1;
	}

	if (av_audio_fifo_realloc(af,
				av_audio_fifo_size(af)+input_frame->nb_samples) != 0) {
		printf("unable to resize fifo\n");
		av_frame_free(&input_frame);
		av_freep(&converted_input_samples[0]);
		free(converted_input_samples);
		return -1;
	}

	if (av_audio_fifo_write(af, (void * *) converted_input_samples,
				input_frame->nb_samples) != input_frame->nb_samples) {
		printf("could not write all samples to fifo\n");
		av_frame_free(&input_frame);
		av_freep(&converted_input_samples[0]);
		free(converted_input_samples);
		return -1;
	}

	av_frame_free(&input_frame);
	av_freep(&converted_input_samples[0]);
	free(converted_input_samples);

	return 1;
}

// Turn uint8_t * * samples into const uint8_t * * samples. Because for input
// samples, that is what swr_convert() wants as a parameter.
static const uint8_t * *
__copy_samples(uint8_t * * const src, const int nb_channels)
{
	if (!src) {
		printf("%s\n", strerror(EINVAL));
		return NULL;
	}

	const uint8_t * * const dst = calloc((size_t) nb_channels, sizeof(uint8_t *));
	if (!dst) {
		printf("%s\n", strerror(errno));
		return NULL;
	}

	for (int i = 0; i < nb_channels; i++) {
		dst[i] = src[i];
	}

	return dst;
}

// Take samples from the FIFO, encode them, and write them to the encoder. We
// try to pull out a fully encoded frame from the encoder, which may or may not
// succeed, depending on whether there is sufficient data present. If there is,
// we write the frame to the output.
//
// We update the pts.
//
// Return values:
// > 0 if we write a frame. This will be the frame's size (compressed size).
// 0 if we do not write a frame (this is not an error)
// -1 if error
static int
__encode_and_write_frame(struct Audiostreamer * const as)
{
	if (!as) {
		printf("%s\n", strerror(EINVAL));
		return -1;
	}

	// Get frame out of fifo.

	AVFrame * output_frame = av_frame_alloc();
	if (!output_frame) {
		printf("unable to allocate output frame\n");
		return -1;
	}

	output_frame->nb_samples     = as->output->codec_ctx->frame_size;
	output_frame->channel_layout = as->output->codec_ctx->channel_layout;
	output_frame->format         = as->output->codec_ctx->sample_fmt;
	output_frame->sample_rate    = as->output->codec_ctx->sample_rate;

	if (av_frame_get_buffer(output_frame, 0) < 0) {
		printf("unable to allocate output frame buffer\n");
		av_frame_free(&output_frame);
		return -1;
	}

	if (av_audio_fifo_read(as->af, (void * *) output_frame->data,
				as->output->codec_ctx->frame_size) < as->output->codec_ctx->frame_size) {
		printf("short read from fifo\n");
		av_frame_free(&output_frame);
		return -1;
	}

	output_frame->pts = as->pts;

	if (as->pts > INT64_MAX - output_frame->nb_samples) {
		printf("overflow\n");
		av_frame_free(&output_frame);
		return -1;
	}

	// PTS can tell us how many seconds we've decoded/encoded. It tells us how
	// many samples we've processed. Since we know how many samples make up a
	// second (sample rate is samples per second), we can tell how many seconds
	// we've output by dividing pts by input->codec_ctx->sample_rate.
	as->pts += output_frame->nb_samples;


	// Send the raw frame to the encoder.
	const int error = avcodec_send_frame(as->output->codec_ctx, output_frame);
	if (error != 0) {
		printf("avcodec_send_frame failed: %s\n", __get_error_string(error));
		av_frame_free(&output_frame);
		return -1;
	}

	av_frame_free(&output_frame);

	return __read_and_write_packet(as);
}

// Read an encoded packet from output encoder. Write it out as a packet.
//
// Prereq: We must either have sent a raw frame to the encoder, or be in
// draining mode.
//
// Returns:
// -1 if error
// > 0 if we write a frame. This will be the frame's size (compressed size).
// 0 if we need to try again with more frames/samples before we can encode a
//   packet (EAGAIN) or we're done (EOF).
static int
__read_and_write_packet(struct Audiostreamer * const as)
{
	if (!as) {
		printf("%s\n", strerror(EINVAL));
		return -1;
	}

	// Read encoded data from the encoder.

	AVPacket output_pkt;
	memset(&output_pkt, 0, sizeof(AVPacket));

	const int error = avcodec_receive_packet(as->output->codec_ctx, &output_pkt);
	if (error != 0) {
		// We expect that we will not always have enough data to get a fully encoded
		// frame out.
		if (error == AVERROR(EAGAIN)) {
			return 0;
		}

		// In draining mode we never get EAGAIN, but we get EOF.
		if (error == AVERROR_EOF) {
			return 0;
		}

		printf("avcodec_receive_packet failed: %s\n", __get_error_string(error));
		return -1;
	}

	// We now have a compressed, encoded frame. This frame is in a packet. We can
	// tell its compressed size: output_pkt.size.
	const int sz = output_pkt.size;

	// Write encoded data packet out using av_write_frame().
	if (av_write_frame(as->output->format_ctx, &output_pkt) < 0) {
		printf("av_write_frame failed\n");
		av_packet_unref(&output_pkt);
		return -1;
	}

	av_packet_unref(&output_pkt);

	return sz;
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

// We're at EOF. However, we still have work to finish up. First, there may be
// samples still in the FIFO. Second, we need to drain the codecs. As
// described in avcodec.h, the codecs may buffer for one reason or another.
//
// To drain them, we put each in draining mode. This is done by sending NULL
// to avcodec_send_packet() (for decoding) and to avcodec_send_frame() for
// encoding. Then we need to call avcodec_receive_frame() (decoding) or
// avcodec_receive_packet() (encoding) repeatedly until we hit
// AVERROR(EAGAIN).
static bool
__drain_codecs(struct Audiostreamer * const as)
{
	if (!as) {
		printf("%s\n", strerror(EINVAL));
		return false;
	}

	// Enter draining mode for decoder.
	if (avcodec_send_packet(as->input->codec_ctx, NULL) != 0) {
		printf("send_packet failed (draining mode)\n");
		return false;
	}

	// Drain the decoder. All frames/samples end up in the FIFO.
	while (1) {
		const int res = __decode_and_store_samples(as->input, as->output, as->af);
		if (res == -1) {
			return false;
		}

		// Decoder said EOF.
		if (res == 0) {
			break;
		}
	}

	// Enter draining mode for encoder.
	if (avcodec_send_frame(as->output->codec_ctx, NULL) != 0) {
		printf("send_frame failed (draining mode)\n");
		return false;
	}

	while (1) {
		const int res = __read_and_write_packet(as);
		if (res == -1) {
			return false;
		}

		// Encoder said EOF.
		if (res == 0) {
			break;
		}
	}

	return true;
}
