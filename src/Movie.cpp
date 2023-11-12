/*
 * Umgebung
 *
 * This file is part of the *Umgebung* library (https://github.com/dennisppaul/umgebung).
 * Copyright (c) 2023 Dennis P Paul.
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Movie.h"

#if !defined(DISABLE_GRAPHICS) && !defined(DISABLE_VIDEO)

#include "Umgebung.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

Movie::Movie(const std::string &filename, int _channels) : PImage() {
    if (init_from_file(filename, _channels) >= 0) {
        std::cout << "+++ Movie: width: " << width << ", height: " << height << ", channels: " << channels << std::endl;
    } else {
        std::cerr << "+++ Movie - ERROR: could not initialize from file" << std::endl;
    }
}

int Movie::init_from_file(const std::string &filename, int _channels) {
    // Open the input file
    formatContext = avformat_alloc_context();
    if (avformat_open_input(&formatContext, filename.c_str(), NULL, NULL) != 0) {
        fprintf(stderr, "Could not open file %s\n", filename.c_str());
        return -1;
    }

    // Retrieve stream information
    if (avformat_find_stream_info(formatContext, NULL) < 0) {
        fprintf(stderr, "Could not find stream information\n");
        return -1;
    }

    // Find the first video stream
    videoStream = -1;
    for (unsigned int i = 0; i < formatContext->nb_streams; i++) {
        if (formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStream = i;
            break;
        }
    }

    if (videoStream == -1) {
        fprintf(stderr, "Could not find a video stream\n");
        return -1;
    }

    // Get a pointer to the codec context for the video stream
    AVCodecParameters *codecParameters = formatContext->streams[videoStream]->codecpar;
    const AVCodec     *codec           = avcodec_find_decoder(codecParameters->codec_id);
    codecContext = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codecContext, codecParameters);
    if (avcodec_open2(codecContext, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        return -1;
    }

    // retrieve movie framerate
    AVRational frame_rate     = formatContext->streams[videoStream]->avg_frame_rate;
    double     frame_duration = 1.0 / (frame_rate.num / (double) frame_rate.den);
    fprintf(stdout, "+++ Movie: framerate     : %i\n", frame_rate.num / frame_rate.den);
    fprintf(stdout, "+++ Movie: frame duration: %f\n", frame_duration);

    // Determine the pixel format and number of channels based on input file
    AVPixelFormat            src_pix_fmt = codecContext->pix_fmt;
    AVPixelFormat            dst_pix_fmt;
    const AVPixFmtDescriptor *desc       = av_pix_fmt_desc_get(src_pix_fmt);
    if (_channels < 0) {
        _channels   = 4;
        dst_pix_fmt = AV_PIX_FMT_RGBA;
//        std::cout << "not looking for format. defaulting to RGBA ( 4 channels )" << std::endl;
    } else if (desc && desc->nb_components == 4) {
        _channels   = 4;
        dst_pix_fmt = AV_PIX_FMT_RGBA;
//        std::cout << "found RGBA video" << std::endl;
    } else {
        _channels   = 3;
        dst_pix_fmt = AV_PIX_FMT_RGB24;
//        std::cout << "found RGB video" << std::endl;
    }

    // Create a sws context for the conversion
    swsContext = sws_getContext(
            codecContext->width, codecContext->height, src_pix_fmt,
            codecContext->width, codecContext->height, dst_pix_fmt,
            SWS_FAST_BILINEAR,
//            SWS_BICUBIC,
            NULL, NULL, NULL);

    if (!swsContext) {
        fprintf(stderr, "Failed to create SwScale context\n");
        return -1;
    }

    // Allocate an AVFrame structure
    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Failed to allocate frame\n");
        return -1;
    }

    // Allocate an AVFrame structure for the converted frame
    convertedFrame = av_frame_alloc();
    if (!convertedFrame) {
        fprintf(stderr, "Failed to allocate converted frame\n");
        return -1;
    }

    int numBytes = av_image_get_buffer_size(dst_pix_fmt,
                                            codecContext->width,
                                            codecContext->height,
                                            1);
    buffer = (uint8_t *) av_malloc(numBytes * sizeof(uint8_t));
    av_image_fill_arrays(convertedFrame->data,
                         convertedFrame->linesize,
                         buffer,
                         dst_pix_fmt,
                         codecContext->width,
                         codecContext->height,
                         1);
    packet = av_packet_alloc();

    init(codecContext->width, codecContext->height, _channels, convertedFrame->data[0]);
    return 1;
}

Movie::~Movie() {
    av_freep(&buffer);
    av_frame_free(&frame);
    av_frame_free(&convertedFrame);
    avcodec_close(codecContext);
    avcodec_free_context(&codecContext);
    avformat_close_input(&formatContext);
    avformat_free_context(formatContext);
    av_packet_free(&packet);
    sws_freeContext(swsContext);
}

bool Movie::available() {
    bool mAvailable = false;
    if (av_read_frame(formatContext, packet) >= 0) {
        if (packet->stream_index == videoStream) {
            avcodec_send_packet(codecContext, packet);
            int ret = avcodec_receive_frame(codecContext, frame);
            if (ret == 0) {
                // Successfully received a frame
                mFrameCounter++;
                mAvailable = true;
                // TODO update texture ... move this to `read()`?
                // convert data to RGBA
                sws_scale(swsContext, frame->data, frame->linesize, 0, frame->height, convertedFrame->data, convertedFrame->linesize);
                // SDL_UpdateTexture(texture, NULL, convertedFrame->data[0], convertedFrame->linesize[0]);
            } else if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                fprintf(stdout, "No more frames : %i\n", ret); // TODO remove this at some point
                // No more frames
                mAvailable = false;
            } else {
                fprintf(stderr, "Error receiving frame: %s\n", av_err2str(ret));  // TODO remove this at some point
                mAvailable = false;
            }
        }
        av_frame_unref(frame);
//        av_frame_unref(convertedFrame); // TODO this creates `bad dst image pointers`
        av_packet_unref(packet);
    }
    return mAvailable;
}

void Movie::read() {
    GLint mFormat;
    if (channels == 4) {
        mFormat = GL_RGBA;
    } else {
        mFormat = GL_RGB;
    }
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 mFormat,
                 width, height,
                 0,
                 mFormat,
                 GL_UNSIGNED_BYTE,
                 convertedFrame->data[0]);
}

#else

Movie::Movie(const std::string &filename, int _channels) : PImage() {
    std::cerr << "Movie - ERROR: video is disabled" << std::endl;
}

Movie::~Movie() {}

bool Movie::available() { return false; }

void Movie::read() {}

int Movie::init_from_file(const std::string &filename, int _channels) { return -1; }

#endif // DISABLE_GRAPHICS && DISABLE_VIDEO