/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2026, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Module: mod_broadcast — One-to-many audio broadcast for very large scale
 *                        (lectures, sermons, town halls, public addresses,
 *                        training sessions, broadcasts).
 *
 * Author: Faisal Hanif <imfanee@gmail.com>
 *
 * broadcast_producer.c — Single-producer tick loop and ring publish (spec §6.2, §8.3).
 *
 */
#include "mod_broadcast.h"

/* Apply silence_policy string on broadcast (zero|cn|moh:...) into dst */
static void b_apply_silence_policy(broadcast_obj_t *b, uint8_t *dst, uint32_t *datalen_out)
{
	const char *pol = b->silence_policy;
	uint32_t bpf = b->bytes_per_frame;
	uint32_t spf = b->samples_per_frame;
	uint32_t ch = b->channels;

	if (zstr(pol) || !strcasecmp(pol, "zero")) {
		if (b->silence_zero_buf) {
			memcpy(dst, b->silence_zero_buf, bpf);
		} else {
			switch_generate_sln_silence((int16_t *)dst, spf, ch, (uint32_t)-1);
		}
		*datalen_out = bpf;
		return;
	}
	if (!strcasecmp(pol, "cn")) {
		uint32_t div = b->profile && b->profile->comfort_noise_level ? b->profile->comfort_noise_level : 1400;
		switch_generate_sln_silence((int16_t *)dst, spf, ch, div * (b->rate / 8000));
		*datalen_out = bpf;
		return;
	}
	if (!strncasecmp(pol, "moh:", 4)) {
		/* SAMPLES, not bytes -- same units bug as the play_fh path below:
		 * requesting bpf samples read twice the audio per tick, so MoH played
		 * at double speed and could over-run dst on wide profiles. */
		switch_size_t want = spf;
		switch_size_t rlen = 0;
		switch_status_t r;

		switch_mutex_lock(b->moh_mutex);
		if (!b->moh_open) {
			const char *path = pol + 4;
			memset(&b->moh_fh, 0, sizeof(b->moh_fh));
			if (switch_core_file_open(&b->moh_fh, path, ch, b->rate,
									  SWITCH_FILE_FLAG_READ | SWITCH_FILE_DATA_SHORT, b->pool) == SWITCH_STATUS_SUCCESS) {
				b->moh_open = SWITCH_TRUE;
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "MoH open failed: %s\n", path);
			}
		}
		if (b->moh_open) {
			unsigned int moh_pos = 0;
			rlen = want;
			r = switch_core_file_read(&b->moh_fh, dst, &rlen);
			if (r != SWITCH_STATUS_SUCCESS || rlen < (switch_size_t) want) {
				switch_core_file_seek(&b->moh_fh, &moh_pos, 0, SEEK_SET);
				rlen = want;
				switch_core_file_read(&b->moh_fh, dst, &rlen);
			}
		}
		switch_mutex_unlock(b->moh_mutex);
		if (rlen < (switch_size_t) want) {
			if (b->silence_zero_buf) {
				memcpy(dst, b->silence_zero_buf, bpf);
			} else {
				switch_generate_sln_silence((int16_t *)dst, spf, ch, (uint32_t)-1);
			}
		}
		*datalen_out = bpf;
		return;
	}
	if (b->silence_zero_buf) {
		memcpy(dst, b->silence_zero_buf, bpf);
	} else {
		switch_generate_sln_silence((int16_t *)dst, spf, ch, (uint32_t)-1);
	}
	*datalen_out = bpf;
}

/*
 * broadcast_producer_run — Producer thread entry (spec §6.2).
 * lock order: play_mutex may precede speaker_lock; audio_in_mutex taken while holding speaker rdlock (spec §7.2: 3 then 6).
 */
void *SWITCH_THREAD_FUNC broadcast_producer_run(switch_thread_t *thread, void *obj)
{
	broadcast_obj_t *b = (broadcast_obj_t *)obj;
	switch_timer_t *timer = &b->producer_tick_timer;
	const char *timer_name;
	switch_time_t work_start, work_end;
	uint64_t seq;
	ring_frame_t *slot;
	member_obj_t *spk;
	switch_size_t got;
	uint8_t local_data[BROADCAST_MAX_FRAME_BYTES];
	uint32_t datalen;
	uint8_t silence_flag;
	uint32_t cflags_snap;

	(void)thread;

	timer_name = (b->profile && b->profile->timer_name) ? b->profile->timer_name : "timerfd";
	if (switch_core_timer_init(timer, timer_name, b->interval_ms, b->samples_per_frame, b->pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "broadcast '%s': timer '%s' failed, trying soft\n", b->name, timer_name);
		if (switch_core_timer_init(timer, "soft", b->interval_ms, b->samples_per_frame, b->pool) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "broadcast '%s': timer init failed\n", b->name);
			broadcast_producer_running_store_rel(b, 0);
			return NULL;
		}
	}

	while (broadcast_producer_running_load_acq(b) && !broadcast_destroyed_load_acq(b)) {
		switch_core_timer_next(timer);
		{
			switch_time_t now_us = switch_micro_time_now();
			if (b->producer_first_tick_us == 0) {
				b->producer_first_tick_us = now_us;
			} else {
				/* Wall-clock drift vs ideal schedule: stat_ticks == completed producer frames before this tick. */
				uint64_t tn = (uint64_t)switch_atomic_read(&b->stat_ticks);
				switch_time_t expected = b->producer_first_tick_us +
					(switch_time_t)(tn * (switch_time_t)b->interval_ms * 1000);
				uint64_t drift;

				drift = (uint64_t)(now_us > expected ? (now_us - expected) : 0);
				if (drift > b->stat_tick_drift_us_max) {
					b->stat_tick_drift_us_max = drift;
				}
				if (drift > (uint64_t)(2 * (uint64_t)b->interval_ms * 1000)) {
					if (now_us - b->last_producer_stall_evt_us > 3000000) {
						char dbuf[24];

						b->last_producer_stall_evt_us = now_us;
						switch_snprintf(dbuf, sizeof(dbuf), "%" SWITCH_UINT64_T_FMT, drift);
						broadcast_enqueue_event(b, BEVT_PRODUCER_STALL, 0, dbuf);
					}
				}
			}
		}
		/* DESIGN NOTE: spec §28.3 measures from before timer_next; wall time includes sleep.
		 * Measure CPU work only after timer fires (spec §20.1 intent). */
		work_start = switch_micro_time_now();

		datalen = 0;
		silence_flag = 1;

		switch_mutex_lock(b->play_mutex);
		if ((broadcast_cflags_load_acq(b) & BFLAG_PLAY_FILE) && b->play_fh) {
			/* switch_core_file_read() counts SAMPLES, not bytes (see
			 * switch_core_file.c: it converts with *len * 2 * channels).
			 * Requesting bytes_per_frame samples read TWICE the audio per tick
			 * -- bpf = spf * channels * 2 -- so every played file ran at double
			 * speed and hit EOF in half its real duration, while datalen (used
			 * downstream as a BYTE count) then discarded half of what was read.
			 * It also over-ran local_data[BROADCAST_MAX_FRAME_BYTES] on wide
			 * profiles: the read was 2*bpf bytes, which exceeds 1920 for
			 * 32kHz+ mono or stereo >= 16kHz. Request samples, convert back. */
			switch_size_t want = b->samples_per_frame;
			switch_size_t rlen = want;
			if (switch_core_file_read(b->play_fh, local_data, &rlen) == SWITCH_STATUS_SUCCESS && rlen == want) {
				datalen = (uint32_t)(rlen * 2 * b->channels);
				silence_flag = 0;
			} else {
				switch_core_file_close(b->play_fh);
				b->play_fh = NULL;
				broadcast_cflags_and_rel(b, (uint32_t)~BFLAG_PLAY_FILE);
			}
		}
		switch_mutex_unlock(b->play_mutex);

		/* One consistent view of admin flags for this tick (weak-ordering safe). */
		cflags_snap = broadcast_cflags_load_acq(b);

		switch_thread_rwlock_rdlock(b->speaker_lock);
		spk = b->current_speaker;
		if (!datalen && spk && !(cflags_snap & BFLAG_PAUSE) && spk->audio_buffer && spk->audio_in_mutex) {
			if (broadcast_mflags_load_acq(spk) & MFLAG_MUTED_SPEAKER) {
				/* Speaker is muted. Drain the buffer to prevent lag when unmuting, but generate silence. */
				switch_mutex_lock(spk->audio_in_mutex);
				switch_buffer_zero(spk->audio_buffer);
				switch_mutex_unlock(spk->audio_in_mutex);
			} else {
				switch_mutex_lock(spk->audio_in_mutex);
				got = switch_buffer_read(spk->audio_buffer, local_data, b->bytes_per_frame);
				switch_mutex_unlock(spk->audio_in_mutex);
				if (got == (switch_size_t)b->bytes_per_frame) {
					datalen = (uint32_t)got;
					silence_flag = 0;
					if (spk->vol_idx != 0) {
						switch_change_sln_volume_granular((int16_t *)local_data, datalen / 2, spk->vol_idx);
					}
				}
			}
		}
		switch_thread_rwlock_unlock(b->speaker_lock);

		if (silence_flag || (cflags_snap & BFLAG_PAUSE)) {
			b_apply_silence_policy(b, local_data, &datalen);
			silence_flag = 1;
		}

		seq = __atomic_load_n(&b->producer_seq, __ATOMIC_RELAXED);
		slot = &b->ring[seq % b->ring_size];
		slot->datalen = datalen;
		slot->samples = b->samples_per_frame;
		slot->silence = silence_flag;
		memcpy(slot->data, local_data, datalen);
		__atomic_store_n(&slot->seq, seq, __ATOMIC_RELEASE);
		__atomic_store_n(&b->producer_seq, seq + 1, __ATOMIC_RELEASE);

		switch_atomic_inc(&b->stat_ticks);
		if (silence_flag) {
			switch_atomic_inc(&b->stat_speaker_silent_ticks);
		}

		work_end = switch_micro_time_now();
		{
			switch_time_t d = work_end - work_start;
			if (d > b->producer_max_tick_us) {
				b->producer_max_tick_us = d;
				b->producer_max_tick_seen_us = work_end;
			}
			b->producer_tick_sum_us += (uint64_t)d;
			b->producer_tick_count_for_avg++;
#ifdef BROADCAST_ENABLE_HISTOGRAM
			if (d <= 50) {
				b->producer_tick_hist.bucket_us_50++;
			} else if (d <= 100) {
				b->producer_tick_hist.bucket_us_100++;
			} else if (d <= 200) {
				b->producer_tick_hist.bucket_us_200++;
			} else if (d <= 500) {
				b->producer_tick_hist.bucket_us_500++;
			} else if (d <= 1000) {
				b->producer_tick_hist.bucket_us_1000++;
			} else if (d <= 5000) {
				b->producer_tick_hist.bucket_us_5000++;
			} else if (d <= 20000) {
				b->producer_tick_hist.bucket_us_20000++;
			} else {
				b->producer_tick_hist.bucket_us_over++;
			}
#endif
			/* DESIGN NOTE: measures producer CPU after timer_next, not wall timer drift (see runbook). */
			if (d > (switch_time_t)(b->interval_ms * 1000 * 2)) {
				switch_atomic_inc(&b->stat_missed_ticks);
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
								  "broadcast '%s' producer tick over budget: %" SWITCH_TIME_T_FMT " us\n", b->name, d);
			}
		}
	}

	switch_core_timer_destroy(timer);
	broadcast_producer_running_store_rel(b, 0);
	return NULL;
}
