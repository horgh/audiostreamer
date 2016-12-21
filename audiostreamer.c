//
//

#include <errno.h>
//#include <libavcodec/audio_frame_queue.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libswresample/swresample.h>
#include <stdio.h>
#include <string.h>

int
main(void)
{
	// Set up library.

	// Register muxers, demuxers, and protocols.
	av_register_all();

	// Make formats available. Specifically, pulse (pulseaudio).
	avdevice_register_all();


	// Open input and set up decoder.

	// PulseAudio input format.
	AVInputFormat * input_format = av_find_input_format("pulse");
	if (!input_format) {
		printf("input format not found\n");
		return 1;
	}

	// Input URL.
	const char * const input_url
		= "alsa_output.pci-0000_00_1f.3.analog-stereo.monitor";

	// Open the input stream.
	AVFormatContext * input_format_ctx = NULL;
	if (avformat_open_input(&input_format_ctx, input_url, input_format,
				NULL) != 0) {
		printf("open input failed\n");
		avformat_free_context(input_format_ctx);
		return 1;
	}

	// Read packets to get stream info.
	if (avformat_find_stream_info(input_format_ctx, NULL) < 0) {
		printf("failed to find stream info\n");
		avformat_close_input(&input_format_ctx);
		avformat_free_context(input_format_ctx);
		return 1;
	}

	// Dump info about the input format.
	av_dump_format(input_format_ctx, 0, input_url, 0);

	// Find codec for the input stream.
	AVCodec * input_codec = avcodec_find_decoder(
			input_format_ctx->streams[0]->codecpar->codec_id);
	if (!input_codec) {
		printf("codec not found\n");
		return 1;
	}

	AVCodecContext * input_codec_ctx = avcodec_alloc_context3(input_codec);
	if (!input_codec_ctx) {
		printf("could not allocate codec context\n");
		return 1;
	}

	// Apparently we need to set some attributes manually
	// http://stackoverflow.com/questions/39461643/avcodec-open2-pcm-channels-out-of-bounds
	// TODO: Confirm what these values should be.
	//input_codec_ctx->sample_rate = 8000;
	//input_codec_ctx->sample_fmt = AV_SAMPLE_FMT_S16;
	//input_codec_ctx->channels = 2;
	//input_codec_ctx->channel_layout = AV_CH_LAYOUT_STEREO;

	if (avcodec_parameters_to_context(input_codec_ctx,
				input_format_ctx->streams[0]->codecpar) < 0) {
		printf("unable to initialize input codec parameters\n");
		return 1;
	}

	// Initialize the codec context to use the codec.
	if (avcodec_open2(input_codec_ctx, input_codec, NULL) != 0) {
		printf("unable to initialize codec context\n");
		return 1;
	}


	// Input is open and decoder is ready. Now set up output and output encoder
	// (muxer).

	// Set up muxing context.
	// AVFormatContext is used for muxing (as well as demuxing).
	// In addition to allocating the context, this sets up the output format which
	// sets which muxer to use.
	AVFormatContext * output_format_ctx = NULL;
	const char * const output_fmt = "mp3";
	//const char * const output_fmt = "oga";
	if (avformat_alloc_output_context2(&output_format_ctx, NULL, output_fmt,
				NULL) < 0) {
		printf("unable to allocate AVFormatContext\n");
		return 1;
	}

	// Open IO context - open output file.
	// stdout
	//const char * const output_url = "pipe:1";
	const char * const output_url = "file:out.mp3";
	if (avio_open(&output_format_ctx->pb, output_url, AVIO_FLAG_WRITE) < 0) {
		printf("unable to open output\n");
		return 1;
	}

	// Create output stream.

	const char * const output_encoder = "libmp3lame";
	//const char * const output_encoder = "libvorbis";
	AVCodec * output_codec = avcodec_find_encoder_by_name(output_encoder);
	if (!output_codec) {
		printf("output codec not found\n");
		return 1;
	}

	// There is a codec member in the stream, but it's deprecated and says we
	// should use its codecpar member instead.
	//AVStream * avs = avformat_new_stream(output_format_ctx, output_codec);
	AVStream * avs = avformat_new_stream(output_format_ctx, NULL);
	if (!avs) {
		printf("unable to add stream\n");
		return 1;
	}

	avs->time_base.den = input_codec_ctx->sample_rate;
	avs->time_base.num = 1;

	printf("added stream\n");

	// Set up output encoder

	AVCodecContext * output_codec_ctx = avcodec_alloc_context3(output_codec);
	if (!output_codec_ctx) {
		printf("unable to allocate output codec context\n");
		return 1;
	}

	output_codec_ctx->channels       = input_codec_ctx->channels;
	output_codec_ctx->channel_layout = AV_CH_LAYOUT_STEREO;
	output_codec_ctx->sample_rate    = input_codec_ctx->sample_rate;
	output_codec_ctx->sample_fmt     = output_codec->sample_fmts[0];
	//output_codec_ctx->sample_fmt     = output_codec->sample_fmts[2];
	output_codec_ctx->bit_rate = 96000;

	//if (avcodec_parameters_to_context(output_codec_ctx, avs->codecpar) < 0) {
	//	printf("unable to initialize output codec parameters\n");
	//	return 1;
	//}

	printf("output codec ctx %d channels\n", output_codec_ctx->channels);

	// Initialize the codec context to use the codec.
	if (avcodec_open2(output_codec_ctx, output_codec, NULL) != 0) {
		printf("unable to initialize output codec context to use codec\n");
		return 1;
	}

	// Set AVStream.codecpar (stream codec parameters).
	if (avcodec_parameters_from_context(avs->codecpar, output_codec_ctx) < 0) {
		printf("unable to set output codec parameters\n");
		return 1;
	}

	// Initialize the codec context to use the codec.
	//if (avcodec_open2(output_codec_ctx, output_codec, NULL) != 0) {
	//	printf("unable to initialize output codec context to use codec\n");
	//	return 1;
	//}

	if (avformat_write_header(output_format_ctx, NULL) < 0) {
		printf("unable to write header\n");
		return 1;
	}

	if (!avcodec_is_open(output_codec_ctx)) {
		printf("codec is not open\n");
		return 1;
	}

	if (!av_codec_is_encoder(output_codec_ctx->codec)) {
		printf("codec is not an encoder\n");
		return 1;
	}

	if (output_codec_ctx->codec->receive_packet) {
		printf("output codec context codec receive packet is set\n");
	}

	if (output_codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
		printf("output codec type is video\n");
	} else if (output_codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
		printf("output codec type is audio \n");
	} else {
		printf("unknown output codec type\n");
		return 1;
	}


	// Set up resampler

	// To be able to convert audio sample formats, we need a resampler.
	// See transcode_aac.c
	SwrContext * resample_ctx = swr_alloc_set_opts(
			NULL,
			av_get_default_channel_layout(output_codec_ctx->channels),
			output_codec_ctx->sample_fmt,
			output_codec_ctx->sample_rate,
			av_get_default_channel_layout(input_codec_ctx->channels),
			input_codec_ctx->sample_fmt,
			input_codec_ctx->sample_rate,
			0,
			NULL);
	if (!resample_ctx) {
		printf("unable to allocate resample context\n");
		return 1;
	}

	if (swr_init(resample_ctx) < 0) {
		printf("unable to open resample context\n");
		return 1;
	}

	// Apparently this being different can cause issues. See transcode_aac.c
	if (output_codec_ctx->sample_rate != input_codec_ctx->sample_rate) {
		printf("sample rate differs\n");
	}


	// Begin read, decode, encode, write process.

	// We repeatedly read in data (av_read_frame()). This gives us an AVPacket
	// with encoded data. We decode each packet with avcodec_send_packet(). Then
	// we get the decoded data out using avcodec_receive_frame().

	// Writing: avformat_write_header() to write the file header, then
	// av_write_frame() repeatedly to write each encoded packet, and finally
	// av_write_trailer() to finalize the file.
	//
	// For some more info on this, read top explanatory comment in avcodec.h.
	// avformat.h also has some information.

	// AVPacket holds encoded data.
	AVPacket input_pkt;
	memset(&input_pkt, 0, sizeof(AVPacket));

	// AVFrame holds decoded data.
	AVFrame * input_frame = av_frame_alloc();
	if (!input_frame) {
		printf("av_frame_alloc\n");
		return 1;
	}

	// Encode to this packet.
	AVPacket output_pkt;
	memset(&output_pkt, 0, sizeof(AVPacket));

	// The number of samples read in a frame can be larger or smaller than what
	// the encoder wants. We need to give it the exact number of frames it wants.
	//
	// This means we can't simply feed a single frame from the input directly to
	// the encoder at a time.
	//
	// We use a AvAudioFifo for this. Note AudioFrameQueue looks like something
	// similar but is apparently not available in my version of ffmpeg. Also, it
	// appears to not hold raw data either, so I'm not sure how to use it.

	// We must have initial allocation size of at least 1.
	AVAudioFifo * af = av_audio_fifo_alloc(output_codec_ctx->sample_fmt,
			output_codec_ctx->channels, 1);
	if (!af) {
		printf("unable to allocate audio fifo\n");
		return 1;
	}

	int frames_written = 0;

	// Global presentation timestamp (PTS). This needs to increase for each sample
	// we output.
	int64_t pts = 1;

	// output_codec_context->frame_size tells us how many samples the output
	// encoder takes.
	printf("output codec frame size %d\n", output_codec_ctx->frame_size);

	while (1) {
		// Do we need to read & decode another frame from the input?
		const int available_samples = av_audio_fifo_size(af);
		if (available_samples < output_codec_ctx->frame_size) {
			// Read an encoded frame as a packet.
			if (av_read_frame(input_format_ctx, &input_pkt) != 0) {
				printf("unable to read frame\n");
				// This happens at EOF.
				break;
			}

			printf("read packet\n");

			// Decode the packet.
			if (avcodec_send_packet(input_codec_ctx, &input_pkt) != 0) {
				printf("send_packet_failed\n");
				return 1;
			}

			// Get decoded data out as a frame.
			if (avcodec_receive_frame(input_codec_ctx, input_frame) != 0) {
				printf("avcodec_receive_frame failed\n");
				return 1;
			}

			av_packet_unref(&input_pkt);

			printf("input_frame has %d samples\n", input_frame->nb_samples);


			// Convert the samples

			uint8_t * * converted_input_samples = calloc(
					(size_t) output_codec_ctx->channels, sizeof(uint8_t *));
			if (!converted_input_samples) {
				printf("%s\n", strerror(errno));
				return 1;
			}

			if (av_samples_alloc(converted_input_samples, NULL,
						output_codec_ctx->channels, input_frame->nb_samples,
						output_codec_ctx->sample_fmt, 0) < 0) {
				printf("av_samples_alloc\n");
				return 1;
			}

			//// swr_convert() wants input to be const uint8_t * *... Copy so that is
			//// the case.
			//const uint8_t * * raw_input_samples = calloc(
			//		(size_t) output_codec_ctx->channels, sizeof(uint8_t *));
			//if (!raw_input_samples) {
			//	printf("%s\n", strerror(errno));
			//	return 1;
			//}

			//if (av_samples_alloc((uint8_t * *) raw_input_samples, NULL,
			//			output_codec_ctx->channels, input_frame->nb_samples,
			//			output_codec_ctx->sample_fmt, 0) < 0) {
			//	printf("av_samples_alloc\n");
			//	return 1;
			//}

			//if (av_samples_copy(raw_input_samples, input_frame->extended_data,
			//			0, 0, input_frame->nb_samples, output_codec_ctx->channels,
			//			output_codec_ctx->sample_fmt) < 0) {
			//	printf("av_samples_copy\n");
			//	return 1;
			//}


			//const uint8_t * const * const input_data0 = (const uint8_t * const * const) input_frame->extended_data;
			//const uint8_t * const * input_data1 = input_data0;
			//const uint8_t * * input_data = (const uint8_t * *) input_data1;
			//const uint8_t * * input_data = (const uint8_t * *) input_frame->extended_data;

			if (swr_convert(resample_ctx, converted_input_samples,
						input_frame->nb_samples,
						(uint8_t * *) input_frame->extended_data,
						//input_data,
						//raw_input_samples,
						input_frame->nb_samples) < 0) {
				printf("swr_convert\n");
				return 1;
			}



			// Add frame's samples to the fifo.

			// Resize fifo so it can contain old and new samples.
			if (av_audio_fifo_realloc(af,
						av_audio_fifo_size(af)+input_frame->nb_samples) != 0) {
				printf("unable to resize fifo\n");
				return 1;
			}

			// Note: I had segfault here prior to trying any resampling. May be index
			// 1 in extended_data being null.
			//if (av_audio_fifo_write(af, (void * *) input_frame->extended_data,
			if (av_audio_fifo_write(af, (void * *) converted_input_samples,
						input_frame->nb_samples) != input_frame->nb_samples) {
				printf("could not write all samples to fifo\n");
				return 1;
			}

			av_frame_unref(input_frame);
			continue;
		}

		// We have enough samples to encode and write.

		// Get frame out of fifo.

		// Encode with this frame.
		AVFrame * output_frame = av_frame_alloc();
		if (!output_frame) {
			printf("unable to allocate output frame\n");
			return 1;
		}

		output_frame->nb_samples     = output_codec_ctx->frame_size;
		output_frame->channel_layout = output_codec_ctx->channel_layout;
		output_frame->format         = output_codec_ctx->sample_fmt;
		output_frame->sample_rate    = output_codec_ctx->sample_rate;

		if (av_frame_get_buffer(output_frame, 0) < 0) {
			printf("unable to allocate output frame buffer\n");
			return 1;
		}

		if (av_audio_fifo_read(af, (void * *) output_frame->data,
		//if (av_audio_fifo_read(af, (void * *) output_frame->extended_data,
					output_codec_ctx->frame_size) < output_codec_ctx->frame_size) {
				printf("short read from fifo\n");
				return 1;
		}

		output_frame->pts = pts;
		pts += output_frame->nb_samples;

		// Send the raw frame to the encoder.
		int error = avcodec_send_frame(output_codec_ctx, output_frame);
		if (error != 0) {
			char buf[255];
			av_strerror(error, buf, sizeof(buf));
			printf("avcodec_send_frame failed: %s\n", buf);
			return 1;
		}

		av_frame_unref(output_frame);
		printf("sent raw frame to output encoder\n");

		// Read encoded data from the encoder.
		// Get encoded data out as a packet.
		error = avcodec_receive_packet(output_codec_ctx, &output_pkt);
		if (error != 0) {
			// Argh, I thought it was sending EINVAL for some reason.
			if (error == AVERROR(EAGAIN)) {
				printf("EAGAIN\n");
				continue;
			}

			char buf[255];
			av_strerror(error, buf, sizeof(buf));
			// EINVAL means: codec not opened, or encoder error
			printf("avcodec_receive_packet failed: %s\n", buf);
			return 1;
		}

		// Then write encoded data packet out using av_write_frame()
		if (av_write_frame(output_format_ctx, &output_pkt) < 0) {
			printf("av_write_frame failed\n");
			return 1;
		}

		av_packet_unref(&output_pkt);

		frames_written++;

		if (frames_written == 600) {
			break;
		}
	}

	if (av_write_trailer(output_format_ctx) != 0) {
		printf("unable to write trailer\n");
		return 1;
	}

	avformat_close_input(&input_format_ctx);
	avformat_free_context(input_format_ctx);
	av_frame_free(&input_frame);

	return 0;
}
