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
 * broadcast_record.c — Recording consumers (spec §15).
 *
 */
#include "mod_broadcast.h"

static brec_type_t b_guess_rec_type(const char *target)
{
	if (zstr(target)) {
		return BREC_TYPE_FILE;
	}
	if (!strncasecmp(target, "shout://", 8)) {
		return BREC_TYPE_SHOUT;
	}
	return BREC_TYPE_FILE;
}

void *SWITCH_THREAD_FUNC broadcast_record_writer_run(switch_thread_t *thread, void *obj)
{
	rec_consumer_t *rec = (rec_consumer_t *)obj;
	broadcast_obj_t *b = rec->broadcast;
	switch_timer_t timer;
	const char *timer_name;

	(void)thread;

	timer_name = (b->profile && b->profile->timer_name) ? b->profile->timer_name : "timerfd";
	if (switch_core_timer_init(&timer, timer_name, b->interval_ms, b->samples_per_frame, rec->pool) != SWITCH_STATUS_SUCCESS) {
		if (switch_core_timer_init(&timer, "soft", b->interval_ms, b->samples_per_frame, rec->pool) != SWITCH_STATUS_SUCCESS) {
			rec->running = 0;
			return NULL;
		}
	}

	while (rec->running && !broadcast_destroyed_load_acq(b)) {
		uint64_t pseq;

		switch_core_timer_next(&timer);
		pseq = __atomic_load_n(&b->producer_seq, __ATOMIC_ACQUIRE);
		while (rec->consumer_seq < pseq && rec->running) {
			ring_frame_t *slot = &b->ring[rec->consumer_seq % b->ring_size];
			if (__atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE) == rec->consumer_seq) {
				switch_size_t len = slot->samples;
				if (switch_core_file_write(&rec->fh, slot->data, &len) != SWITCH_STATUS_SUCCESS) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "broadcast record write error\n");
					rec->running = 0;
					broadcast_enqueue_event(b, BEVT_RECORDING_STOP, 0, "write-error");
					goto end;
				}
			}
			rec->consumer_seq++;
		}
	}

end:
	switch_core_file_close(&rec->fh);
	switch_core_timer_destroy(&timer);
	return NULL;
}

/*
 * broadcast_record_start — Append recording consumer (spec §15.2).
 * lock order: recording_lock only; caller must not hold recording_lock.
 */
switch_status_t broadcast_record_start(broadcast_obj_t *b, const char *target, switch_core_session_t *expand_session)
{
	rec_consumer_t *rec;
	switch_memory_pool_t *pool = NULL;
	switch_threadattr_t *attr = NULL;
	char *expanded = NULL;
	const char *path = target;

	if (!b || zstr(target)) {
		return SWITCH_STATUS_FALSE;
	}

	if (expand_session) {
		expanded = switch_channel_expand_variables(switch_core_session_get_channel(expand_session), target);
		if (expanded) {
			path = expanded;
		}
	}

	if (switch_core_new_memory_pool(&pool) != SWITCH_STATUS_SUCCESS) {
		switch_safe_free(expanded);
		return SWITCH_STATUS_FALSE;
	}

	rec = switch_core_alloc(pool, sizeof(*rec));
	memset(rec, 0, sizeof(*rec));
	rec->pool = pool;
	rec->broadcast = b;
	rec->target = switch_core_strdup(pool, path);
	rec->type = b_guess_rec_type(path);
	rec->fh.channels = b->channels;
	rec->fh.native_rate = b->rate;

	if (switch_core_file_open(&rec->fh, path, b->channels, b->rate,
							  SWITCH_FILE_FLAG_WRITE | SWITCH_FILE_DATA_SHORT, pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "broadcast_record_start: cannot open %s\n", path);
		switch_core_destroy_memory_pool(&pool);
		switch_safe_free(expanded);
		return SWITCH_STATUS_FALSE;
	}

	rec->consumer_seq = __atomic_load_n(&b->producer_seq, __ATOMIC_ACQUIRE);
	rec->running = 1;

	switch_thread_rwlock_wrlock(b->recording_lock);
	rec->next = b->recording_head;
	b->recording_head = rec;
	switch_thread_rwlock_unlock(b->recording_lock);

	switch_threadattr_create(&attr, pool);
	switch_threadattr_stacksize_set(attr, SWITCH_THREAD_STACKSIZE);
	if (switch_thread_create(&rec->writer_thread, attr, broadcast_record_writer_run, rec, pool) != SWITCH_STATUS_SUCCESS) {
		switch_thread_rwlock_wrlock(b->recording_lock);
		b->recording_head = rec->next;
		switch_thread_rwlock_unlock(b->recording_lock);
		switch_core_file_close(&rec->fh);
		switch_core_destroy_memory_pool(&pool);
		switch_safe_free(expanded);
		return SWITCH_STATUS_FALSE;
	}

	broadcast_cflags_or_rel(b, BFLAG_RECORDING);
	broadcast_enqueue_event(b, BEVT_RECORDING_START, 0, path);
	switch_safe_free(expanded);
	return SWITCH_STATUS_SUCCESS;
}

/*
 * broadcast_record_stop — Stop one or all recordings matching target (NULL = all).
 */
switch_status_t broadcast_record_stop(broadcast_obj_t *b, const char *target)
{
	rec_consumer_t *cur, *prev, *next, *stopped = NULL;
	switch_status_t jst;
	switch_status_t st = SWITCH_STATUS_FALSE;

	switch_thread_rwlock_wrlock(b->recording_lock);
	prev = NULL;
	cur = b->recording_head;
	while (cur) {
		next = cur->next;
		if (target == NULL || target[0] == '\0' || !strcmp(cur->target, target)) {
			if (prev) {
				prev->next = next;
			} else {
				b->recording_head = next;
			}
			cur->next = stopped;
			stopped = cur;
			cur->running = 0;
			st = SWITCH_STATUS_SUCCESS;
		} else {
			prev = cur;
		}
		cur = next;
	}
	if (!b->recording_head) {
		broadcast_cflags_and_rel(b, (uint32_t)~BFLAG_RECORDING);
	}
	switch_thread_rwlock_unlock(b->recording_lock);

	while (stopped) {
		rec_consumer_t *r = stopped;
		stopped = r->next;
		r->next = NULL;
		if (r->writer_thread) {
			switch_thread_join(&jst, r->writer_thread);
		}
		broadcast_enqueue_event(b, BEVT_RECORDING_STOP, 0, NULL);
		switch_core_destroy_memory_pool(&r->pool);
	}
	return st;
}

void broadcast_record_stop_all(broadcast_obj_t *b)
{
	broadcast_record_stop(b, NULL);
}
