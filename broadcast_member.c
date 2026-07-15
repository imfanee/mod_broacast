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
 * broadcast_member.c — member_obj_t codec setup and listener list hooks (spec §10).
 *
 */
#include "mod_broadcast.h"

/*
 * broadcast_member_next_id — Allocate next member id under id_mutex.
 * lock order: caller must not hold listener_lock.
 */
uint32_t broadcast_member_next_id(broadcast_obj_t *b)
{
	uint32_t id;
	switch_mutex_lock(b->id_mutex);
	id = ++b->next_member_id;
	switch_mutex_unlock(b->id_mutex);
	return id;
}

/*
 * broadcast_member_prepare — Init per-member fields and L16 codecs (spec §16).
 * Caller: session thread; must not hold broadcast listener_lock.
 */
switch_status_t broadcast_member_prepare(broadcast_obj_t *b, member_obj_t *m, bmember_role_t role)
{
	switch_status_t status;

	m->broadcast = b;
	broadcast_member_role_store_rel(m, role);
	m->write_gain = 1.0f;
	m->read_gain = 1.0f;
	m->vol_idx = 0;
	/* m->session, m->channel, m->pool, m->uuid are set by caller before prepare */
	if (!m->session || !m->channel) {
		return SWITCH_STATUS_FALSE;
	}

	/* DESIGN NOTE: Prompt §Things That Bite suggests matching negotiated ms/rate; we use broadcast profile
	 * rate/interval and rely on FreeSWITCH core resampling (spec §16.4) for simplicity and stable ring format. */
	if (switch_core_codec_init(&m->write_codec, "L16", NULL, NULL, b->rate, b->interval_ms, b->channels,
							   SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL, m->pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}
	/* Do NOT set_write_codec here: leave the session's negotiated wire codec (e.g. PCMU) in place so the core
	 * transcodes our L16 frames (tagged write_frame.codec = &m->write_codec) down to the wire in
	 * switch_core_session_write_frame. Overriding the session write codec to L16 makes frame->codec ==
	 * session->write_codec, so the core takes the raw do_write path and ships L16 bytes under the PCMU payload
	 * type -> listener hears white noise. This mirrors mod_conference, which sets read but never write codec. */

	if (role == BMEMBER_ROLE_SPEAKER) {
		if (switch_core_codec_init(&m->read_codec, "L16", NULL, NULL, b->rate, b->interval_ms, b->channels,
								   SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL, m->pool) != SWITCH_STATUS_SUCCESS) {
			switch_core_codec_destroy(&m->write_codec);
			return SWITCH_STATUS_FALSE;
		}
		switch_core_session_set_read_codec(m->session, &m->read_codec);
		switch_mutex_init(&m->audio_in_mutex, SWITCH_MUTEX_NESTED, m->pool);
		status = switch_buffer_create_dynamic(&m->audio_buffer, 1024, b->bytes_per_frame * 10, b->bytes_per_frame * 25);
		if (status != SWITCH_STATUS_SUCCESS) {
			switch_core_codec_destroy(&m->read_codec);
			switch_core_codec_destroy(&m->write_codec);
			return status;
		}
	}

	broadcast_member_energy_threshold_store_rel(m, 500);
	return SWITCH_STATUS_SUCCESS;
}

/*
 * broadcast_member_ensure_speaker_audio — Lazily create read path + ring buffer for set_speaker targets
 * that were not prepared as BMEMBER_ROLE_SPEAKER (spec §9.6 same bounds as prepare).
 */
switch_status_t broadcast_member_ensure_speaker_audio(broadcast_obj_t *b, member_obj_t *m)
{
	switch_status_t status;

	if (!m || !b || !m->pool || !m->session) {
		return SWITCH_STATUS_FALSE;
	}
	if (m->audio_buffer && m->audio_in_mutex) {
		return SWITCH_STATUS_SUCCESS;
	}

	if (!m->read_codec.codec_interface) {
		if (switch_core_codec_init(&m->read_codec, "L16", NULL, NULL, b->rate, b->interval_ms, b->channels,
								   SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL, m->pool) != SWITCH_STATUS_SUCCESS) {
			return SWITCH_STATUS_FALSE;
		}
		switch_core_session_set_read_codec(m->session, &m->read_codec);
	}
	if (!m->audio_in_mutex) {
		if (switch_mutex_init(&m->audio_in_mutex, SWITCH_MUTEX_NESTED, m->pool) != SWITCH_STATUS_SUCCESS) {
			return SWITCH_STATUS_FALSE;
		}
	}
	if (!m->audio_buffer) {
		status = switch_buffer_create_dynamic(&m->audio_buffer, 1024, b->bytes_per_frame * 10, b->bytes_per_frame * 25);
		if (status != SWITCH_STATUS_SUCCESS) {
			return status;
		}
	}
	return SWITCH_STATUS_SUCCESS;
}

void broadcast_member_cleanup_codecs(member_obj_t *m)
{
	if (!m) {
		return;
	}
	if (m->read_codec.codec_interface) {
		switch_core_codec_destroy(&m->read_codec);
	}
	if (m->write_codec.codec_interface) {
		switch_core_codec_destroy(&m->write_codec);
	}
}

/*
 * broadcast_listener_add — Insert listener at head (spec §10.2 head insertion).
 * lock order: listener_lock only here; do not nest control_mutex before listener_lock incorrectly.
 */
switch_status_t broadcast_listener_add(broadcast_obj_t *b, member_obj_t *m)
{
	if (broadcast_destroyed_load_acq(b)) {
		return SWITCH_STATUS_FALSE;
	}
	if (broadcast_cflags_load_acq(b) & BFLAG_LOCKED) {
		return SWITCH_STATUS_FALSE;
	}

	switch_thread_rwlock_wrlock(b->listener_lock);
	if (b->listener_count >= b->profile->max_listeners) {
		switch_thread_rwlock_unlock(b->listener_lock);
		return SWITCH_STATUS_FALSE;
	}
	if (!m->id) {
		m->id = broadcast_member_next_id(b);
	}
	broadcast_member_role_store_rel(m, BMEMBER_ROLE_LISTENER);
	m->joined_at = switch_micro_time_now();
	{
		uint64_t cs = __atomic_load_n(&b->producer_seq, __ATOMIC_ACQUIRE);
		if (b->profile->listener_pre_buffer_ms && b->profile->listener_pre_buffer_ms < b->interval_ms * b->ring_size) {
			uint64_t back = (uint64_t)(b->profile->listener_pre_buffer_ms / b->interval_ms);
			if (cs > back) {
				cs -= back;
			}
		}
		broadcast_member_consumer_seq_store_rel(m, cs);
	}
	m->next = b->listener_head;
	m->prev = NULL;
	if (b->listener_head) {
		b->listener_head->prev = m;
	}
	b->listener_head = m;
	b->listener_count++;
	if (b->listener_count > b->stat_peak_listeners) {
		b->stat_peak_listeners = b->listener_count;
	}
	switch_thread_rwlock_unlock(b->listener_lock);

	if (!(broadcast_mflags_load_acq(m) & MFLAG_GHOST)) {
		switch_atomic_inc(&b->stat_listeners_joined);
		switch_atomic_inc(&broadcast_globals.total_listeners_served);
		broadcast_enqueue_event(b, BEVT_LISTENER_JOIN, m->id, NULL);
	}
	broadcast_mflags_or_rel(m, MFLAG_RUNNING);
	return SWITCH_STATUS_SUCCESS;
}

/*
 * broadcast_listener_unlink_for_promotion — Remove listener from list without leave stats/event.
 * Used when promoting an in-bridge listener to speaker. Caller must not hold listener_lock.
 */
switch_status_t broadcast_listener_unlink_for_promotion(broadcast_obj_t *b, member_obj_t *m)
{
	member_obj_t *cur;
	int found = 0;

	if (!b || !m) {
		return SWITCH_STATUS_FALSE;
	}

	switch_thread_rwlock_wrlock(b->listener_lock);
	for (cur = b->listener_head; cur; cur = cur->next) {
		if (cur == m) {
			found = 1;
			break;
		}
	}
	if (found) {
		if (m->prev) {
			m->prev->next = m->next;
		} else {
			b->listener_head = m->next;
		}
		if (m->next) {
			m->next->prev = m->prev;
		}
		m->next = m->prev = NULL;
		if (b->listener_count) {
			b->listener_count--;
		}
	}
	switch_thread_rwlock_unlock(b->listener_lock);

	return found ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

/*
 * broadcast_listener_del — Remove listener from list (spec §10.3).
 * Caller: must not hold listener_lock.
 */
switch_status_t broadcast_listener_del(broadcast_obj_t *b, member_obj_t *m)
{
	member_obj_t *cur;
	int found = 0;

	if (!b || !m) {
		return SWITCH_STATUS_FALSE;
	}

	switch_thread_rwlock_wrlock(b->listener_lock);
	for (cur = b->listener_head; cur; cur = cur->next) {
		if (cur == m) {
			found = 1;
			break;
		}
	}
	if (found) {
		if (m->prev) {
			m->prev->next = m->next;
		} else {
			b->listener_head = m->next;
		}
		if (m->next) {
			m->next->prev = m->prev;
		}
		m->next = m->prev = NULL;
		if (b->listener_count) {
			b->listener_count--;
		}
		if (!(broadcast_mflags_load_acq(m) & MFLAG_GHOST)) {
			switch_atomic_inc(&b->stat_listeners_left);
			broadcast_enqueue_event(b, BEVT_LISTENER_LEAVE, m->id, NULL);
		}
	}
	switch_thread_rwlock_unlock(b->listener_lock);

	return SWITCH_STATUS_SUCCESS;
}
