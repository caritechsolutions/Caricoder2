/*
 * caricoder3 - a thumbnail of the channel's picture every few seconds
 * Copyright (c) 2026 CariTech Solutions
 *
 * The channel writes a small JPEG of its video to a file (--thumb PATH)
 * every `interval` seconds; the supervisor serves it, and the GUI shows
 * it on the channel list and page. On a transcoding channel the picture
 * is one the decoder made anyway. On a channel that copies its video,
 * one key frame every interval is decoded on the CPU for the purpose.
 */
#ifndef CC3_THUMB_H
#define CC3_THUMB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cc3.h"
#include "codec.h"

/* Turns thumbnails on: the file to write and how often (seconds). */
void thumb_init(const char *path, int interval_s);
bool thumb_enabled(void);

/* A decoded picture, from whoever has one. Cheap when not due. */
void thumb_offer(const struct vframe *f);

/* A copied video stream's unit (PES), with its coding: the tap decodes
 * one key frame when a thumbnail is due. */
void thumb_tap_unit(const struct es_unit *u, enum codec_id codec);

/* Baseline JPEG of an 8-bit yuv420p picture; returns the byte count
 * (malloc'd into *out), 0 on failure. quality 1..100. */
size_t thumb_jpeg(const uint8_t *y, int ys, const uint8_t *u, int us, const uint8_t *v, int vs,
                  int width, int height, int quality, uint8_t **out);

#endif
