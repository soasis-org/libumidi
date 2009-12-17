/*-
 * Copyright (c) 2006-2009 Hans Petter Selasky. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/uio.h>
#include <sys/file.h>
#include <sys/signal.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/filio.h>

#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "umidi20.h"

/* prototypes */

static void umidi20_uninit(void);
static void *umidi20_watchdog_alloc(void *arg);

static void *umidi20_watchdog_play_rec(void *arg);

static void umidi20_watchdog_record_sub(struct umidi20_device *dev, struct umidi20_device *play_dev, uint32_t position, uint32_t effects);
static void umidi20_watchdog_play_sub(struct umidi20_device *dev, uint32_t position);
static void umidi20_watchdog_play_sub_check_key(struct umidi20_device *dev, struct umidi20_event *event);
static void *umidi20_watchdog_files(void *arg);
static void umidi20_stop_thread(pthread_t *p_td, pthread_mutex_t *mtx);
static void *umidi20_watchdog_song(void *arg);

/* structures */

struct umidi20_root_device root_dev;

/* functions */

uint32_t
umidi20_get_curr_position(void)
{
	uint32_t position;

	pthread_mutex_lock(&(root_dev.mutex));
	position = root_dev.curr_position;
	pthread_mutex_unlock(&(root_dev.mutex));

	return (position);
}

void
umidi20_set_record_event_callback(uint8_t device_no, umidi20_event_callback_t *func, void *arg)
{
	if (device_no >= UMIDI20_N_DEVICES)
		return;

	root_dev.rec[device_no].event_callback_func = func;
	root_dev.rec[device_no].event_callback_arg = arg;
}

void
umidi20_set_play_event_callback(uint8_t device_no, umidi20_event_callback_t *func, void *arg)
{
	if (device_no >= UMIDI20_N_DEVICES)
		return;

	root_dev.play[device_no].event_callback_func = func;
	root_dev.play[device_no].event_callback_arg = arg;
}

void
umidi20_init(void)
{
	uint32_t x;

	umidi20_mutex_init(&(root_dev.mutex));

	umidi20_gettime(&(root_dev.curr_time));

	root_dev.start_time = root_dev.curr_time;
	root_dev.curr_position = 0;

	for (x = 0; x < UMIDI20_N_DEVICES; x++) {
		root_dev.rec[x].file_no = -1;
		root_dev.rec[x].device_no = x;
		root_dev.rec[x].update = 1;
		snprintf(root_dev.rec[x].fname,
		    sizeof(root_dev.rec[x].fname),
		    "/dev/umidi0.%x", x);

		root_dev.play[x].file_no = -1;
		root_dev.play[x].device_no = x;
		root_dev.play[x].update = 1;
		snprintf(root_dev.play[x].fname,
		    sizeof(root_dev.play[x].fname),
		    "/dev/umidi0.%x", x);
	}

	if (pthread_create(&(root_dev.thread_alloc), NULL,
	    &umidi20_watchdog_alloc, NULL)) {
		root_dev.thread_alloc = NULL;
	}
	if (pthread_create(&(root_dev.thread_play_rec), NULL,
	    &umidi20_watchdog_play_rec, NULL)) {
		root_dev.thread_play_rec = NULL;
	}
	if (pthread_create(&(root_dev.thread_files), NULL,
	    &umidi20_watchdog_files, NULL)) {
		root_dev.thread_files = NULL;
	}
	atexit(&umidi20_uninit);

	return;
}

static void
umidi20_uninit(void)
{
	pthread_mutex_lock(&(root_dev.mutex));

	umidi20_stop_thread(&(root_dev.thread_alloc),
	    &(root_dev.mutex));

	umidi20_stop_thread(&(root_dev.thread_play_rec),
	    &(root_dev.mutex));

	pthread_mutex_unlock(&(root_dev.mutex));
	return;
}

static void
umidi20_stop_thread(pthread_t *p_td, pthread_mutex_t *p_mtx)
{
	pthread_t td;
	uint8_t recurse = 0;

	pthread_mutex_assert(p_mtx, MA_OWNED);

	td = *p_td;
	*p_td = NULL;

	if (td) {
		while (pthread_mutex_unlock(p_mtx) == 0) {
			recurse++;
		}

		pthread_kill(td, SIGURG);
		pthread_join(td, NULL);

		while (recurse--) {
			pthread_mutex_lock(p_mtx);
		}
	}
	return;
}

static void *
umidi20_watchdog_alloc(void *arg)
{
	struct umidi20_event *event;

	pthread_mutex_lock(&(root_dev.mutex));

	while (root_dev.thread_alloc) {

		while (UMIDI20_IF_QLEN(&(root_dev.free_queue)) < UMIDI20_BUF_EVENTS) {
			pthread_mutex_unlock(&(root_dev.mutex));
			event = umidi20_event_alloc(NULL, 0);
			pthread_mutex_lock(&(root_dev.mutex));
			if (event) {
				UMIDI20_IF_ENQUEUE_LAST(&(root_dev.free_queue), event);
			} else {
				break;
			}
		}
		pthread_mutex_unlock(&(root_dev.mutex));

		while (usleep(100000));

		pthread_mutex_lock(&(root_dev.mutex));
	}

	pthread_mutex_unlock(&(root_dev.mutex));

	return NULL;
}

static void *
umidi20_watchdog_play_rec(void *arg)
{
	struct timespec ts = {0, 0};
	uint32_t position;
	uint32_t x;

	pthread_mutex_lock(&(root_dev.mutex));

	while (root_dev.thread_play_rec) {

		umidi20_gettime(&ts);

		root_dev.curr_time = ts;

		position = umidi20_difftime
		    (&(root_dev.curr_time), &(root_dev.start_time));

		root_dev.curr_position = position;

		for (x = 0; x < UMIDI20_N_DEVICES; x++) {
			umidi20_watchdog_record_sub(&(root_dev.rec[x]), &(root_dev.play[x]),
			    position, root_dev.effects);
		}

		for (x = 0; x < UMIDI20_N_DEVICES; x++) {
			umidi20_watchdog_play_sub(&(root_dev.play[x]), position);
		}

		pthread_mutex_unlock(&(root_dev.mutex));

		while (usleep(1000));

		pthread_mutex_lock(&(root_dev.mutex));
	}

	pthread_mutex_unlock(&(root_dev.mutex));

	return NULL;
}

static void
umidi20_watchdog_record_sub(struct umidi20_device *dev,
    struct umidi20_device *play_dev,
    uint32_t curr_position, uint32_t effect)
{
	struct umidi20_event *event;
	struct umidi20_event *event_copy;
	uint8_t cmd;
	uint8_t drop;

	curr_position -= dev->start_position;

	if (curr_position >= dev->end_offset) {
		/* time overflow */
		if (dev->enabled_usr) {
			DEBUG("time overflow\n");
			dev->enabled_usr = 0;
		}
	}
	/* record */

	while (dev->file_no >= 0) {

		/*
	         * read data regularly so that
	         * the buffer-size is kept low,
	         * even if not recording:
	         */
		if (read(dev->file_no, &cmd, 1) != 1) {
			if (errno != EWOULDBLOCK) {
				dev->update = 1;
			}
			break;
		}
		if (dev->enabled_usr == 0) {
			break;
		}
		event = umidi20_convert_to_event(&(dev->conv), cmd, 1);

		if (event == NULL) {
			break;
		}
		DEBUG("pos = %d\n", curr_position);

		event->device_no = dev->device_no;
		event->position = curr_position;

		drop = 0;

		if (dev->event_callback_func != NULL) {

			pthread_mutex_unlock(&(root_dev.mutex));

			(dev->event_callback_func) (dev->device_no,
			    dev->event_callback_arg, event, &drop);

			pthread_mutex_lock(&(root_dev.mutex));
		}
		if (play_dev->enabled_usr) {
			if (effect & UMIDI20_EFFECT_LOOPBACK) {
				event_copy = umidi20_event_copy(event, 1);
				if (event_copy) {
					umidi20_event_queue_insert
					    (&(play_dev->queue), event_copy, UMIDI20_CACHE_INPUT);
				}
			}
		}
		if (drop) {
			umidi20_event_free(event);
		} else {
			umidi20_event_queue_insert
			    (&(dev->queue), event, UMIDI20_CACHE_INPUT);
		}
	}
	return;
}

static void
umidi20_watchdog_play_sub(struct umidi20_device *dev,
    uint32_t curr_position)
{
	struct umidi20_event *event;
	struct umidi20_event *event_root;
	uint32_t delta_position;
	int err;
	uint8_t len;
	uint8_t drop;

	/* playback */

	curr_position -= dev->start_position;

	if (curr_position >= dev->end_offset) {
		/* time overflow */
		if (dev->enabled_usr) {
			DEBUG("time overflow\n");
			dev->enabled_usr = 0;
		}
		return;
	}
	while (1) {

		UMIDI20_IF_POLL_HEAD(&(dev->queue), event_root);
		event = event_root;

		if (event == NULL) {
			break;
		}
		delta_position = (event->position - curr_position);

		if (delta_position >= 0x80000000) {

			drop = 0;

			if (dev->event_callback_func != NULL) {

				pthread_mutex_unlock(&(root_dev.mutex));

				(dev->event_callback_func) (dev->device_no,
				    dev->event_callback_arg, event, &drop);

				pthread_mutex_lock(&(root_dev.mutex));
			}
			if ((dev->file_no >= 0) &&
			    (dev->enabled_usr) &&
			    (event->cmd[1] != 0xFF) &&
			    (!drop)) {

				/* only write non-meta/reset commands */

				do {
					len = umidi20_command_to_len[event->cmd[0] & 0xF];

					/* check key */

					umidi20_watchdog_play_sub_check_key(dev, event);

					/* try to write data */

					err = write(dev->file_no, event->cmd + 1, len);
					if ((err < 0) && (errno != EWOULDBLOCK)) {
						/* try to re-open the device */
						dev->update = 1;
						break;
					} else if (err != len) {
						/* we are done - the queue is full */
						break;
					}
				} while ((event = event->p_next));
			}
			UMIDI20_IF_REMOVE(&(dev->queue), event_root);

			umidi20_event_free(event_root);
		} else {
			break;
		}
	}
	return;
}

static void
umidi20_watchdog_play_sub_check_key(struct umidi20_device *dev,
    struct umidi20_event *event)
{
	uint32_t offset;

	/* check for key start */

	if (umidi20_event_is_key_start(event)) {
		offset = ((128 * (umidi20_event_get_channel(event) & 0xF)) +
		    (umidi20_event_get_key(event) & 0x7F));
		dev->key_on_table[offset / 8] |= (1 << (offset % 8));
	}
	/* check for key end */

	if (umidi20_event_is_key_end(event)) {
		offset = ((128 * (umidi20_event_get_channel(event) & 0xF)) +
		    (umidi20_event_get_key(event) & 0x7F));
		dev->key_on_table[offset / 8] &= ~(1 << (offset % 8));
	}
	return;
}

static void *
umidi20_watchdog_files(void *arg)
{
	struct umidi20_device *dev;
	uint32_t x;
	int file_no;
	int err;

	pthread_mutex_lock(&(root_dev.mutex));

	while (root_dev.thread_files) {

		for (x = 0; x < UMIDI20_N_DEVICES; x++) {

			dev = &(root_dev.play[x]);

			if (dev->update) {

				file_no = dev->file_no;
				dev->file_no = -1;

				if (file_no > 2) {
					close(file_no);
				}
				if (dev->enabled_cfg)
					file_no = open(dev->fname, O_WRONLY | O_NONBLOCK);
				else
					file_no = -1;

				if (file_no >= 0) {
					dev->update = 0;
					dev->file_no = file_no;
				}
			}
			dev = &(root_dev.rec[x]);

			if (dev->update) {

				file_no = dev->file_no;
				dev->file_no = -1;

				if (file_no > 2) {
					close(file_no);
				}
				if (dev->enabled_cfg)
					file_no = open(dev->fname, O_RDONLY | O_NONBLOCK);
				else
					file_no = -1;

				if (file_no >= 0) {

					/* set non-blocking I/O */
					err = 1;
					err = ioctl(file_no, FIONBIO, &err);

					dev->update = 0;
					dev->file_no = file_no;
				}
			}
		}

		pthread_mutex_unlock(&(root_dev.mutex));
		usleep(100000);
		pthread_mutex_lock(&(root_dev.mutex));
	}
	pthread_mutex_unlock(&(root_dev.mutex));
	return NULL;
}

/*
 * flag: 0 - default
 *       1 - use cache
 */
struct umidi20_event *
umidi20_event_alloc(struct umidi20_event ***ppp_next, uint8_t flag)
{
	struct umidi20_event *event = NULL;

	if (flag == 1) {
		pthread_mutex_lock(&(root_dev.mutex));
		UMIDI20_IF_DEQUEUE(&(root_dev.free_queue), event);
		pthread_mutex_unlock(&(root_dev.mutex));
	}
	if (event == NULL) {
		event = malloc(sizeof(*event));
	}
	if (event) {
		bzero(event, sizeof(*event));
		if (ppp_next) {
			**ppp_next = event;
			*ppp_next = &(event->p_next);
		}
	}
	return event;
}

void
umidi20_event_free(struct umidi20_event *event)
{
	struct umidi20_event *p_next;

	while (event) {
		p_next = event->p_next;
		free(event);
		event = p_next;
	}
	return;
}

struct umidi20_event *
umidi20_event_copy(struct umidi20_event *event, uint8_t flag)
{
	struct umidi20_event *p_curr;
	struct umidi20_event *p_next = NULL;
	struct umidi20_event **pp_next = &p_next;

	while (event) {

		p_curr = umidi20_event_alloc(&pp_next, flag);

		if (p_curr == NULL) {
			goto fail;
		}
		/* copy data */

		p_curr->position = event->position;
		p_curr->revision = event->revision;
		p_curr->tick = event->tick;
		p_curr->device_no = event->device_no;
		bcopy(event->cmd, p_curr->cmd, UMIDI20_COMMAND_LEN);

		/* get next event */
		event = event->p_next;
	}
	return p_next;

fail:
	umidi20_event_free(p_next);
	return NULL;
}

struct umidi20_event *
umidi20_event_from_data(const uint8_t *data_ptr,
    uint32_t data_len, uint8_t flag)
{
	struct umidi20_event *p_curr = NULL;
	struct umidi20_event *p_next = NULL;
	struct umidi20_event **pp_next = &p_next;
	uint8_t i;
	uint8_t cont = 0;
	static const uint8_t p0 = 0;
	static const uint8_t s0 = 0;

	if (data_len == 0) {
		goto fail;
	}
	p_curr = umidi20_event_alloc(&pp_next, flag);
	if (p_curr == NULL) {
		goto fail;
	}
	i = 1;
	while (1) {

		if (data_len == 0) {
			p_curr->cmd[0] = (p0 | s0 | (i - 1));
			break;
		}
		if (i == 8) {
			p_curr->cmd[0] = (cont ?
			    (p0 | s0 | 0x8) :
			    (p0 | s0 | 0x0));
			i = 1;
			cont = 1;

			p_curr = umidi20_event_alloc(&pp_next, flag);
			if (p_curr == NULL) {
				goto fail;
			}
		}
		p_curr->cmd[i] = *data_ptr;
		i++;
		data_ptr++;
		data_len--;
	}
	return p_next;

fail:
	umidi20_event_free(p_next);
	return NULL;
}

uint32_t
umidi20_event_get_what(struct umidi20_event *event)
{
	if (event == NULL) {
		return 0;
	}
	switch (event->cmd[1] >> 4) {
	case 0x8:
	case 0x9:
		return (UMIDI20_WHAT_CHANNEL |
		    UMIDI20_WHAT_KEY |
		    UMIDI20_WHAT_VELOCITY);
	case 0xA:
		return (UMIDI20_WHAT_CHANNEL |
		    UMIDI20_WHAT_KEY |
		    UMIDI20_WHAT_KEY_PRESSURE);
	case 0xB:
		return (UMIDI20_WHAT_CHANNEL |
		    UMIDI20_WHAT_CONTROL_VALUE |
		    UMIDI20_WHAT_CONTROL_ADDRESS);
	case 0xC:
		return (UMIDI20_WHAT_CHANNEL |
		    UMIDI20_WHAT_PROGRAM_VALUE);
	case 0xD:
		return (UMIDI20_WHAT_CHANNEL |
		    UMIDI20_WHAT_CHANNEL_PRESSURE);
	case 0xE:
		return (UMIDI20_WHAT_CHANNEL |
		    UMIDI20_WHAT_PITCH_BEND);
	default:
		break;
	}
	return 0;
}

uint8_t
umidi20_event_is_meta(struct umidi20_event *event)
{
	return ((umidi20_event_get_length_first(event) > 1) &&
	    (event->cmd[1] == 0xFF));
}

uint8_t
umidi20_event_is_key_start(struct umidi20_event *event)
{
	if (event == NULL)
		return 0;
	return (((event->cmd[1] & 0xF0) == 0x90) && event->cmd[3]);
}

uint8_t
umidi20_event_is_key_end(struct umidi20_event *event)
{
	if (event == NULL)
		return 0;
	return (((event->cmd[1] & 0xF0) == 0x80) ||
	    (((event->cmd[1] & 0xF0) == 0x90) && (event->cmd[3] == 0)));
}

uint8_t
umidi20_event_is_tempo(struct umidi20_event *event)
{
	if (event == NULL)
		return 0;
	return ((event->cmd[1] == 0xFF) &&
	    (event->cmd[2] == 0x51));
}

uint8_t
umidi20_event_is_voice(struct umidi20_event *event)
{
	if (event == NULL)
		return 0;
	return ((event->cmd[1] >= 0x80) &&
	    (event->cmd[1] <= 0xEF));
}

uint8_t
umidi20_event_is_sysex(struct umidi20_event *event)
{
	if (event == NULL)
		return 0;
	return (event->cmd[1] == 0xF0);
}

uint8_t
umidi20_event_get_channel(struct umidi20_event *event)
{
	uint32_t what = umidi20_event_get_what(event);

	return ((what & UMIDI20_WHAT_CHANNEL) ?
	    (event->cmd[1] & 0x0F) : 0);
}

void
umidi20_event_set_channel(struct umidi20_event *event, uint8_t c)
{
	uint32_t what = umidi20_event_get_what(event);

	if (what & UMIDI20_WHAT_CHANNEL) {
		event->cmd[1] &= 0xF0;
		event->cmd[1] |= (c & 0x0F);
	}
	return;
}

uint8_t
umidi20_event_get_key(struct umidi20_event *event)
{
	uint32_t what = umidi20_event_get_what(event);

	return ((what & UMIDI20_WHAT_KEY) ?
	    event->cmd[2] : 0);
}

void
umidi20_event_set_key(struct umidi20_event *event, uint8_t k)
{
	uint32_t what = umidi20_event_get_what(event);

	if (what & UMIDI20_WHAT_KEY) {
		event->cmd[2] = k & 0x7F;
	}
	return;
}

uint8_t
umidi20_event_get_velocity(struct umidi20_event *event)
{
	uint32_t what = umidi20_event_get_what(event);

	return ((what & UMIDI20_WHAT_VELOCITY) ?
	    event->cmd[3] : 0);
}

void
umidi20_event_set_velocity(struct umidi20_event *event, uint8_t k)
{
	uint32_t what = umidi20_event_get_what(event);

	if (what & UMIDI20_WHAT_VELOCITY) {
		event->cmd[3] = k & 0x7F;
	}
	return;
}

uint8_t
umidi20_event_get_pressure(struct umidi20_event *event)
{
	uint32_t what = umidi20_event_get_what(event);

	if (what & UMIDI20_WHAT_CHANNEL_PRESSURE) {
		return event->cmd[2];
	}
	if (what & UMIDI20_WHAT_KEY_PRESSURE) {
		return event->cmd[3];
	}
	return 0;
}

void
umidi20_event_set_pressure(struct umidi20_event *event, uint8_t p)
{
	uint32_t what = umidi20_event_get_what(event);

	if (what & UMIDI20_WHAT_CHANNEL_PRESSURE) {
		event->cmd[2] = p & 0x7F;
	}
	if (what & UMIDI20_WHAT_KEY_PRESSURE) {
		event->cmd[3] = p & 0x7F;
	}
	return;
}

uint8_t
umidi20_event_get_control_address(struct umidi20_event *event)
{
	uint32_t what = umidi20_event_get_what(event);

	return ((what & UMIDI20_WHAT_CONTROL_ADDRESS) ?
	    event->cmd[2] : 0);
}

void
umidi20_event_set_control_address(struct umidi20_event *event, uint8_t a)
{
	uint32_t what = umidi20_event_get_what(event);

	if (what & UMIDI20_WHAT_CONTROL_ADDRESS) {
		event->cmd[2] = a & 0x7F;
	}
	return;
}

uint8_t
umidi20_event_get_control_value(struct umidi20_event *event)
{
	uint32_t what = umidi20_event_get_what(event);

	return ((what & UMIDI20_WHAT_CONTROL_VALUE) ?
	    event->cmd[3] : 0);
}

void
umidi20_event_set_control_value(struct umidi20_event *event, uint8_t a)
{
	uint32_t what = umidi20_event_get_what(event);

	if (what & UMIDI20_WHAT_CONTROL_VALUE) {
		event->cmd[3] = a & 0x7F;
	}
	return;
}

uint8_t
umidi20_event_get_program_number(struct umidi20_event *event)
{
	uint32_t what = umidi20_event_get_what(event);

	return ((what & UMIDI20_WHAT_PROGRAM_VALUE) ?
	    event->cmd[2] : 0);
}

void
umidi20_event_set_program_number(struct umidi20_event *event, uint8_t n)
{
	uint32_t what = umidi20_event_get_what(event);

	if (what & UMIDI20_WHAT_PROGRAM_VALUE) {
		event->cmd[2] = n & 0x7F;
	}
	return;
}

uint16_t
umidi20_event_get_pitch_value(struct umidi20_event *event)
{
	uint32_t what = umidi20_event_get_what(event);

	return ((what & UMIDI20_WHAT_PITCH_BEND) ?
	    (event->cmd[2] | (event->cmd[3] << 7)) : 0);
}

void
umidi20_event_set_pitch_value(struct umidi20_event *event, uint16_t n)
{
	uint32_t what = umidi20_event_get_what(event);

	if (what & UMIDI20_WHAT_PITCH_BEND) {
		event->cmd[2] = n & 0x7F;
		event->cmd[3] = (n >> 7) & 0x7F;
	}
	return;
}

uint32_t
umidi20_event_get_length_first(struct umidi20_event *event)
{
	uint32_t len;

	if (event) {
		len = umidi20_command_to_len[event->cmd[0] & 0xF];
	} else {
		len = 0;
	}
	return len;
}

uint32_t
umidi20_event_get_length(struct umidi20_event *event)
{
	uint32_t len = 0;

	while (event) {
		len += umidi20_command_to_len[event->cmd[0] & 0xF];
		event = event->p_next;
	}
	return len;
}

void
umidi20_event_copy_out(struct umidi20_event *event, uint8_t *dst,
    uint32_t offset, uint32_t len)
{
	uint32_t part_len;

	while (offset > 0) {
		part_len = umidi20_command_to_len[event->cmd[0] & 0xF];

		if (offset < part_len) {
			break;
		}
		offset -= part_len;
		event = event->p_next;
	}

	while (len > 0) {

		part_len = umidi20_command_to_len[event->cmd[0] & 0xF];
		part_len -= offset;

		if (part_len > len) {
			part_len = len;
		}
		bcopy(event->cmd + 1 + offset, dst, part_len);

		dst += part_len;
		len -= part_len;
		offset = 0;
		event = event->p_next;
	}
	return;
}

uint8_t
umidi20_event_get_meta_number(struct umidi20_event *event)
{
	return (umidi20_event_is_meta(event) ?
	    event->cmd[2] : 0);
}

void
umidi20_event_set_meta_number(struct umidi20_event *event, uint8_t n)
{
	if (umidi20_event_is_meta(event)) {
		event->cmd[2] = n & 0x7F;
	}
	return;
}

uint32_t
umidi20_event_get_tempo(struct umidi20_event *event)
{
	uint32_t tempo;

	if (!umidi20_event_is_tempo(event)) {
		tempo = 1;
	} else {
		tempo = (event->cmd[3] << 16) | (event->cmd[4] << 8) | event->cmd[5];
		if (tempo == 0) {
			tempo = 1;
		}
		tempo = 60000000 / tempo;
		if (tempo == 0) {
			tempo = 1;
		}
		if (tempo > 65535) {
			tempo = 65535;
		}
	}
	return tempo;
}

void
umidi20_event_set_tempo(struct umidi20_event *event,
    uint32_t tempo)
{
	if (!umidi20_event_is_tempo(event)) {
		return;
	}
	if (tempo < 3) {
		tempo = 3;
	}
	if (tempo > 65535) {
		tempo = 65535;
	}
	tempo = (60000000 + (tempo / 2) - 1) / tempo;

	event->cmd[3] = (tempo >> 16) & 0xFF;
	event->cmd[4] = (tempo >> 8) & 0xFF;
	event->cmd[5] = (tempo & 0xFF);
	event->cmd[0] = 6;		/* bytes */

	return;
}

struct umidi20_event *
umidi20_event_queue_search(struct umidi20_event_queue *queue,
    uint32_t position, uint8_t cache_no)
{
	struct umidi20_event *event = queue->ifq_cache[cache_no];

	if (event == NULL) {
		UMIDI20_IF_POLL_HEAD(queue, event);
		if (event == NULL) {
			goto done;
		}
	}
	while (1) {
		if (event->position < position) {
			break;
		}
		if (event->p_prevpkt == NULL) {
			break;
		}
		event = event->p_prevpkt;
	}

	while (1) {
		if (event->position >= position) {
			queue->ifq_cache[cache_no] = event;
			goto done;
		}
		if (event->p_nextpkt == NULL) {
			queue->ifq_cache[cache_no] = event;
			event = NULL;
			goto done;
		}
		event = event->p_nextpkt;
	}
done:
	return event;
}

void
umidi20_event_queue_copy(struct umidi20_event_queue *src,
    struct umidi20_event_queue *dst,
    uint32_t pos_a, uint32_t pos_b,
    uint16_t rev_a, uint16_t rev_b,
    uint8_t cache_no, uint8_t flag)
{
	struct umidi20_event *event_a;
	struct umidi20_event *event_b;
	struct umidi20_event *event_n;

	if (pos_b < pos_a) {
		pos_b = -1;
	}
	event_a = umidi20_event_queue_search(src, pos_a, cache_no);
	event_b = umidi20_event_queue_search(src, pos_b, cache_no);

	while (event_a != event_b) {

		if ((event_a->revision >= rev_a) &&
		    (event_a->revision < rev_b)) {

			event_n = umidi20_event_copy(event_a, flag);

			if (event_n) {
				umidi20_event_queue_insert(dst, event_n, cache_no);
			} else {
				/* XXX do what */
			}
		}
		event_a = event_a->p_nextpkt;
	}
	return;
}

void
umidi20_event_queue_move(struct umidi20_event_queue *src,
    struct umidi20_event_queue *dst,
    uint32_t pos_a, uint32_t pos_b,
    uint16_t rev_a, uint16_t rev_b,
    uint8_t cache_no)
{
	struct umidi20_event *event_a;
	struct umidi20_event *event_b;
	struct umidi20_event *event_n;

	if (pos_b < pos_a) {
		pos_b = -1;
	}
	event_a = umidi20_event_queue_search(src, pos_a, cache_no);
	event_b = umidi20_event_queue_search(src, pos_b, cache_no);

	while (event_a != event_b) {

		event_n = event_a->p_nextpkt;

		if ((event_a->revision >= rev_a) &&
		    (event_a->revision < rev_b)) {

			UMIDI20_IF_REMOVE(src, event_a);

			if (dst) {
				umidi20_event_queue_insert(dst, event_a, cache_no);
			} else {
				umidi20_event_free(event_a);
			}
		}
		event_a = event_n;
	}
	return;
}

void
umidi20_event_queue_insert(struct umidi20_event_queue *dst,
    struct umidi20_event *event_n,
    uint8_t cache_no)
{
	struct umidi20_event *event_a =
	umidi20_event_queue_search(dst, event_n->position + 1, cache_no);

	if (event_a == NULL) {
		/* queue at end */
		UMIDI20_IF_ENQUEUE_LAST(dst, event_n);
	} else {
		/* queue before event */
		UMIDI20_IF_ENQUEUE_BEFORE(dst, event_a, event_n);
	}
	return;
}

void
umidi20_event_queue_drain(struct umidi20_event_queue *src)
{
	struct umidi20_event *event;

	while (1) {

		UMIDI20_IF_DEQUEUE(src, event);

		if (event == NULL) {
			break;
		}
		umidi20_event_free(event);
	}
	return;
}

/*
 * the following statemachine, that converts MIDI commands to
 * USB MIDI packets, derives from Linux's usbmidi.c, which
 * was written by "Clemens Ladisch":
 *
 * return values:
 *    0: No command
 * Else: Command is complete
 */
uint8_t
umidi20_convert_to_command(struct umidi20_converter *conv, uint8_t b)
{
	static const uint8_t p0 = 0x0;
	static const uint8_t s0 = 0x0;

	if (b >= 0xf8) {
		conv->temp_0[0] = p0 | 0x01 | 0x8;
		conv->temp_0[1] = b;
		conv->temp_0[2] = 0;
		conv->temp_0[3] = 0;
		conv->temp_0[4] = 0;
		conv->temp_0[5] = 0;
		conv->temp_0[6] = 0;
		conv->temp_0[7] = 0;
		conv->temp_cmd = conv->temp_0;
		return 1;

	} else if (b >= 0xf0) {
		switch (b) {
		case 0xf0:		/* system exclusive begin */
			conv->temp_1[1] = b;
			conv->state = UMIDI20_ST_SYSEX_1;
			break;
		case 0xf1:		/* MIDI time code */
		case 0xf3:		/* song select */
			conv->temp_1[1] = b;
			conv->state = UMIDI20_ST_1PARAM;
			break;
		case 0xf2:		/* song position pointer */
			conv->temp_1[1] = b;
			conv->state = UMIDI20_ST_2PARAM_1;
			break;
		case 0xf4:		/* unknown */
		case 0xf5:		/* unknown */
			conv->state = UMIDI20_ST_UNKNOWN;
			break;
		case 0xf6:		/* tune request */
			conv->temp_1[0] = p0 | 0x01 | 0x8;
			conv->temp_1[1] = 0xf6;
			conv->temp_1[2] = 0;
			conv->temp_1[3] = 0;
			conv->temp_1[4] = 0;
			conv->temp_1[5] = 0;
			conv->temp_1[6] = 0;
			conv->temp_1[7] = 0;
			conv->temp_cmd = conv->temp_1;
			conv->state = UMIDI20_ST_UNKNOWN;
			return 1;

		case 0xf7:		/* system exclusive end */
			switch (conv->state) {
			case UMIDI20_ST_SYSEX_0:
				conv->temp_1[0] = p0 | 0x1 | s0;
				conv->temp_1[1] = 0xf7;
				conv->temp_1[2] = 0;
				conv->temp_1[3] = 0;
				conv->temp_1[4] = 0;
				conv->temp_1[5] = 0;
				conv->temp_1[6] = 0;
				conv->temp_1[7] = 0;
				conv->temp_cmd = conv->temp_1;
				conv->state = UMIDI20_ST_UNKNOWN;
				return 1;
			case UMIDI20_ST_SYSEX_1:
				conv->temp_1[0] = p0 | 0x2 | s0;
				conv->temp_1[2] = 0xf7;
				conv->temp_1[3] = 0;
				conv->temp_1[4] = 0;
				conv->temp_1[5] = 0;
				conv->temp_1[6] = 0;
				conv->temp_1[7] = 0;
				conv->temp_cmd = conv->temp_1;
				conv->state = UMIDI20_ST_UNKNOWN;
				return 1;
			case UMIDI20_ST_SYSEX_2:
				conv->temp_1[0] = p0 | 0x3 | s0;
				conv->temp_1[3] = 0xf7;
				conv->temp_1[4] = 0;
				conv->temp_1[5] = 0;
				conv->temp_1[6] = 0;
				conv->temp_1[7] = 0;
				conv->temp_cmd = conv->temp_1;
				conv->state = UMIDI20_ST_UNKNOWN;
				return 1;
			case UMIDI20_ST_SYSEX_3:
				conv->temp_1[0] = p0 | 0x4 | s0;
				conv->temp_1[4] = 0xf7;
				conv->temp_1[5] = 0;
				conv->temp_1[6] = 0;
				conv->temp_1[7] = 0;
				conv->temp_cmd = conv->temp_1;
				conv->state = UMIDI20_ST_UNKNOWN;
				return 1;
			case UMIDI20_ST_SYSEX_4:
				conv->temp_1[0] = p0 | 0x5 | s0;
				conv->temp_1[5] = 0xf7;
				conv->temp_1[6] = 0;
				conv->temp_1[7] = 0;
				conv->temp_cmd = conv->temp_1;
				conv->state = UMIDI20_ST_UNKNOWN;
				return 1;
			case UMIDI20_ST_SYSEX_5:
				conv->temp_1[0] = p0 | 0x6 | s0;
				conv->temp_1[6] = 0xf7;
				conv->temp_1[7] = 0;
				conv->temp_cmd = conv->temp_1;
				conv->state = UMIDI20_ST_UNKNOWN;
				return 1;
			case UMIDI20_ST_SYSEX_6:
				conv->temp_1[0] = p0 | 0x7 | s0;
				conv->temp_1[7] = 0xf7;
				conv->temp_cmd = conv->temp_1;
				conv->state = UMIDI20_ST_UNKNOWN;
				return 1;
			}
			conv->state = UMIDI20_ST_UNKNOWN;
			break;
		}
	} else if (b >= 0x80) {
		conv->temp_1[1] = b;
		if ((b >= 0xc0) && (b <= 0xdf)) {
			conv->state = UMIDI20_ST_1PARAM;
		} else {
			conv->state = UMIDI20_ST_2PARAM_1;
		}
	} else {			/* b < 0x80 */
		switch (conv->state) {
		case UMIDI20_ST_1PARAM:
			if (conv->temp_1[1] >= 0xf0) {
				conv->state = UMIDI20_ST_UNKNOWN;
			}
			conv->temp_1[0] = p0 | 0x02 | 0x8;
			conv->temp_1[2] = b;
			conv->temp_1[3] = 0;
			conv->temp_1[4] = 0;
			conv->temp_1[5] = 0;
			conv->temp_1[6] = 0;
			conv->temp_1[7] = 0;
			conv->temp_cmd = conv->temp_1;
			return 1;
		case UMIDI20_ST_2PARAM_1:
			conv->temp_1[2] = b;
			conv->state = UMIDI20_ST_2PARAM_2;
			break;
		case UMIDI20_ST_2PARAM_2:
			if (conv->temp_1[1] < 0xf0) {
				conv->state = UMIDI20_ST_2PARAM_1;
			} else {
				conv->state = UMIDI20_ST_UNKNOWN;
			}
			conv->temp_1[0] = p0 | 0x03 | 0x8;
			conv->temp_1[3] = b;
			conv->temp_1[4] = 0;
			conv->temp_1[5] = 0;
			conv->temp_1[6] = 0;
			conv->temp_1[7] = 0;
			conv->temp_cmd = conv->temp_1;
			return 1;
		case UMIDI20_ST_SYSEX_0:
			conv->temp_1[1] = b;
			conv->state = UMIDI20_ST_SYSEX_1;
			break;
		case UMIDI20_ST_SYSEX_1:
			conv->temp_1[2] = b;
			conv->state = UMIDI20_ST_SYSEX_2;
			break;
		case UMIDI20_ST_SYSEX_2:
			conv->temp_1[3] = b;
			conv->state = UMIDI20_ST_SYSEX_3;
			break;
		case UMIDI20_ST_SYSEX_3:
			conv->temp_1[4] = b;
			conv->state = UMIDI20_ST_SYSEX_4;
			break;
		case UMIDI20_ST_SYSEX_4:
			conv->temp_1[5] = b;
			conv->state = UMIDI20_ST_SYSEX_5;
			break;
		case UMIDI20_ST_SYSEX_5:
			conv->temp_1[6] = b;
			conv->state = UMIDI20_ST_SYSEX_6;
			break;
		case UMIDI20_ST_SYSEX_6:
			if (conv->temp_1[1] == 0xF0) {
				conv->temp_1[0] = p0 | 0x0 | s0;
			} else {
				conv->temp_1[0] = p0 | 0x8 | s0;
			}
			conv->temp_1[7] = b;
			conv->temp_cmd = conv->temp_1;
			conv->state = UMIDI20_ST_SYSEX_0;
			return 1;
		}
	}
	return 0;
}

struct umidi20_event *
umidi20_convert_to_event(struct umidi20_converter *conv,
    uint8_t b, uint8_t flag)
{
	struct umidi20_event *event = NULL;

	if (umidi20_convert_to_command(conv, b)) {

		if (conv->temp_cmd[0] == 0x0) {
			/* long command begins */
			umidi20_event_free(conv->p_next);
			conv->p_next = NULL;
			conv->pp_next = &(conv->p_next);
		}
		if (conv->temp_cmd[0] <= 0x8) {
			event = umidi20_event_alloc(&(conv->pp_next), flag);
		} else {
			event = umidi20_event_alloc(NULL, flag);
		}

		bcopy(conv->temp_cmd, event->cmd, UMIDI20_COMMAND_LEN);

		if ((conv->temp_cmd[0] == 0x8) ||
		    (conv->temp_cmd[0] == 0x0)) {
			event = NULL;
		} else if (conv->temp_cmd[0] < 8) {
			event = conv->p_next;
			conv->p_next = NULL;
			conv->pp_next = &(conv->p_next);
		} else {
			/* short command */
		}
	}
	return event;
}

void
umidi20_convert_reset(struct umidi20_converter *conv)
{
	umidi20_event_free(conv->p_next);
	bzero(conv, sizeof(*conv));
	return;
}

const
uint8_t	umidi20_command_to_len[16] = {

	/* Long MIDI commands */

	[0x0] = 7,			/* bytes, long command begins */
	[0x1] = 1,			/* bytes, long command ends */
	[0x2] = 2,			/* bytes, long command ends */
	[0x3] = 3,			/* bytes, long command ends */
	[0x4] = 4,			/* bytes, long command ends */
	[0x5] = 5,			/* bytes, long command ends */
	[0x6] = 6,			/* bytes, long command ends */
	[0x7] = 7,			/* bytes, long command ends */
	[0x8] = 7,			/* bytes, long command continues */

	/* Short MIDI commands */

	[0x9] = 1,			/* bytes, short command ends */
	[0xA] = 2,			/* bytes, short command ends */
	[0xB] = 3,			/* bytes, short command ends */
	[0xC] = 4,			/* bytes, short command ends */
	[0xD] = 5,			/* bytes, short command ends */
	[0xE] = 6,			/* bytes, short command ends */
	[0xF] = 7,			/* bytes, short command ends */
};

void
umidi20_gettime(struct timespec *ts)
{
	if (clock_gettime(CLOCK_MONOTONIC, ts) == -1) {
		bzero(ts, sizeof(*ts));
	}
	return;
}

uint32_t
umidi20_difftime(struct timespec *a, struct timespec *b)
{
	struct timespec c;

	c.tv_sec = a->tv_sec - b->tv_sec;
	c.tv_nsec = a->tv_nsec - b->tv_nsec;

	if (a->tv_nsec < b->tv_nsec) {
		c.tv_sec -= 1;
		c.tv_nsec += 1000000000;
	}
	return ((c.tv_sec * 1000) + (c.tv_nsec / 1000000));
}

int
umidi20_mutex_init(pthread_mutex_t *pmutex)
{
	pthread_mutexattr_t attr;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	return pthread_mutex_init(pmutex, &attr);
}

static void
umidi20_device_start(struct umidi20_device *dev,
    uint32_t start_position,
    uint32_t end_offset)
{
	dev->start_position = start_position;
	dev->end_offset = end_offset;
	dev->enabled_usr = 1;
	return;
}

static void
umidi20_device_stop(struct umidi20_device *dev)
{
	uint32_t y;
	uint8_t buf[3];

	dev->enabled_usr = 0;
	umidi20_convert_reset(&(dev->conv));
	umidi20_event_queue_drain(&(dev->queue));

	/*
	 * XXX turn off all active keys
	 */
	if (dev->file_no >= 0) {

		/* turn any notes off */
		for (y = 0; y != (16 * 128); y++) {
			if (dev->key_on_table[y / 8] & (1 << (y % 8))) {
				buf[0] = (0x90 | (y / 128));
				buf[1] = (y % 128);
				buf[2] = (0);
				write(dev->file_no, buf, 3);
			}
		}

		/* turn pedal off */
		for (y = 0; y != 16; y++) {
			buf[0] = 0xB0 | y;
			buf[1] = 0x40;
			buf[2] = 0;
			write(dev->file_no, buf, 3);
		}
	}
	bzero(dev->key_on_table, sizeof(dev->key_on_table));
	return;
}

static void
umidi20_put_queue(uint8_t device_no,
    struct umidi20_event *event)
{
	struct umidi20_device *dev;

	if (device_no >= UMIDI20_N_DEVICES) {
		umidi20_event_free(event);
		return;
	}
	pthread_mutex_lock(&(root_dev.mutex));

	dev = &(root_dev.play[device_no]);

	if (dev->enabled_usr &&
	    dev->enabled_cfg) {
		umidi20_event_queue_insert
		    (&(dev->queue), event, UMIDI20_CACHE_INPUT);
	} else {
		umidi20_event_free(event);
	}

	pthread_mutex_unlock(&(root_dev.mutex));

	return;
}

static struct umidi20_event *
umidi20_get_queue(uint8_t device_no)
{
	struct umidi20_device *dev;
	struct umidi20_event *event;

	if (device_no >= UMIDI20_N_DEVICES) {
		return NULL;
	}
	pthread_mutex_lock(&(root_dev.mutex));

	dev = &(root_dev.rec[device_no]);

	if (dev->enabled_usr &&
	    dev->enabled_cfg) {
		UMIDI20_IF_DEQUEUE(&(dev->queue), event);
	} else {
		event = NULL;
	}

	pthread_mutex_unlock(&(root_dev.mutex));

	return event;
}

struct timespec
umidi20_get_curr_time(void)
{
	struct timespec ts;

	pthread_mutex_lock(&(root_dev.mutex));
	ts = root_dev.curr_time;
	pthread_mutex_unlock(&(root_dev.mutex));
	return ts;
}

void
umidi20_start(uint32_t start_offset, uint32_t end_offset, uint8_t flag)
{
	uint32_t x;
	uint32_t start_position;

	if (flag == 0) {
		return;
	}
	pthread_mutex_lock(&(root_dev.mutex));

	umidi20_stop(flag);

	/* sanity checking */

	if ((end_offset <= start_offset) ||
	    (start_offset > 0x80000000) ||
	    (end_offset > 0x80000000)) {
		goto done;
	}
	start_position = (root_dev.curr_position - start_offset);

	if (flag & UMIDI20_FLAG_PLAY) {
		for (x = 0; x < UMIDI20_N_DEVICES; x++) {
			umidi20_device_start
			    (&(root_dev.play[x]), start_position, end_offset);
		}
	}
	if (flag & UMIDI20_FLAG_RECORD) {
		for (x = 0; x < UMIDI20_N_DEVICES; x++) {
			umidi20_device_start
			    (&(root_dev.rec[x]), start_position, end_offset);
		}
	}
done:
	pthread_mutex_unlock(&(root_dev.mutex));
	return;
}

void
umidi20_stop(uint8_t flag)
{
	uint32_t x;

	if (flag == 0) {
		return;
	}
	pthread_mutex_lock(&(root_dev.mutex));
	if (flag & UMIDI20_FLAG_PLAY) {
		for (x = 0; x < UMIDI20_N_DEVICES; x++) {
			umidi20_device_stop(&(root_dev.play[x]));
		}
	}
	if (flag & UMIDI20_FLAG_RECORD) {
		for (x = 0; x < UMIDI20_N_DEVICES; x++) {
			umidi20_device_stop(&(root_dev.rec[x]));
		}
	}
	pthread_mutex_unlock(&(root_dev.mutex));
	return;
}

uint8_t
umidi20_all_dev_off(uint8_t flag)
{
	uint32_t x;
	uint8_t retval = 1;

	if (flag == 0) {
		goto done;
	}
	pthread_mutex_lock(&(root_dev.mutex));
	if (flag & UMIDI20_FLAG_PLAY) {
		for (x = 0; x < UMIDI20_N_DEVICES; x++) {
			if (root_dev.play[x].enabled_cfg) {
				retval = 0;
				break;
			}
		}
	}
	if (flag & UMIDI20_FLAG_RECORD) {
		for (x = 0; x < UMIDI20_N_DEVICES; x++) {
			if (root_dev.rec[x].enabled_cfg) {
				retval = 0;
				break;
			}
		}
	}
	pthread_mutex_unlock(&(root_dev.mutex));
done:
	return retval;
}

struct umidi20_song *
umidi20_song_alloc(pthread_mutex_t *p_mtx, uint16_t file_format,
    uint16_t resolution, uint8_t div_type)
{
	struct umidi20_song *song = malloc(sizeof(*song));

	if (song) {

		bzero(song, sizeof(*song));

		song->p_mtx = p_mtx;

		if (pthread_create(&(song->thread_io), NULL,
		    &umidi20_watchdog_song, song)) {
			song->thread_io = NULL;
		}
		song->midi_file_format = file_format;

		if (resolution == 0) {
			/* avoid division by zero */
			resolution = 1;
		}
		song->midi_resolution = resolution;
		song->midi_division_type = div_type;
	}
	return song;
}

void
umidi20_song_free(struct umidi20_song *song)
{
	struct umidi20_track *track;

	if (song == NULL) {
		return;
	}
	pthread_mutex_assert(song->p_mtx, MA_OWNED);

	umidi20_stop_thread(&(song->thread_io), song->p_mtx);

	umidi20_song_stop(song, UMIDI20_FLAG_PLAY | UMIDI20_FLAG_RECORD);

	while (1) {

		UMIDI20_IF_DEQUEUE(&(song->queue), track);

		if (track == NULL) {
			break;
		}
		umidi20_track_free(track);
	}

	free(song);
	return;
}

static void
umidi20_watchdog_song_sub(struct umidi20_song *song)
{
	struct umidi20_track *track;
	struct umidi20_event *event;
	struct umidi20_event_queue queue;
	uint32_t curr_position;
	uint32_t position;
	uint32_t x;

	pthread_mutex_assert(song->p_mtx, MA_OWNED);

	bzero(&queue, sizeof(queue));

	pthread_mutex_lock(&(root_dev.mutex));
	curr_position = root_dev.curr_position;
	pthread_mutex_unlock(&(root_dev.mutex));

	track = song->queue.ifq_cache[UMIDI20_CACHE_INPUT];

	if (song->rec_enabled && track) {

		for (x = 0; x < UMIDI20_N_DEVICES; x++) {

			while (1) {

				event = umidi20_get_queue(x);

				if (event == NULL) {
					break;
				}
				umidi20_event_queue_insert
				    (&(track->queue), event,
				    UMIDI20_CACHE_INPUT);
			}
		}
	}
	if (song->play_enabled) {

		position = (curr_position - song->play_start_position);

		position += (song->play_start_offset + 1500);

		if (position >= song->play_end_offset) {
			song->play_enabled = 0;
			position = song->play_end_offset;
		}
		UMIDI20_QUEUE_FOREACH(track, &(song->queue)) {
			if (!(track->mute_flag)) {
				umidi20_event_queue_copy(&(track->queue), &queue,
				    song->play_last_offset,
				    position, 0, -1,
				    UMIDI20_CACHE_OUTPUT, 0);
			}
		}

		song->play_last_offset = position;

		while (1) {

			UMIDI20_IF_DEQUEUE(&queue, event);

			if (event == NULL) {
				break;
			}
			umidi20_put_queue(event->device_no, event);
		}
	}
	return;
}


static void *
umidi20_watchdog_song(void *arg)
{
	struct umidi20_song *song = arg;

	pthread_mutex_lock(song->p_mtx);

	while (song->thread_io) {

		umidi20_watchdog_song_sub(song);

		pthread_mutex_unlock(song->p_mtx);

		usleep(250000);

		pthread_mutex_lock(song->p_mtx);
	}

	pthread_mutex_unlock(song->p_mtx);

	return NULL;
}

struct umidi20_track *
umidi20_song_track_by_unit(struct umidi20_song *song, uint16_t unit)
{
	struct umidi20_track *track;

	if (song == NULL) {
		return NULL;
	}
	pthread_mutex_assert(song->p_mtx, MA_OWNED);

	UMIDI20_IF_POLL_HEAD(&(song->queue), track);

	while (track) {
		if (!unit--) {
			break;
		}
		track = track->p_nextpkt;
	}
	return track;
}

/*
 * if "track == NULL" then
 * recording is disabled
 */
void
umidi20_song_set_record_track(struct umidi20_song *song,
    struct umidi20_track *track)
{
	if (song == NULL) {
		return;
	}
	pthread_mutex_assert(song->p_mtx, MA_OWNED);
	song->queue.ifq_cache[UMIDI20_CACHE_INPUT] = track;
	return;
}

/*
 * this function can be called
 * multiple times in a row:
 */
void
umidi20_song_start(struct umidi20_song *song, uint32_t start_offset,
    uint32_t end_offset,
    uint8_t flags)
{
	uint32_t curr_position;

	if (song == NULL) {
		goto done;
	}
	if (flags == 0) {
		goto done;
	}
	pthread_mutex_assert(song->p_mtx, MA_OWNED);

	umidi20_song_stop(song, flags);

	/* sanity checking */

	if ((end_offset <= start_offset) ||
	    (start_offset > 0x80000000) ||
	    (end_offset > 0x80000000)) {
		goto done;
	}
	umidi20_start(start_offset, end_offset, flags);

	pthread_mutex_lock(&(root_dev.mutex));
	curr_position = root_dev.curr_position;
	pthread_mutex_unlock(&(root_dev.mutex));

	if (flags & UMIDI20_FLAG_PLAY) {
		song->play_enabled = 1;
		song->play_start_position = curr_position;
		song->play_start_offset = start_offset;
		song->play_last_offset = start_offset;
		song->play_end_offset = end_offset;
	}
	if (flags & UMIDI20_FLAG_RECORD) {
		song->rec_enabled = 1;
	}
	/* update buffering */

	umidi20_watchdog_song_sub(song);

	song->pc_flags |= flags;

done:
	return;
}

void
umidi20_song_stop(struct umidi20_song *song, uint8_t flags)
{
	if (song == NULL) {
		goto done;
	}
	if (flags == 0) {
		goto done;
	}
	pthread_mutex_assert(song->p_mtx, MA_OWNED);

	flags &= song->pc_flags;

	if (flags & UMIDI20_FLAG_PLAY) {
		song->play_enabled = 0;
	}
	if (flags & UMIDI20_FLAG_RECORD) {
		song->rec_enabled = 0;
	}
	umidi20_stop(flags);

	song->pc_flags &= ~flags;

done:
	return;
}

void
umidi20_song_track_add(struct umidi20_song *song,
    struct umidi20_track *track_ref,
    struct umidi20_track *track_new,
    uint8_t is_before_ref)
{
	pthread_mutex_assert(song->p_mtx, MA_OWNED);

	if (song == NULL) {
		umidi20_track_free(track_new);
		return;
	}
	if (track_ref == NULL) {
		UMIDI20_IF_ENQUEUE_LAST(&(song->queue), track_new);
	} else {
		if (is_before_ref) {
			UMIDI20_IF_ENQUEUE_BEFORE(&(song->queue), track_ref, track_new);
		} else {
			UMIDI20_IF_ENQUEUE_AFTER(&(song->queue), track_ref, track_new);
		}
	}
	return;
}

void
umidi20_song_track_remove(struct umidi20_song *song,
    struct umidi20_track *track)
{
	pthread_mutex_assert(song->p_mtx, MA_OWNED);

	if (song == NULL) {
		return;			/* XXX should not happen */
	}
	if (track == NULL) {
		return;
	}
	UMIDI20_IF_REMOVE(&(song->queue), track);

	umidi20_track_free(track);

	return;
}

void
umidi20_song_recompute_position(struct umidi20_song *song)
{
	struct umidi20_track *conductor_track;
	struct umidi20_track *track;
	struct umidi20_event *event;
	struct umidi20_event *event_copy;

	uint32_t tempo;
	uint32_t last_tick;
	uint32_t delta_tick;
	uint32_t factor;
	uint32_t position_curr;
	uint32_t position_rem;
	uint32_t divisor;

	if (song == NULL) {
		return;
	}
	pthread_mutex_assert(song->p_mtx, MA_OWNED);

	UMIDI20_IF_POLL_HEAD(&(song->queue), conductor_track);

	if (conductor_track == NULL) {
		goto done;
	}
	/*
	 * First copy the conductor track to
	 * all the other tracks:
	 */

	UMIDI20_QUEUE_FOREACH(event, &(conductor_track->queue)) {

		if (umidi20_event_is_tempo(event)) {

			UMIDI20_QUEUE_FOREACH(track, &(song->queue)) {

				if (track != conductor_track) {

					event_copy = umidi20_event_copy(event, 0);
					if (event_copy == NULL) {
						goto fail;
					}
					umidi20_event_queue_insert(&(track->queue),
					    event_copy,
					    UMIDI20_CACHE_INPUT);
				}
			}
		}
	}

	/*
	 * Compute new position information:
	 */
	UMIDI20_QUEUE_FOREACH(track, &(song->queue)) {

		tempo = 120;		/* BPM */
		last_tick = 0;
		position_curr = 0;
		position_rem = 0;

		switch (song->midi_division_type) {
		case UMIDI20_FILE_DIVISION_TYPE_PPQ:
			divisor = (tempo * song->midi_resolution);
			break;
		case UMIDI20_FILE_DIVISION_TYPE_SMPTE24:
			divisor = (24 * song->midi_resolution);
			break;
		case UMIDI20_FILE_DIVISION_TYPE_SMPTE25:
			divisor = (25 * song->midi_resolution);
			break;
		case UMIDI20_FILE_DIVISION_TYPE_SMPTE30DROP:
			divisor = (29.97 * song->midi_resolution);
			break;
		case UMIDI20_FILE_DIVISION_TYPE_SMPTE30:
			divisor = (30 * song->midi_resolution);
			break;
		default:
			divisor = 120;
			break;
		}

		if (song->midi_division_type == UMIDI20_FILE_DIVISION_TYPE_PPQ) {
			factor = UMIDI20_BPM;
		} else {
			factor = (UMIDI20_BPM / 60);
		}

		DEBUG("divisor=%d\n", divisor);

		UMIDI20_QUEUE_FOREACH(event, &(track->queue)) {

			delta_tick = (event->tick - last_tick);
			last_tick = event->tick;

			position_curr += (delta_tick / divisor) * factor;
			position_rem += (delta_tick % divisor) * factor;

			position_curr += (position_rem / divisor);
			position_rem %= divisor;

			DEBUG("%d / %d, pos = %d, tick = %d\n", delta_tick,
			    divisor, position_curr, event->tick);

			/* update position */
			event->position = position_curr;

			if (umidi20_event_is_tempo(event) &&
			    (song->midi_division_type == UMIDI20_FILE_DIVISION_TYPE_PPQ)) {
				tempo = umidi20_event_get_tempo(event);
				divisor = (tempo * song->midi_resolution);
				position_rem = 0;
			}
		}
	}

fail:
	/*
	 * Remove all tempo information from
	 * non-conductor tracks:
	 */
	UMIDI20_QUEUE_FOREACH(track, &(song->queue)) {

		if (track != conductor_track) {

			UMIDI20_QUEUE_FOREACH_SAFE(event, &(track->queue), event_copy) {

				if (umidi20_event_is_tempo(event)) {

					UMIDI20_IF_REMOVE(&(track->queue), event);

					umidi20_event_free(event);
				}
			}
		}
	}
done:
	return;
}

void
umidi20_song_recompute_tick(struct umidi20_song *song)
{
	struct umidi20_track *track;
	struct umidi20_event *event;
	struct umidi20_event *event_next;

	if (song == NULL) {
		return;
	}
	pthread_mutex_assert(song->p_mtx, MA_OWNED);

	song->midi_division_type = UMIDI20_FILE_DIVISION_TYPE_PPQ;
	song->midi_resolution = 500;

	/*
	 * First remove all tempo
	 * information:
	 */
	UMIDI20_QUEUE_FOREACH(track, &(song->queue)) {

		UMIDI20_QUEUE_FOREACH_SAFE(event, &(track->queue), event_next) {

			event->tick = event->position;

			if (umidi20_event_is_tempo(event)) {
				UMIDI20_IF_REMOVE(&(track->queue), event);
				umidi20_event_free(event);
			}
		}
	}
	return;
}

void
umidi20_song_compute_max_min(struct umidi20_song *song)
{
	struct umidi20_track *track;

	pthread_mutex_assert(song->p_mtx, MA_OWNED);

	song->position_max = 0;
	song->track_max = 0;
	song->band_max = 0;

	UMIDI20_QUEUE_FOREACH(track, &(song->queue)) {
		umidi20_track_compute_max_min(track);

		if (track->position_max > song->position_max) {
			song->position_max = track->position_max;
		}
		song->band_max +=
		    (track->band_max - track->band_min);
	}

	song->track_max = UMIDI20_IF_QLEN(&(song->queue));

	return;
}

void
umidi20_config_export(struct umidi20_config *cfg)
{
	uint32_t x;

	bzero(cfg, sizeof(*cfg));

	pthread_mutex_lock(&(root_dev.mutex));

	cfg->effects = root_dev.effects;

	for (x = 0; x < UMIDI20_N_DEVICES; x++) {

		strlcpy(cfg->cfg_dev[x].rec_fname,
		    root_dev.rec[x].fname,
		    sizeof(cfg->cfg_dev[x].rec_fname));

		cfg->cfg_dev[x].rec_enabled_cfg =
		    root_dev.rec[x].enabled_cfg;

		strlcpy(cfg->cfg_dev[x].play_fname,
		    root_dev.play[x].fname,
		    sizeof(cfg->cfg_dev[x].play_fname));

		cfg->cfg_dev[x].play_enabled_cfg =
		    root_dev.play[x].enabled_cfg;
	}

	pthread_mutex_unlock(&(root_dev.mutex));
	return;
}

void
umidi20_config_import(struct umidi20_config *cfg)
{
	uint32_t x;

	pthread_mutex_lock(&(root_dev.mutex));

	root_dev.effects = cfg->effects;

	for (x = 0; x < UMIDI20_N_DEVICES; x++) {

		if (strcasecmp(root_dev.rec[x].fname,
		    cfg->cfg_dev[x].rec_fname)) {
			root_dev.rec[x].update = 1;
			strlcpy(root_dev.rec[x].fname,
			    cfg->cfg_dev[x].rec_fname,
			    sizeof(root_dev.rec[x].fname));
		}
		if (root_dev.rec[x].enabled_cfg !=
		    cfg->cfg_dev[x].rec_enabled_cfg) {

			root_dev.rec[x].update = 1;
			root_dev.rec[x].enabled_cfg =
			    cfg->cfg_dev[x].rec_enabled_cfg;
		}
		if (strcasecmp(root_dev.play[x].fname,
		    cfg->cfg_dev[x].play_fname)) {

			root_dev.play[x].update = 1;
			strlcpy(root_dev.play[x].fname,
			    cfg->cfg_dev[x].play_fname,
			    sizeof(root_dev.play[x].fname));
		}
		if (root_dev.play[x].enabled_cfg !=
		    cfg->cfg_dev[x].play_enabled_cfg) {

			root_dev.play[x].update = 1;
			root_dev.play[x].enabled_cfg =
			    cfg->cfg_dev[x].play_enabled_cfg;
		}
	}
	pthread_mutex_unlock(&(root_dev.mutex));
	return;
}

struct umidi20_track *
umidi20_track_alloc(void)
{
	struct umidi20_track *track;

	track = malloc(sizeof(*track));
	if (track) {
		bzero(track, sizeof(*track));
	}
	return track;
}

void
umidi20_track_free(struct umidi20_track *track)
{
	if (track == NULL) {
		return;
	}
	umidi20_event_queue_drain(&(track->queue));

	free(track);
	return;
}

void
umidi20_track_compute_max_min(struct umidi20_track *track)
{
	struct umidi20_event *event;
	struct umidi20_event *event_last;
	struct umidi20_event *last_key_press[128];
	uint32_t what;
	uint8_t key;
	uint8_t is_on;
	uint8_t is_off;
	uint8_t meta_num;

	bzero(&last_key_press, sizeof(last_key_press));

	track->key_max = 0x00;
	track->key_min = 0xFF;

	track->position_max = 0;

	UMIDI20_QUEUE_FOREACH(event, &(track->queue)) {

		what = umidi20_event_get_what(event);

		if (what & UMIDI20_WHAT_KEY) {

			is_on = umidi20_event_is_key_start(event);
			is_off = umidi20_event_is_key_end(event);
			key = umidi20_event_get_key(event) & 0x7F;

			if (is_on || is_off) {

				event_last = last_key_press[key];
				last_key_press[key] = NULL;

				/* update duration */

				if (event_last) {
					event_last->duration =
					    (event->position - event_last->position);
				}
				if (is_on) {
					last_key_press[key] = event;
				}
			}
			if (key > track->key_max) {
				track->key_max = key;
			}
			if (key < track->key_min) {
				track->key_min = key;
			}
		}
		if (umidi20_event_is_meta(event)) {
			meta_num = umidi20_event_get_meta_number(event);
			what = umidi20_event_get_length(event);

			if (meta_num == 0x03) {
				what -= 2;
				if (what > (sizeof(track->name) - 1)) {
					what = (sizeof(track->name) - 1);
				}
				umidi20_event_copy_out(event, track->name, 2, what);
				track->name[what] = 0;
			}
			if (meta_num == 0x04) {
				what -= 2;
				if (what > (sizeof(track->instrument) - 1)) {
					what = (sizeof(track->instrument) - 1);
				}
				umidi20_event_copy_out(event, track->instrument, 2, what);
				track->instrument[what] = 0;
			}
		}
	}
	if ((track->key_max == 0x00) &&
	    (track->key_min == 0xFF)) {
		track->key_max = 0x3C;
		track->key_min = 0x3C;
	}
	track->band_min =
	    UMIDI20_KEY_TO_BAND_NUMBER(track->key_min);

	track->band_max =
	    UMIDI20_KEY_TO_BAND_NUMBER(track->key_max + UMIDI20_BAND_SIZE);

	UMIDI20_IF_POLL_TAIL(&(track->queue), event);
	if (event) {
		track->position_max = event->position;
	}
	for (key = 0; key < 0x80; key++) {
		event_last = last_key_press[key];
		if (event_last) {
			event_last->duration =
			    (event->position - event_last->position);
		}
	}
	return;
}
