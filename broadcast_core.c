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
 * broadcast_core.c — broadcast_obj_t lifecycle, global hash, housekeeping.
 *
 */
#include "mod_broadcast.h"
#include <stdlib.h>
#include <string.h>

struct broadcast_globals broadcast_globals = {0};

static char *b_expand_auto_record_template(broadcast_obj_t *b)
{
	switch_event_t *ev = NULL;
	char *xp = NULL;

	if (!b || !b->profile || zstr(b->profile->record_template)) {
		return NULL;
	}
	/* DESIGN NOTE: minimal event for expand when no channel session — supports ${broadcast_name}, ${name}, ${uuid}, ${strftime(...)}. */
	if (switch_event_create(&ev, SWITCH_EVENT_CHANNEL_DATA) != SWITCH_STATUS_SUCCESS) {
		return NULL;
	}
	switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "broadcast_name", b->name);
	switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "name", b->name);
	switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "uuid", b->uuid_str);
	xp = switch_event_expand_headers(ev, b->profile->record_template);
	switch_event_destroy(&ev);
	return xp;
}

void broadcast_hash_rdlock(void)
{
	switch_thread_rwlock_rdlock(broadcast_globals.hash_rwlock);
}

void broadcast_hash_rdunlock(void)
{
	switch_thread_rwlock_unlock(broadcast_globals.hash_rwlock);
}

void broadcast_hash_wrlock(void)
{
	switch_thread_rwlock_wrlock(broadcast_globals.hash_rwlock);
}

void broadcast_hash_wrunlock(void)
{
	switch_thread_rwlock_unlock(broadcast_globals.hash_rwlock);
}

/*
 * broadcast_find — Lookup broadcast by name under hash read lock.
 * Caller must not hold hash_rwlock; this function acquires rdlock internally.
 */
broadcast_obj_t *broadcast_find(const char *name)
{
	broadcast_obj_t *b;
	if (zstr(name)) {
		return NULL;
	}
	switch_thread_rwlock_rdlock(broadcast_globals.hash_rwlock);
	b = (broadcast_obj_t *) switch_core_hash_find(broadcast_globals.broadcast_hash, name);
	switch_thread_rwlock_unlock(broadcast_globals.hash_rwlock);
	return b;
}

/*
 * broadcast_find_and_ref — Like broadcast_find, but holds a live reference until broadcast_release.
 * Refused while destroy/teardown is in progress so callers never pin a dying broadcast_obj_t.
 */
broadcast_obj_t *broadcast_find_and_ref(const char *name)
{
	broadcast_obj_t *b;

	if (zstr(name)) {
		return NULL;
	}
	switch_thread_rwlock_rdlock(broadcast_globals.hash_rwlock);
	b = (broadcast_obj_t *) switch_core_hash_find(broadcast_globals.broadcast_hash, name);
	if (b) {
		if (broadcast_destroyed_load_acq(b) || (broadcast_cflags_load_acq(b) & BFLAG_DESTRUCT)) {
			b = NULL;
		} else {
			(void) __atomic_fetch_add((int32_t *)(void *)&b->live_refs, 1, __ATOMIC_ACQ_REL);
		}
	}
	switch_thread_rwlock_unlock(broadcast_globals.hash_rwlock);
	return b;
}

void broadcast_release(broadcast_obj_t *b)
{
	if (!b) {
		return;
	}
	(void) __atomic_fetch_sub((int32_t *)(void *)&b->live_refs, 1, __ATOMIC_ACQ_REL);
}

/*
 * broadcast_create — Allocate broadcast, start producer thread, insert hash.
 * parent_pool: optional (may be NULL); per-broadcast pool is always created.
 * optional_session_for_expand: used for record-template expansion on auto-record (may be NULL).
 */
broadcast_obj_t *broadcast_create(const char *name, const char *profile_name, switch_memory_pool_t *parent_pool,
								  switch_core_session_t *optional_session_for_expand)
{
	switch_memory_pool_t *pool = NULL;
	broadcast_obj_t *b;
	broadcast_profile_t *prof;
	switch_uuid_t uuid;
	uint32_t spf, bpf;
	switch_threadattr_t *thd_attr = NULL;
	switch_status_t st;

	(void)parent_pool;

	if (zstr(name)) {
		return NULL;
	}

	if (switch_core_new_memory_pool(&pool) != SWITCH_STATUS_SUCCESS) {
		return NULL;
	}

	/* DESIGN NOTE: We must hold config_rwlock rdlock while looking up the profile via
	 * broadcast_config_find_profile and cloning it to protect against a concurrent
	 * config reload race. Once cloned, all profile strings are copied into the
	 * broadcast's own memory pool, making it independent of reload. */
	switch_thread_rwlock_rdlock(broadcast_globals.config_rwlock);
	{
		broadcast_profile_t *p_template = broadcast_config_find_profile(profile_name);
		prof = p_template ? broadcast_profile_clone(p_template, pool) : NULL;
	}
	switch_thread_rwlock_unlock(broadcast_globals.config_rwlock);

	if (!prof) {
		switch_core_destroy_memory_pool(&pool);
		return NULL;
	}

	spf = (prof->rate * prof->interval_ms) / 1000;
	bpf = spf * prof->channels * 2;
	if (bpf > BROADCAST_MAX_FRAME_BYTES) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "broadcast frame size %u exceeds BROADCAST_MAX_FRAME_BYTES; reduce rate/channels (spec §16.6)\n", bpf);
		switch_core_destroy_memory_pool(&pool);
		return NULL;
	}

	b = switch_core_alloc(pool, sizeof(*b));
	memset(b, 0, sizeof(*b));
	b->pool = pool;
	b->name = switch_core_strdup(pool, name);
	b->profile = prof;
	b->rate = prof->rate;
	b->interval_ms = prof->interval_ms;
	b->channels = (uint8_t) prof->channels;
	b->samples_per_frame = spf;
	b->bytes_per_frame = bpf;
	b->ring_size = prof->ring_size ? prof->ring_size : BROADCAST_DEFAULT_RING_SIZE;
	b->silence_policy = switch_core_strdup(pool, prof->silence_policy);

	switch_uuid_get(&uuid);
	switch_uuid_format(b->uuid_str, &uuid);

	switch_thread_rwlock_create(&b->speaker_lock, pool);
	switch_thread_rwlock_create(&b->listener_lock, pool);
	switch_thread_rwlock_create(&b->recording_lock, pool);
	switch_mutex_init(&b->control_mutex, SWITCH_MUTEX_NESTED, pool);
	switch_mutex_init(&b->id_mutex, SWITCH_MUTEX_NESTED, pool);
	switch_mutex_init(&b->event_mutex, SWITCH_MUTEX_NESTED, pool);
	switch_mutex_init(&b->play_mutex, SWITCH_MUTEX_NESTED, pool);
	switch_mutex_init(&b->moh_mutex, SWITCH_MUTEX_NESTED, pool);

	b->event_ring_size = broadcast_globals.event_ring_size ? broadcast_globals.event_ring_size : BROADCAST_EVENT_RING_SIZE;
	b->event_ring = switch_core_alloc(pool, sizeof(evt_msg_t) * b->event_ring_size);

	b->ring = switch_core_alloc(pool, sizeof(ring_frame_t) * b->ring_size);
	/* Pre-zeroed frame; producer b_apply_silence_policy memcpy first bytes_per_frame (zero / MoH pad / default). */
	b->silence_zero_buf = switch_core_alloc(pool, BROADCAST_MAX_FRAME_BYTES);
	memset(b->silence_zero_buf, 0, BROADCAST_MAX_FRAME_BYTES);

	b->created_at = switch_micro_time_now();
	broadcast_cflags_or_rel(b, BFLAG_RUNNING);
	__atomic_store_n(&b->producer_seq, 0, __ATOMIC_RELEASE);

	switch_threadattr_create(&thd_attr, pool);
	switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);

	switch_thread_rwlock_wrlock(broadcast_globals.hash_rwlock);
	if (switch_core_hash_find(broadcast_globals.broadcast_hash, name)) {
		switch_thread_rwlock_unlock(broadcast_globals.hash_rwlock);
		switch_core_destroy_memory_pool(&pool);
		return NULL;
	}
	if ((uint32_t) switch_atomic_read(&broadcast_globals.active_broadcasts) >= broadcast_globals.max_broadcasts) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "max-broadcasts exceeded\n");
		switch_thread_rwlock_unlock(broadcast_globals.hash_rwlock);
		switch_core_destroy_memory_pool(&pool);
		return NULL;
	}
	broadcast_producer_running_store_rel(b, 1);
	st = switch_thread_create(&b->producer_thread, thd_attr, broadcast_producer_run, b, pool);
	if (st != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "producer thread create failed\n");
		broadcast_producer_running_store_rel(b, 0);
		switch_thread_rwlock_unlock(broadcast_globals.hash_rwlock);
		switch_core_destroy_memory_pool(&pool);
		return NULL;
	}
	switch_core_hash_insert(broadcast_globals.broadcast_hash, b->name, b);
	switch_atomic_inc(&broadcast_globals.active_broadcasts);
	switch_thread_rwlock_unlock(broadcast_globals.hash_rwlock);

	/* Creator (dialplan session) holds one ref until broadcast_release at end of broadcast_app_function. */
	__atomic_store_n((int32_t *)(void *)&b->live_refs, 1, __ATOMIC_RELEASE);

	switch_atomic_inc(&broadcast_globals.total_broadcasts_created);
	broadcast_fire_critical_event(b, BEVT_BROADCAST_CREATE, 0, NULL);

	if (b->profile->auto_record == SWITCH_TRUE) {
		char *expanded = NULL;

		if (optional_session_for_expand) {
			expanded = switch_channel_expand_variables(switch_core_session_get_channel(optional_session_for_expand),
													 b->profile->record_template);
		} else {
			expanded = b_expand_auto_record_template(b);
		}
		if (expanded) {
			if (broadcast_record_start(b, expanded, optional_session_for_expand) != SWITCH_STATUS_SUCCESS) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
								  "broadcast %s: auto_record failed for path (see recording-failed event)\n", b->name);
				broadcast_enqueue_event(b, BEVT_RECORDING_FAILED, 0, expanded);
			}
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "broadcast %s: auto_record template expand failed (no session context)\n", b->name);
			broadcast_enqueue_event(b, BEVT_RECORDING_FAILED, 0, "expand-failed");
		}
		switch_safe_free(expanded);
	}

	return b;
}

/* Hang up via session locate so we never dereference member_obj after releasing listener_lock (destroy-safe). */
static void b_hangup_listener_session_by_uuid(const char *uuid, const char *reason, switch_call_cause_t cause)
{
	switch_core_session_t *ls;
	switch_channel_t *ch;

	if (zstr(uuid)) {
		return;
	}
	ls = switch_core_session_locate(uuid);
	if (!ls) {
		return;
	}
	ch = switch_core_session_get_channel(ls);
	if (ch) {
		switch_channel_set_variable(ch, "broadcast_left_reason", reason);
		switch_channel_hangup(ch, cause);
	}
	switch_core_session_rwunlock(ls);
}

/*
 * Set KICKED then hang up. Hangup uses switch_core_session_locate(m->uuid) when uuid is set so the channel
 * is resolved under core session refcounting (avoids stale m->channel after narrow races).
 * MUST NOT unlink from listener_head.
 */
void broadcast_kick_listener(member_obj_t *m, const char *reason)
{
	broadcast_obj_t *b;

	if (!m || broadcast_member_role_load_acq(m) != BMEMBER_ROLE_LISTENER) {
		return;
	}
	b = m->broadcast;
	broadcast_mflags_or_rel(m, MFLAG_KICKED);
	if (b) {
		broadcast_enqueue_event(b, BEVT_LISTENER_KICKED, m->id, NULL);
	}
	if (!zstr(m->uuid)) {
		b_hangup_listener_session_by_uuid(m->uuid, reason, SWITCH_CAUSE_NORMAL_CLEARING);
		return;
	}
	if (m->channel) {
		switch_channel_set_variable(m->channel, "broadcast_left_reason", reason);
		switch_channel_hangup(m->channel, SWITCH_CAUSE_NORMAL_CLEARING);
	}
}

void broadcast_kick_all_listeners(broadcast_obj_t *b)
{
	member_obj_t *m;
	uint32_t nk_total = 0, nk_proc = 0, i;
	char **uuid_snap = NULL;
	char *uuid_stack[BROADCAST_DESTROY_KICK_STACK];
	int uuid_snap_malloc = 0;

	if (!b) {
		return;
	}

	switch_thread_rwlock_wrlock(b->listener_lock);
	for (m = b->listener_head; m; m = m->next) {
		nk_total++;
	}
	nk_proc = nk_total;
	if (nk_total) {
		if (nk_total <= BROADCAST_DESTROY_KICK_STACK) {
			uuid_snap = uuid_stack;
		} else {
			uuid_snap = (char **)malloc(sizeof(*uuid_snap) * nk_total);
			if (uuid_snap) {
				uuid_snap_malloc = 1;
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
								  "broadcast %s kick_all: OOM for uuid snapshot (%u); kicking first %u only\n",
								  b->name, nk_total, (unsigned)BROADCAST_DESTROY_KICK_STACK);
				uuid_snap = uuid_stack;
				nk_proc = BROADCAST_DESTROY_KICK_STACK;
			}
		}
		if (uuid_snap) {
			i = 0;
			for (m = b->listener_head; m && i < nk_proc; m = m->next) {
				broadcast_mflags_or_rel(m, MFLAG_KICKED);
				broadcast_enqueue_event(b, BEVT_LISTENER_KICKED, m->id, NULL);
				uuid_snap[i++] = (m->uuid && *m->uuid) ? switch_core_strdup(b->pool, m->uuid) : switch_core_strdup(b->pool, "");
			}
		}
	}
	switch_thread_rwlock_unlock(b->listener_lock);

	for (i = 0; i < nk_proc; i++) {
		if (uuid_snap[i] && *uuid_snap[i]) {
			b_hangup_listener_session_by_uuid(uuid_snap[i], "kicked", SWITCH_CAUSE_NORMAL_CLEARING);
		}
	}
	if (uuid_snap_malloc) {
		free(uuid_snap);
	}
}

/*
 * broadcast_destroy_ex — Teardown broadcast (spec §17.3 order).
 * @param grace_ms optional wall-clock wait (milliseconds) after optional announcement before hard teardown.
 * @param announce_file optional WAV path played via producer play path during grace (NULL = silence only).
 * lock order: hash_wrlock -> control_mutex -> speaker -> listener -> recording (§7.2).
 */
switch_status_t broadcast_destroy_ex(broadcast_obj_t *b, uint32_t grace_ms, const char *announce_file)
{
	switch_status_t join_status;
	member_obj_t *m;

	if (!b) {
		return SWITCH_STATUS_FALSE;
	}

	if (grace_ms > 60000) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "broadcast %s destroy: grace_ms %u exceeds limit; clamping to 60000\n", b->name, grace_ms);
		grace_ms = 60000;
	}

	/* DESIGN NOTE: BFLAG_DESTRUCT first so find_and_ref rejects new pins; destroyed stays 0 during grace
	 * so listener bridges keep running and hear producer-driven announcement playback. */
	switch_mutex_lock(b->control_mutex);
	broadcast_cflags_or_rel(b, BFLAG_DESTRUCT);
	switch_mutex_unlock(b->control_mutex);

	if (grace_ms > 0) {
		char grace_evt_data[512];
		switch_snprintf(grace_evt_data, sizeof(grace_evt_data), "%u|%s", grace_ms, announce_file ? announce_file : "");
		broadcast_enqueue_event(b, BEVT_DESTROY_GRACE, 0, grace_evt_data);

		if (!zstr(announce_file)) {
			switch_mutex_lock(b->play_mutex);
			if (b->play_fh) {
				switch_core_file_close(b->play_fh);
				b->play_fh = NULL;
			}
			b->play_fh = switch_core_alloc(b->pool, sizeof(switch_file_handle_t));
			memset(b->play_fh, 0, sizeof(*b->play_fh));
			if (switch_core_file_open(b->play_fh, announce_file, b->channels, b->rate,
									  SWITCH_FILE_FLAG_READ | SWITCH_FILE_DATA_SHORT, b->pool) == SWITCH_STATUS_SUCCESS) {
				broadcast_cflags_or_rel(b, BFLAG_PLAY_FILE);
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
								  "broadcast %s destroy: could not open announcement %s\n", b->name, announce_file);
				b->play_fh = NULL;
			}
			switch_mutex_unlock(b->play_mutex);
		}
		switch_sleep((switch_interval_time_t)grace_ms * 1000000);
		switch_mutex_lock(b->play_mutex);
		if (b->play_fh) {
			switch_core_file_close(b->play_fh);
			b->play_fh = NULL;
		}
		broadcast_cflags_and_rel(b, (uint32_t)~BFLAG_PLAY_FILE);
		switch_mutex_unlock(b->play_mutex);
	}

	switch_mutex_lock(b->control_mutex);
	broadcast_destroyed_store_rel(b, 1);
	switch_mutex_unlock(b->control_mutex);

	/* Stop play file (idempotent if grace cleared it) */
	switch_mutex_lock(b->play_mutex);
	if (b->play_fh) {
		switch_core_file_close(b->play_fh);
		b->play_fh = NULL;
	}
	broadcast_cflags_and_rel(b, (uint32_t)~BFLAG_PLAY_FILE);
	switch_mutex_unlock(b->play_mutex);

	switch_mutex_lock(b->moh_mutex);
	if (b->moh_open) {
		switch_core_file_close(&b->moh_fh);
		b->moh_open = SWITCH_FALSE;
	}
	switch_mutex_unlock(b->moh_mutex);

	broadcast_record_stop_all(b);

	{
		member_obj_t *old_speaker = NULL;

		switch_thread_rwlock_wrlock(b->speaker_lock);
		if (b->current_speaker) {
			old_speaker = b->current_speaker;
			b->current_speaker = NULL;
		}
		switch_thread_rwlock_unlock(b->speaker_lock);
		if (old_speaker) {
			broadcast_speaker_stop_input(old_speaker);
		}

		{
			/*
			 * Under listener_lock rdlock: set MFLAG_KICKED and copy each UUID into broadcast pool.
			 * After unlock, hang up via switch_core_session_locate only (no member_obj deref) — closes
			 * snapshot→kick UAF if the session outlives the stale pointer window.
			 */
			uint32_t nk_total = 0, nk_proc = 0, i;
			char **uuid_snap = NULL;
			char *uuid_stack[BROADCAST_DESTROY_KICK_STACK];
			int uuid_snap_malloc = 0;

			switch_thread_rwlock_rdlock(b->listener_lock);
			for (m = b->listener_head; m; m = m->next) {
				nk_total++;
			}
			nk_proc = nk_total;
			if (nk_total) {
				if (nk_total <= BROADCAST_DESTROY_KICK_STACK) {
					uuid_snap = uuid_stack;
				} else {
					uuid_snap = (char **)malloc(sizeof(*uuid_snap) * nk_total);
					if (uuid_snap) {
						uuid_snap_malloc = 1;
					} else {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
										  "broadcast %s destroy: OOM for uuid snapshot (%u); kicking first %u listeners only\n",
										  b->name, nk_total, (unsigned)BROADCAST_DESTROY_KICK_STACK);
						uuid_snap = uuid_stack;
						nk_proc = BROADCAST_DESTROY_KICK_STACK;
					}
				}
				if (uuid_snap) {
					i = 0;
					for (m = b->listener_head; m && i < nk_proc; m = m->next) {
						broadcast_mflags_or_rel(m, MFLAG_KICKED);
						uuid_snap[i++] = (m->uuid && *m->uuid) ? switch_core_strdup(b->pool, m->uuid) : switch_core_strdup(b->pool, "");
					}
				}
			}
			switch_thread_rwlock_unlock(b->listener_lock);

			for (i = 0; i < nk_proc; i++) {
				if (uuid_snap[i] && *uuid_snap[i]) {
					/* DESIGN NOTE: FS 1.10 has no SWITCH_CAUSE_ADMINISTRATIVE_BLOCK; SERVICE_UNAVAILABLE marks mass teardown in CDRs. */
					b_hangup_listener_session_by_uuid(uuid_snap[i], "destroyed", SWITCH_CAUSE_SERVICE_UNAVAILABLE);
				}
			}
			if (uuid_snap_malloc) {
				free(uuid_snap);
			}
		}

		{
			switch_time_t deadline = switch_micro_time_now() + 10000000;
			for (;;) {
				uint32_t lc = 0;
				int has_sp = 0;
				int speaker_wait = 0;

				if (switch_micro_time_now() > deadline) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
									  "broadcast %s destroy: timeout waiting for members to exit\n", b->name);
					break;
				}
				switch_thread_rwlock_rdlock(b->listener_lock);
				lc = b->listener_count;
				switch_thread_rwlock_unlock(b->listener_lock);
				switch_thread_rwlock_rdlock(b->speaker_lock);
				has_sp = b->current_speaker ? 1 : 0;
				switch_thread_rwlock_unlock(b->speaker_lock);
				if (old_speaker) {
					speaker_wait = broadcast_member_in_speaker_wait_load_acq(old_speaker) ? 1 : 0;
				}
				if (lc == 0 && !has_sp && !speaker_wait) {
					break;
				}
				switch_yield(20000);
			}
		}

		{
			/* Wait until at most one ref remains (the caller's from broadcast_find_and_ref, or destroy_all's bump). */
			switch_time_t drefs = switch_micro_time_now() + 10000000;
			for (;;) {
				int32_t lr = __atomic_load_n((int32_t *)(void *)&b->live_refs, __ATOMIC_ACQUIRE);
				if (lr <= 1) {
					break;
				}
				if (switch_micro_time_now() > drefs) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
									  "broadcast %s destroy: timeout waiting for live_refs=%d\n", b->name, (int) lr);
					break;
				}
				switch_yield(20000);
			}
		}
	}

	broadcast_producer_running_store_rel(b, 0);
	if (b->producer_thread) {
		switch_thread_join(&join_status, b->producer_thread);
		b->producer_thread = NULL;
	}

	switch_thread_rwlock_wrlock(broadcast_globals.hash_rwlock);
	switch_core_hash_delete(broadcast_globals.broadcast_hash, b->name);
	switch_atomic_dec(&broadcast_globals.active_broadcasts);
	switch_thread_rwlock_unlock(broadcast_globals.hash_rwlock);

	broadcast_fire_critical_event(b, BEVT_BROADCAST_DESTROY, 0, NULL);

	switch_core_destroy_memory_pool(&b->pool);
	return SWITCH_STATUS_SUCCESS;
}

void broadcast_destroy_all(void)
{
	for (;;) {
		switch_hash_index_t *hi;
		broadcast_obj_t *b = NULL;
		void *val = NULL;

		switch_thread_rwlock_wrlock(broadcast_globals.hash_rwlock);
		hi = switch_core_hash_first(broadcast_globals.broadcast_hash);
		if (hi) {
			switch_core_hash_this(hi, NULL, NULL, &val);
			b = (broadcast_obj_t *) val;
			if (b) {
				(void) __atomic_fetch_add((int32_t *)(void *)&b->live_refs, 1, __ATOMIC_ACQ_REL);
			}
		}
		switch_thread_rwlock_unlock(broadcast_globals.hash_rwlock);
		if (!b) {
			break;
		}
		broadcast_destroy(b);
	}
}

void broadcast_runtime_housekeeping(void)
{
	switch_hash_index_t *hi;
	switch_time_t now = switch_micro_time_now();

	switch_thread_rwlock_rdlock(broadcast_globals.hash_rwlock);
	for (hi = switch_core_hash_first(broadcast_globals.broadcast_hash); hi; hi = switch_core_hash_next(&hi)) {
		void *val = NULL;
		broadcast_obj_t *b;

		switch_core_hash_this(hi, NULL, NULL, &val);
		b = (broadcast_obj_t *) val;
		if (!b || broadcast_destroyed_load_acq(b)) {
			continue;
		}
		if (!broadcast_producer_running_load_acq(b) && !(broadcast_cflags_load_acq(b) & BFLAG_DESTRUCT)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "broadcast %s producer thread stopped unexpectedly\n", b->name);
		}

		/* Heartbeat (profile heartbeat_interval_sec; 0 = disabled) */
		if (b->profile && b->profile->heartbeat_interval_sec > 0) {
			switch_time_t interval_us = (switch_time_t)b->profile->heartbeat_interval_sec * 1000000;
			if (b->last_heartbeat_us == 0) {
				b->last_heartbeat_us = now;
			} else if (now - b->last_heartbeat_us >= interval_us) {
				char databuf[256];
				uint32_t spid = 0;
				b->last_heartbeat_us = now;
				switch_thread_rwlock_rdlock(b->speaker_lock);
				if (b->current_speaker) {
					spid = b->current_speaker->id;
				}
				switch_thread_rwlock_unlock(b->speaker_lock);
				if (b->profile->verbose_events) {
					switch_snprintf(databuf, sizeof(databuf),
									"listeners=%u peak=%u speaker_id=%u ticks=%u missed=%u drift_max_us=%" SWITCH_UINT64_T_FMT
									" resyncs=%u dropped=%u",
									b->listener_count, b->stat_peak_listeners, spid,
									(uint32_t)switch_atomic_read(&b->stat_ticks),
									(uint32_t)switch_atomic_read(&b->stat_missed_ticks),
									(uint64_t)b->stat_tick_drift_us_max,
									(uint32_t)switch_atomic_read(&b->stat_listener_resyncs),
									(uint32_t)switch_atomic_read(&b->stat_events_dropped));
				} else {
					switch_snprintf(databuf, sizeof(databuf), "listeners=%u speaker_id=%u", b->listener_count, spid);
				}
				broadcast_enqueue_event(b, BEVT_HEARTBEAT, 0, databuf);
			}
		}

		/* Wedged listener detection (no seq advance for >5s while still running) */
		{
			member_obj_t *m;
			char **uuids = NULL;
			uint32_t nu = 0, cap = 0, i;

			switch_thread_rwlock_wrlock(b->listener_lock);
			for (m = b->listener_head; m; m = m->next) {
				switch_time_t last;

				if (broadcast_member_role_load_acq(m) != BMEMBER_ROLE_LISTENER) {
					continue;
				}
				if (!(broadcast_mflags_load_acq(m) & MFLAG_RUNNING)) {
					continue;
				}
				last = broadcast_member_last_seq_advance_load_acq(m);
				if (last <= 0 || now - last <= 5000000) {
					continue;
				}
				if (!m->uuid || !*m->uuid) {
					continue;
				}
				if (nu >= cap) {
					uint32_t ncap = cap ? cap * 2 : 8;
					char **narr = (char **)realloc(uuids, sizeof(char *) * ncap);

					if (!narr) {
						break;
					}
					uuids = narr;
					cap = ncap;
				}
				uuids[nu] = strdup(m->uuid);
				if (!uuids[nu]) {
					break;
				}
				broadcast_mflags_or_rel(m, MFLAG_KICKED);
				nu++;
			}
			switch_thread_rwlock_unlock(b->listener_lock);
			for (i = 0; i < nu; i++) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT,
								  "broadcast %s: wedged listener uuid %s (no tick advance >5s); hanging up\n", b->name, uuids[i]);
				b_hangup_listener_session_by_uuid(uuids[i], "wedge_timeout", SWITCH_CAUSE_MEDIA_TIMEOUT);
				free(uuids[i]);
			}
			free(uuids);
		}
	}
	switch_thread_rwlock_unlock(broadcast_globals.hash_rwlock);
}
