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
 * broadcast_speaker.c — Speaker input thread and assignment (spec §9).
 *
 */
#include "mod_broadcast.h"

void *SWITCH_THREAD_FUNC broadcast_speaker_input_run(switch_thread_t *thread, void *obj)
{
	member_obj_t *m = (member_obj_t *)obj;
	broadcast_obj_t *b = m->broadcast;
	switch_frame_t *read_frame = NULL;
	switch_status_t status;

	(void)thread;

	/* Pin the speaker's session for the lifetime of the read loop. This helper thread reads another
	 * session's frames; without the read lock, a concurrent teardown (uuid_kill / hangup) can free the
	 * session's read_codec while switch_core_session_read_frame is dereferencing it -> SIGSEGV (observed
	 * via mod_loopback channel_read_frame). The read lock only blocks final session DESTROY, which must
	 * wait until we stop reading anyway (stop_input joins us first). */
	if (switch_core_session_read_lock(m->session) != SWITCH_STATUS_SUCCESS) {
		broadcast_member_input_running_store_rel(m, 0);
		return NULL;
	}

	broadcast_mflags_or_rel(m, MFLAG_ITHREAD);

	while (broadcast_member_input_running_load_acq(m) && (broadcast_mflags_load_acq(m) & MFLAG_RUNNING) && switch_channel_ready(m->channel)) {
		status = switch_core_session_read_frame(m->session, &read_frame, SWITCH_IO_FLAG_NONE, 0);
		if (!SWITCH_READ_ACCEPTABLE(status)) {
			break;
		}
		if (switch_test_flag(read_frame, SFF_CNG)) {
			continue;
		}
		if (read_frame->datalen == 0) {
			continue;
		}

		if (b->profile && b->profile->energy_detection) {
			int16_t *data = (int16_t *)read_frame->data;
			uint32_t i, n = read_frame->datalen / 2;
			uint32_t e = 0;
			uint32_t score;
			uint32_t thresh;
			switch_bool_t talking;
			switch_time_t silent_since;
			switch_time_t now;

			for (i = 0; i < n; i++) {
				e += (uint32_t) abs((int)data[i]);
			}
			score = n ? (e / n) : 0;
			broadcast_member_energy_last_score_store_rel(m, score);
			thresh = broadcast_member_energy_threshold_load_acq(m);
			if (score > thresh) {
				talking = broadcast_member_talking_state_load_acq(m);
				if (!talking) {
					char ebuf[32];
					broadcast_member_talking_state_store_rel(m, SWITCH_TRUE);
					switch_snprintf(ebuf, sizeof(ebuf), "%u", score);
					broadcast_enqueue_event(b, BEVT_SPEAKER_TALKING, m->id, ebuf);
				}
				broadcast_member_silent_since_store_rel(m, 0);
			} else {
				now = switch_micro_time_now();
				silent_since = broadcast_member_silent_since_load_acq(m);
				if (!silent_since) {
					broadcast_member_silent_since_store_rel(m, now);
					silent_since = now;
				}
				talking = broadcast_member_talking_state_load_acq(m);
				if (talking && b->profile &&
					(now - silent_since) > (switch_time_t)b->profile->silence_window_ms * 1000) {
					char ebuf[32];
					broadcast_member_talking_state_store_rel(m, SWITCH_FALSE);
					switch_snprintf(ebuf, sizeof(ebuf), "%u", score);
					broadcast_enqueue_event(b, BEVT_SPEAKER_SILENT, m->id, ebuf);
				}
			}
		}

		switch_mutex_lock(m->audio_in_mutex);
		switch_buffer_write(m->audio_buffer, read_frame->data, read_frame->datalen);
		switch_mutex_unlock(m->audio_in_mutex);
	}

	broadcast_mflags_and_rel(m, (uint32_t)~MFLAG_ITHREAD);
	broadcast_member_input_running_store_rel(m, 0);
	switch_core_session_rwunlock(m->session);
	return NULL;
}

switch_status_t broadcast_speaker_start_input(member_obj_t *m)
{
	switch_threadattr_t *attr = NULL;
	switch_status_t st;

	if (!m) {
		return SWITCH_STATUS_FALSE;
	}
	if (broadcast_member_input_thread_load_acq(m)) {
		return SWITCH_STATUS_SUCCESS;
	}
	switch_threadattr_create(&attr, m->pool);
	switch_threadattr_stacksize_set(attr, SWITCH_THREAD_STACKSIZE);
	broadcast_member_input_running_store_rel(m, 1);
	st = switch_thread_create(&m->input_thread, attr, broadcast_speaker_input_run, m, m->pool);
	if (st != SWITCH_STATUS_SUCCESS) {
		broadcast_member_input_running_store_rel(m, 0);
		return st;
	}
	return SWITCH_STATUS_SUCCESS;
}

void broadcast_speaker_stop_input(member_obj_t *m)
{
	switch_status_t st;
	switch_thread_t *thr;

	if (!m) {
		return;
	}
	broadcast_member_input_running_store_rel(m, 0);
	thr = broadcast_member_input_thread_exchange_null(m);
	if (!thr) {
		return;
	}
	switch_thread_join(&st, thr);
}

/*
 * broadcast_request_promotion — Foreign-thread (API) request to promote an in-bridge listener to speaker.
 *
 * SYSTEMIC LIFETIME RULE: current_speaker may only be *published* for a member by that member's OWN
 * session thread (the self-join path, or the promotion-commit in the app's MFLAG_ROLE_TRANSITION_TO_SPEAKER
 * branch), because that same thread also clears it (speaker_main_wait) before its session pool — which owns
 * audio_buffer/audio_in_mutex/the member itself — is freed. A foreign thread that writes current_speaker = m
 * directly races m's own teardown: m's speaker_main_wait can observe current_speaker != m (not published yet),
 * bail, and free the session before this thread publishes, leaving the producer reading a freed speaker.
 *
 * So this foreign-thread entry point NEVER touches current_speaker: it only unlinks m from the listener list
 * and raises MFLAG_ROLE_TRANSITION_TO_SPEAKER, handing off to m's own thread to install itself as speaker.
 * Caller must hold a session pin on m (b_find_member_pinned) for the duration of this call.
 * lock order: takes listener_lock (via unlink); must NOT be called holding speaker_lock.
 */
switch_status_t broadcast_request_promotion(broadcast_obj_t *b, member_obj_t *m)
{
	switch_time_t deadline;

	if (!b || !m) {
		return SWITCH_STATUS_FALSE;
	}
	if (!broadcast_member_in_listener_bridge_load_acq(m)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "broadcast: cannot promote m_%u (not an active listener)\n", m->id);
		return SWITCH_STATUS_FALSE;
	}
	if (broadcast_listener_unlink_for_promotion(b, m) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "broadcast: promotion failed (m_%u not on listener list)\n", m->id);
		return SWITCH_STATUS_FALSE;
	}
	/* Signal m's own thread to leave broadcast_listener_run and take the speaker branch, where it will
	 * call broadcast_set_speaker(b, m) itself. Clearing MFLAG_RUNNING makes the listener loop exit. */
	broadcast_mflags_or_rel(m, MFLAG_ROLE_TRANSITION_TO_SPEAKER);
	broadcast_mflags_and_rel(m, (uint32_t)~MFLAG_RUNNING);
	deadline = switch_micro_time_now() + 200000;
	while (broadcast_member_in_listener_bridge_load_acq(m) && switch_micro_time_now() < deadline) {
		switch_yield(1000);
	}
	if (broadcast_member_in_listener_bridge_load_acq(m)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "broadcast: promotion wait timeout m_%u; re-adding as listener\n", m->id);
		broadcast_mflags_and_rel(m, (uint32_t)~(MFLAG_ROLE_TRANSITION_TO_SPEAKER));
		if (broadcast_listener_add(b, m) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "broadcast: promotion rollback listener_add failed m_%u\n", m->id);
		}
		return SWITCH_STATUS_FALSE;
	}
	return SWITCH_STATUS_SUCCESS;
}

/*
 * broadcast_set_speaker — Install speaker pointer (spec §9.2).
 * Publish current_speaker only after new_speaker's input path exists and input thread is started
 * (producer may rdlock immediately after wrunlock).
 * MUST be called on new_speaker's OWN session thread (self-join, or promotion-commit) — see the lifetime
 * rule on broadcast_request_promotion. An in-bridge listener is still owned by broadcast_listener_run and
 * is refused here: promote it via broadcast_request_promotion, which hands off to its own thread.
 * lock order: control_mutex (optional outer), speaker_lock here; do not take listener_lock after speaker_lock.
 */
switch_status_t broadcast_set_speaker(broadcast_obj_t *b, member_obj_t *new_speaker)
{
	member_obj_t *old;

	if (!b) {
		return SWITCH_STATUS_FALSE;
	}

	/* Defensive: never publish a member that is still running its listener loop on another thread. */
	if (new_speaker && broadcast_member_in_listener_bridge_load_acq(new_speaker)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "broadcast: set_speaker refused for in-bridge listener m_%u (use broadcast_request_promotion)\n", new_speaker->id);
		return SWITCH_STATUS_FALSE;
	}

	if (new_speaker) {
		if (broadcast_member_ensure_speaker_audio(b, new_speaker) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
							  "broadcast: set_speaker could not init speaker audio for m_%u\n", new_speaker->id);
			return SWITCH_STATUS_FALSE;
		}
		broadcast_member_role_store_rel(new_speaker, BMEMBER_ROLE_SPEAKER);
		if (broadcast_speaker_start_input(new_speaker) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
							  "broadcast: set_speaker start_input failed for m_%u\n", new_speaker->id);
			return SWITCH_STATUS_FALSE;
		}
	}

	switch_thread_rwlock_wrlock(b->speaker_lock);
	old = b->current_speaker;
	if (old == new_speaker) {
		switch_thread_rwlock_unlock(b->speaker_lock);
		return SWITCH_STATUS_SUCCESS;
	}
	b->current_speaker = new_speaker;
	b->speaker_active_since = switch_micro_time_now();
	if (new_speaker) {
		/* Per-member anchor for CDR broadcast_was_speaker_for_ms (dialplan exit path). */
		new_speaker->speaker_since_us = b->speaker_active_since;
	}
	switch_thread_rwlock_unlock(b->speaker_lock);

	if (new_speaker) {
		broadcast_fire_critical_event(b, BEVT_SPEAKER_SET, new_speaker->id, NULL);
	} else {
		broadcast_fire_critical_event(b, BEVT_SPEAKER_CLEAR, 0, NULL);
	}

	if (old && old != new_speaker) {
		broadcast_speaker_stop_input(old);
		/* Demotion-by-replacement: the old speaker's app (speaker branch / promoted-listener branch) returns
		 * as soon as main_wait observes current_speaker != old, so its session is already exiting. Do NOT
		 * re-add it to listener_head: the app flow has no path back into broadcast_listener_run, so a re-added
		 * node dangles the instant the session pool frees -> use-after-free in the producer / listeners cmd.
		 * The replaced speaker simply steps down (disconnects).
		 * TODO(design): true demote-to-listener would require the speaker app branch to loop into
		 * broadcast_listener_run after main_wait returns; until then, never re-link a departing member. */
		old->speaker_since_us = 0;
	}
	return SWITCH_STATUS_SUCCESS;
}

switch_status_t broadcast_clear_speaker(broadcast_obj_t *b)
{
	return broadcast_set_speaker(b, NULL);
}

/* Compare-and-clear: if this member is still the installed speaker as its session tears down, drop it
 * (under speaker_lock wrlock, so the producer is never mid-read) before its session pool frees. This is
 * the systemic guard against ANY speaker-assignment path that fails to route the member back through
 * speaker_main_wait/clear_speaker on exit — without it, current_speaker can dangle and the producer
 * reads a freed audio_buffer/audio_in_mutex -> SIGSEGV. Idempotent: a no-op if we are not the speaker. */
void broadcast_clear_speaker_if_current(broadcast_obj_t *b, member_obj_t *m)
{
	int was = 0;

	if (!b || !m) {
		return;
	}
	switch_thread_rwlock_wrlock(b->speaker_lock);
	if (b->current_speaker == m) {
		b->current_speaker = NULL;
		was = 1;
	}
	switch_thread_rwlock_unlock(b->speaker_lock);
	if (was) {
		broadcast_speaker_stop_input(m);
	}
}

/*
 * broadcast_speaker_main_wait — Block session thread while speaker is active (spec §6.3).
 * DESIGN NOTE: not producer/listener hot path; short switch_yield is acceptable here (spec §3 R7).
 * in_speaker_wait pairs with broadcast_destroy's drain poll so the broadcast pool is not freed while
 * this thread may still rdlock b->speaker_lock (see broadcast_core.c).
 */
void broadcast_speaker_main_wait(member_obj_t *m)
{
	broadcast_obj_t *b = m->broadcast;

	broadcast_member_in_speaker_wait_store_rel(m, 1);
	while ((broadcast_mflags_load_acq(m) & MFLAG_RUNNING) && switch_channel_ready(m->channel) && !broadcast_destroyed_load_acq(b)) {
		int still = 0;
		switch_dtmf_t dtmf;

		while (switch_channel_ready(m->channel) && switch_channel_dequeue_dtmf(m->channel, &dtmf) == SWITCH_STATUS_SUCCESS) {
			broadcast_listener_handle_dtmf(m, dtmf.digit);
		}

		switch_thread_rwlock_rdlock(b->speaker_lock);
		still = (b->current_speaker == m);
		switch_thread_rwlock_unlock(b->speaker_lock);
		if (!still) {
			break;
		}
		switch_yield(20000);
	}
	/* Clear our speaker slot before in_speaker_wait drops so destroy cannot free speaker_lock while we still
	 * need it. If we were demoted, current_speaker is already someone else — do not clear_speaker(b). */
	switch_thread_rwlock_rdlock(b->speaker_lock);
	if (b->current_speaker == m) {
		switch_thread_rwlock_unlock(b->speaker_lock);
		broadcast_clear_speaker(b);
	} else {
		switch_thread_rwlock_unlock(b->speaker_lock);
	}
	broadcast_member_in_speaker_wait_store_rel(m, 0);
}
