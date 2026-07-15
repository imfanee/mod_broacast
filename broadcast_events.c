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
 * broadcast_events.c — Deferred event queue per broadcast and module emitter thread.
 *
 */
#include "mod_broadcast.h"

/* DESIGN NOTE: Spec §6.6 describes SPSC; multiple threads enqueue (join/leave/resync).
 * We protect the fixed ring with event_mutex. Emitter acquires hash rdlock then per-broadcast
 * event_mutex — never nested with listener_lock/speaker_lock (see spec §7.2 extension). */

static const char *b_evt_subclass(bevt_type_t t)
{
	switch (t) {
	case BEVT_BROADCAST_CREATE: return "broadcast::create";
	case BEVT_BROADCAST_DESTROY: return "broadcast::destroy";
	case BEVT_SPEAKER_SET: return "broadcast::speaker-set";
	case BEVT_SPEAKER_CLEAR: return "broadcast::speaker-clear";
	case BEVT_SPEAKER_TALKING: return "broadcast::speaker-talking";
	case BEVT_SPEAKER_SILENT: return "broadcast::speaker-silent";
	case BEVT_LISTENER_JOIN: return "broadcast::listener-join";
	case BEVT_LISTENER_LEAVE: return "broadcast::listener-leave";
	case BEVT_LISTENER_RESYNC: return "broadcast::listener-resync";
	case BEVT_LISTENER_KICKED: return "broadcast::listener-kicked";
	case BEVT_RECORDING_START: return "broadcast::recording-start";
	case BEVT_RECORDING_STOP: return "broadcast::recording-stop";
	case BEVT_PAUSE: return "broadcast::pause";
	case BEVT_RESUME: return "broadcast::resume";
	case BEVT_LOCK: return "broadcast::lock";
	case BEVT_UNLOCK: return "broadcast::unlock";
	case BEVT_SPEAKER_REQUEST: return "broadcast::speaker-request";
	case BEVT_RECORDING_FAILED: return "broadcast::recording-failed";
	case BEVT_PRODUCER_STALL: return "broadcast::producer-stall";
	case BEVT_HEARTBEAT: return "broadcast::heartbeat";
	case BEVT_DESTROY_GRACE: return "broadcast::destroy-grace";
	case BEVT_DTMF_ACTION: return "broadcast::dtmf-action";
	default: return "broadcast::create";
	}
}

static void b_evt_add_common(broadcast_obj_t *b, switch_event_t *event)
{
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Broadcast-Name", b->name);
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Broadcast-UUID", b->uuid_str);
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Broadcast-Profile", b->profile ? b->profile->name : "default");
	switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Timestamp", "%" SWITCH_TIME_T_FMT, switch_micro_time_now());
}

static member_obj_t *b_find_member_by_id(broadcast_obj_t *b, uint32_t id)
{
	member_obj_t *m;
	switch_thread_rwlock_rdlock(b->listener_lock);
	for (m = b->listener_head; m; m = m->next) {
		if (m->id == id) {
			switch_thread_rwlock_unlock(b->listener_lock);
			return m;
		}
	}
	switch_thread_rwlock_unlock(b->listener_lock);
	switch_thread_rwlock_rdlock(b->speaker_lock);
	if (b->current_speaker && b->current_speaker->id == id) {
		member_obj_t *sp = b->current_speaker;
		switch_thread_rwlock_unlock(b->speaker_lock);
		return sp;
	}
	switch_thread_rwlock_unlock(b->speaker_lock);
	return NULL;
}

static void b_fire_evt_msg(broadcast_obj_t *b, const evt_msg_t *msg)
{
	switch_event_t *event = NULL;
	const char *sub = b_evt_subclass(msg->type);

	if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, sub) != SWITCH_STATUS_SUCCESS) {
		return;
	}
	b_evt_add_common(b, event);
	switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Member-ID", "%u", msg->member_id);
	if (!zstr(msg->data)) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Broadcast-Event-Data", msg->data);
	}

	if (msg->type == BEVT_LISTENER_JOIN || msg->type == BEVT_LISTENER_LEAVE) {
		member_obj_t *m = b_find_member_by_id(b, msg->member_id);
		if (m && m->session) {
			switch_caller_profile_t *prof = switch_channel_get_caller_profile(m->channel);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Member-UUID", m->uuid);
			if (prof) {
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Caller-ID-Name", prof->caller_id_name);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Caller-ID-Number", prof->caller_id_number);
			}
			if (m->tag) {
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Tag", m->tag);
			}
		}
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Listener-Count", "%u", b->listener_count);
	} else if (msg->type == BEVT_LISTENER_RESYNC) {
		member_obj_t *m = b_find_member_by_id(b, msg->member_id);
		if (m) {
			switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Resync-Total", "%u", broadcast_member_resync_load_acq(m));
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Lag-Frames", msg->data);
		}
	} else if (msg->type == BEVT_SPEAKER_TALKING || msg->type == BEVT_SPEAKER_SILENT) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Energy", msg->data);
		if (b->profile) {
			switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Silence-Window-Ms", "%u", b->profile->silence_window_ms);
		}
	} else if (msg->type == BEVT_SPEAKER_REQUEST) {
		member_obj_t *m = b_find_member_by_id(b, msg->member_id);
		if (m && m->session) {
			switch_caller_profile_t *prof = switch_channel_get_caller_profile(m->channel);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Member-UUID", m->uuid);
			if (prof) {
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Caller-ID-Name", prof->caller_id_name);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Caller-ID-Number", prof->caller_id_number);
			}
			if (m->tag) {
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Tag", m->tag);
			}
		}
	} else if (msg->type == BEVT_RECORDING_STOP && !zstr(msg->data)) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Reason", msg->data);
	} else if (msg->type == BEVT_RECORDING_FAILED && !zstr(msg->data)) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Reason", msg->data);
	} else if (msg->type == BEVT_PRODUCER_STALL && !zstr(msg->data)) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Drift-Us", msg->data);
	} else if (msg->type == BEVT_DESTROY_GRACE) {
		char *grace = NULL;
		char *ann = NULL;
		char *data_copy = strdup(msg->data);
		if (data_copy) {
			char *p = strchr(data_copy, '|');
			if (p) {
				*p = '\0';
				grace = data_copy;
				ann = p + 1;
			} else {
				grace = data_copy;
			}
			if (grace) {
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Grace-Ms", grace);
			}
			if (ann) {
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Announcement-File", ann);
			}
			free(data_copy);
		}
	} else if (msg->type == BEVT_DTMF_ACTION) {
		char *digit = NULL;
		char *payload = NULL;
		char *data_copy = strdup(msg->data);
		if (data_copy) {
			char *p = strchr(data_copy, '|');
			if (p) {
				*p = '\0';
				digit = data_copy;
				payload = p + 1;
			} else {
				digit = data_copy;
			}
			if (digit) {
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "DTMF-Digit", digit);
			}
			if (payload) {
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "DTMF-Payload", payload);
			}
			free(data_copy);
		}
	}

	switch_event_fire(&event);
}

/*
 * broadcast_enqueue_event — Queue deferred CUSTOM event for emitter thread.
 * Caller must not hold b->event_mutex.
 */
void broadcast_enqueue_event(broadcast_obj_t *b, bevt_type_t type, uint32_t member_id, const char *data)
{
	uint64_t prod, cons, sz;
	evt_msg_t *slot;

	if (!b || broadcast_destroyed_load_acq(b)) {
		return;
	}

	switch_mutex_lock(b->event_mutex);
	sz = b->event_ring_size;
	prod = b->event_prod_idx;
	cons = b->event_cons_idx;
	if (sz == 0 || !b->event_ring) {
		switch_mutex_unlock(b->event_mutex);
		return;
	}
	if (prod - cons >= sz - 1) {
		switch_atomic_inc(&b->stat_events_dropped);
		b->event_cons_idx++;
		switch_mutex_unlock(b->event_mutex);
		return;
	}
	slot = &b->event_ring[prod % sz];
	slot->type = type;
	slot->member_id = member_id;
	slot->seq = prod;
	slot->when = switch_micro_time_now();
	if (!zstr(data)) {
		switch_copy_string(slot->data, data, sizeof(slot->data));
	} else {
		slot->data[0] = '\0';
	}
	b->event_prod_idx = prod + 1;
	switch_mutex_unlock(b->event_mutex);
}

/*
 * broadcast_fire_critical_event — Synchronous fire for critical subclasses (spec §14.5).
 * Caller may hold b->control_mutex; must not hold event_mutex.
 */
void broadcast_fire_critical_event(broadcast_obj_t *b, bevt_type_t type, uint32_t member_id, const char *data)
{
	evt_msg_t em;
	memset(&em, 0, sizeof(em));
	em.type = type;
	em.member_id = member_id;
	if (!zstr(data)) {
		switch_copy_string(em.data, data, sizeof(em.data));
	}
	b_fire_evt_msg(b, &em);
}

void *SWITCH_THREAD_FUNC broadcast_event_emitter_run(switch_thread_t *thread, void *obj)
{
	(void)thread;
	(void)obj;

	while (broadcast_globals.emitter_running) {
		switch_hash_index_t *hi;
		int worked = 0;

		switch_thread_rwlock_rdlock(broadcast_globals.hash_rwlock);
		for (hi = switch_core_hash_first(broadcast_globals.broadcast_hash); hi; hi = switch_core_hash_next(&hi)) {
			void *val = NULL;
			broadcast_obj_t *b;

			switch_core_hash_this(hi, NULL, NULL, &val);
			b = (broadcast_obj_t *) val;
			if (!b || broadcast_destroyed_load_acq(b)) {
				continue;
			}
			switch_mutex_lock(b->event_mutex);
			while (b->event_cons_idx < b->event_prod_idx) {
				evt_msg_t copy = b->event_ring[b->event_cons_idx % b->event_ring_size];
				b->event_cons_idx++;
				switch_mutex_unlock(b->event_mutex);
				b_fire_evt_msg(b, &copy);
				worked = 1;
				switch_mutex_lock(b->event_mutex);
			}
			switch_mutex_unlock(b->event_mutex);
		}
		switch_thread_rwlock_unlock(broadcast_globals.hash_rwlock);

		if (!worked) {
			switch_yield(5000);
		}
	}
	return NULL;
}

switch_status_t broadcast_event_emitter_start(void)
{
	switch_threadattr_t *thd_attr = NULL;

	broadcast_globals.emitter_running = 1;
	switch_threadattr_create(&thd_attr, broadcast_globals.pool);
	switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
	return switch_thread_create(&broadcast_globals.event_emitter_thread, thd_attr,
							   broadcast_event_emitter_run, NULL, broadcast_globals.pool);
}

void broadcast_event_emitter_stop(void)
{
	broadcast_globals.emitter_running = 0;
	if (broadcast_globals.event_emitter_thread) {
		switch_status_t st;
		switch_thread_join(&st, broadcast_globals.event_emitter_thread);
		broadcast_globals.event_emitter_thread = NULL;
	}
}

void broadcast_event_reserve_all(void)
{
	switch_event_reserve_subclass("broadcast::create");
	switch_event_reserve_subclass("broadcast::destroy");
	switch_event_reserve_subclass("broadcast::speaker-set");
	switch_event_reserve_subclass("broadcast::speaker-clear");
	switch_event_reserve_subclass("broadcast::speaker-talking");
	switch_event_reserve_subclass("broadcast::speaker-silent");
	switch_event_reserve_subclass("broadcast::listener-join");
	switch_event_reserve_subclass("broadcast::listener-leave");
	switch_event_reserve_subclass("broadcast::listener-resync");
	switch_event_reserve_subclass("broadcast::listener-kicked");
	switch_event_reserve_subclass("broadcast::recording-start");
	switch_event_reserve_subclass("broadcast::recording-stop");
	switch_event_reserve_subclass("broadcast::pause");
	switch_event_reserve_subclass("broadcast::resume");
	switch_event_reserve_subclass("broadcast::lock");
	switch_event_reserve_subclass("broadcast::unlock");
	switch_event_reserve_subclass("broadcast::speaker-request");
	switch_event_reserve_subclass("broadcast::recording-failed");
	switch_event_reserve_subclass("broadcast::producer-stall");
	switch_event_reserve_subclass("broadcast::heartbeat");
	switch_event_reserve_subclass("broadcast::destroy-grace");
	switch_event_reserve_subclass("broadcast::dtmf-action");
}

void broadcast_event_free_all(void)
{
	switch_event_free_subclass("broadcast::create");
	switch_event_free_subclass("broadcast::destroy");
	switch_event_free_subclass("broadcast::speaker-set");
	switch_event_free_subclass("broadcast::speaker-clear");
	switch_event_free_subclass("broadcast::speaker-talking");
	switch_event_free_subclass("broadcast::speaker-silent");
	switch_event_free_subclass("broadcast::listener-join");
	switch_event_free_subclass("broadcast::listener-leave");
	switch_event_free_subclass("broadcast::listener-resync");
	switch_event_free_subclass("broadcast::listener-kicked");
	switch_event_free_subclass("broadcast::recording-start");
	switch_event_free_subclass("broadcast::recording-stop");
	switch_event_free_subclass("broadcast::pause");
	switch_event_free_subclass("broadcast::resume");
	switch_event_free_subclass("broadcast::lock");
	switch_event_free_subclass("broadcast::unlock");
	switch_event_free_subclass("broadcast::speaker-request");
	switch_event_free_subclass("broadcast::recording-failed");
	switch_event_free_subclass("broadcast::producer-stall");
	switch_event_free_subclass("broadcast::heartbeat");
	switch_event_free_subclass("broadcast::destroy-grace");
	switch_event_free_subclass("broadcast::dtmf-action");
}
