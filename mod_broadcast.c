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
 * mod_broadcast.c — Module entry, dialplan application, API registration.
 *
 */
#include "mod_broadcast.h"

SWITCH_MODULE_LOAD_FUNCTION(mod_broadcast_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_broadcast_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_broadcast_runtime);
SWITCH_MODULE_DEFINITION(mod_broadcast, mod_broadcast_load, mod_broadcast_shutdown, mod_broadcast_runtime);

static void b_copy_seg(char *dst, size_t dlen, const char *start, size_t seglen)
{
	if (!dst || dlen == 0) {
		return;
	}
	if (seglen >= dlen) {
		seglen = dlen - 1;
	}
	memcpy(dst, start, seglen);
	dst[seglen] = '\0';
}

static void b_parse_app_data(const char *data, char *name, size_t nlen, char *profile, size_t plen, char *params, size_t prlen)
{
	const char *at;
	const char *plus;
	const char *p = data;

	name[0] = profile[0] = params[0] = '\0';
	if (zstr(p)) {
		return;
	}
	at = strchr(p, '@');
	plus = strchr(p, '+');
	if (at && (!plus || at < plus)) {
		b_copy_seg(name, nlen, p, (size_t)(at - p));
		if (plus && plus > at) {
			b_copy_seg(profile, plen, at + 1, (size_t)(plus - at - 1));
			switch_copy_string(params, plus + 1, prlen);
		} else {
			switch_copy_string(profile, at + 1, plen);
		}
	} else if (plus) {
		b_copy_seg(name, nlen, p, (size_t)(plus - p));
		switch_copy_string(params, plus + 1, prlen);
	} else {
		switch_copy_string(name, p, nlen);
	}
}

static switch_bool_t b_param_get(const char *params, const char *key, char *out, size_t olen)
{
	char buf[1024];
	char *argv[32] = {0};
	int argc, i;

	if (zstr(params) || zstr(key)) {
		return SWITCH_FALSE;
	}
	switch_copy_string(buf, params, sizeof(buf));
	argc = switch_separate_string(buf, ',', argv, (sizeof(argv) / sizeof(argv[0])) - 1);
	for (i = 0; i < argc; i++) {
		char *eq = strchr(argv[i], '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		if (!strcasecmp(argv[i], key)) {
			switch_copy_string(out, eq + 1, olen);
			return SWITCH_TRUE;
		}
	}
	return SWITCH_FALSE;
}

static void broadcast_member_set_exit_variables(broadcast_obj_t *b, member_obj_t *m, switch_channel_t *channel, switch_time_t join_us)
{
	char tmp[64];
	const char *left_reason = switch_channel_get_variable(channel, "broadcast_left_reason");
	const char *ended_by_self = switch_channel_get_variable(channel, "broadcast_ended_by_self");
	const char *kicked_by = switch_channel_get_variable(channel, "broadcast_kicked_by");
	bmember_role_t final_role_enum = broadcast_member_role_load_acq(m);

	/* Set total duration */
	switch_snprintf(tmp, sizeof(tmp), "%" SWITCH_TIME_T_FMT, (switch_micro_time_now() - join_us) / 1000);
	switch_channel_set_variable(channel, "broadcast_total_duration_ms", tmp);

	/* Set speaker duration if they spent time as speaker */
	if (m->speaker_since_us > 0) {
		switch_snprintf(tmp, sizeof(tmp), "%" SWITCH_TIME_T_FMT, (switch_micro_time_now() - m->speaker_since_us) / 1000);
		switch_channel_set_variable(channel, "broadcast_was_speaker_for_ms", tmp);
	}

	/* Determine left reason if not set */
	if (zstr(left_reason)) {
		if (broadcast_destroyed_load_acq(b)) {
			left_reason = "destroyed";
		} else if (broadcast_mflags_load_acq(m) & MFLAG_KICKED) {
			left_reason = "kicked";
		} else {
			left_reason = "hangup";
		}
		switch_channel_set_variable(channel, "broadcast_left_reason", left_reason);
	}

	/* Determine kicked_by */
	if (zstr(kicked_by)) {
		if (!strcmp(left_reason, "kicked")) {
			kicked_by = "admin";
		} else if (!strcmp(left_reason, "wedge_timeout")) {
			kicked_by = "wedged";
		} else if (!strcmp(left_reason, "destroyed")) {
			kicked_by = "destroy";
		} else {
			kicked_by = "";
		}
		switch_channel_set_variable(channel, "broadcast_kicked_by", kicked_by);
	}

	/* Determine ended_by_self */
	if (zstr(ended_by_self)) {
		if (!strcmp(left_reason, "hangup") || !strcmp(left_reason, "self-leave") || !strcmp(left_reason, "self-hangup")) {
			ended_by_self = "true";
		} else {
			ended_by_self = "false";
		}
		switch_channel_set_variable(channel, "broadcast_ended_by_self", ended_by_self);
	}

	/* Set final role */
	switch_channel_set_variable(channel, "broadcast_final_role", (final_role_enum == BMEMBER_ROLE_SPEAKER) ? "speaker" : "listener");
}

SWITCH_STANDARD_APP(broadcast_app_function)
{
	char nm[256], prof[128], prm[1024];
	char rolebuf[32] = "listener";
	char ghostbuf[8] = "";
	char excbuf[8] = "";
	char tagbuf[256] = "";
	char dispbuf[256] = "";
	bmember_role_t role = BMEMBER_ROLE_LISTENER;
	switch_bool_t ghost = SWITCH_FALSE;
	switch_bool_t exclusive = SWITCH_FALSE;
	broadcast_obj_t *b;
	member_obj_t *m = NULL;
	switch_memory_pool_t *pool;
	switch_channel_t *channel;
	switch_time_t join_us;
	char tmp[64];

	if (zstr(data)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "broadcast: missing data\n");
		return;
	}

	b_parse_app_data(data, nm, sizeof(nm), prof, sizeof(prof), prm, sizeof(prm));
	if (zstr(nm)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "broadcast: missing name\n");
		return;
	}

	if (b_param_get(prm, "role", rolebuf, sizeof(rolebuf))) {
		if (!strcasecmp(rolebuf, "speaker")) {
			role = BMEMBER_ROLE_SPEAKER;
		} else {
			role = BMEMBER_ROLE_LISTENER;
		}
	}
	if (b_param_get(prm, "ghost", ghostbuf, sizeof(ghostbuf))) {
		ghost = switch_true(ghostbuf);
	}
	if (b_param_get(prm, "exclusive", excbuf, sizeof(excbuf))) {
		exclusive = switch_true(excbuf);
	}
	(void)b_param_get(prm, "tag", tagbuf, sizeof(tagbuf));
	(void)b_param_get(prm, "display_name", dispbuf, sizeof(dispbuf));

	pool = switch_core_session_get_pool(session);
	channel = switch_core_session_get_channel(session);
	join_us = switch_micro_time_now();

	b = broadcast_find_and_ref(nm);
	if (b && exclusive) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "broadcast: exclusive join rejected (exists)\n");
		switch_channel_hangup(channel, SWITCH_CAUSE_USER_BUSY);
		broadcast_release(b);
		return;
	}
	if (!b) {
		const char *pname = prof[0] ? prof : broadcast_globals.default_profile_name;
		b = broadcast_create(nm, pname, pool, session);
		if (!b) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "broadcast: could not create %s\n", nm);
			return;
		}
	}

	m = switch_core_session_alloc(session, sizeof(*m));
	memset(m, 0, sizeof(*m));
	m->pool = pool;
	m->session = session;
	m->channel = channel;
	m->uuid = switch_core_strdup(pool, switch_core_session_get_uuid(session));
	if (tagbuf[0]) {
		m->tag = switch_core_strdup(pool, tagbuf);
	}
	if (dispbuf[0]) {
		m->display_name = switch_core_strdup(pool, dispbuf);
	}
	if (ghost) {
		broadcast_mflags_or_rel(m, MFLAG_GHOST);
	}

	if (broadcast_member_prepare(b, m, role) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "broadcast: codec init failed\n");
		broadcast_release(b);
		return;
	}

	switch_channel_set_variable(channel, "broadcast_name", b->name);
	switch_channel_set_variable(channel, "broadcast_uuid", b->uuid_str);
	switch_channel_set_variable(channel, "broadcast_role", role == BMEMBER_ROLE_SPEAKER ? "speaker" : "listener");
	switch_snprintf(tmp, sizeof(tmp), "%" SWITCH_TIME_T_FMT, (switch_time_t)(join_us / 1000000));
	switch_channel_set_variable(channel, "broadcast_join_time", tmp);
	switch_channel_set_variable(channel, "broadcast_profile", b->profile ? b->profile->name : "");

	if (role == BMEMBER_ROLE_SPEAKER) {
		switch_thread_rwlock_rdlock(b->speaker_lock);
		if (b->current_speaker && b->current_speaker != m && b->profile && !b->profile->speaker_override) {
			switch_thread_rwlock_unlock(b->speaker_lock);
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "broadcast: speaker already present\n");
			switch_channel_hangup(channel, SWITCH_CAUSE_USER_BUSY);
			broadcast_member_cleanup_codecs(m);
			broadcast_release(b);
			return;
		}
		switch_thread_rwlock_unlock(b->speaker_lock);
		m->id = broadcast_member_next_id(b);
		broadcast_mflags_or_rel(m, MFLAG_RUNNING);
		switch_snprintf(tmp, sizeof(tmp), "%u", m->id);
		switch_channel_set_variable(channel, "broadcast_member_id", tmp);
		if (broadcast_set_speaker(b, m) != SWITCH_STATUS_SUCCESS) {
			broadcast_member_cleanup_codecs(m);
			broadcast_release(b);
			return;
		}
		broadcast_speaker_main_wait(m);
		broadcast_mflags_and_rel(m, (uint32_t)~MFLAG_RUNNING);
		broadcast_member_set_exit_variables(b, m, channel, join_us);
	} else {
		if (broadcast_listener_add(b, m) != SWITCH_STATUS_SUCCESS) {
			switch_channel_set_variable(channel, "broadcast_left_reason", "error");
			switch_channel_hangup(channel, SWITCH_CAUSE_USER_BUSY);
			broadcast_member_cleanup_codecs(m);
			broadcast_release(b);
			return;
		}
		switch_snprintf(tmp, sizeof(tmp), "%u", m->id);
		switch_channel_set_variable(channel, "broadcast_member_id", tmp);
		broadcast_listener_run(m);
		if (broadcast_mflags_load_acq(m) & MFLAG_ROLE_TRANSITION_TO_SPEAKER) {
			broadcast_mflags_and_rel(m, (uint32_t)~MFLAG_ROLE_TRANSITION_TO_SPEAKER);
			broadcast_mflags_or_rel(m, MFLAG_RUNNING);
			switch_channel_set_variable(channel, "broadcast_role", "speaker");
			/* Promotion commit: publish current_speaker on the member's OWN thread (the API thread that
			 * requested the promotion only signalled us). This thread also clears current_speaker in
			 * speaker_main_wait before its session pool frees, so the producer can never read a freed
			 * speaker (the systemic speaker-lifetime TOCTOU). */
			if (broadcast_set_speaker(b, m) == SWITCH_STATUS_SUCCESS) {
				broadcast_speaker_main_wait(m);
			} else {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
								  "broadcast: promotion commit failed for m_%u; leaving\n", m->id);
			}
			broadcast_mflags_and_rel(m, (uint32_t)~MFLAG_RUNNING);
			broadcast_member_set_exit_variables(b, m, channel, join_us);
		} else {
			switch_snprintf(tmp, sizeof(tmp), "%u", (unsigned)broadcast_member_resync_load_acq(m));
			switch_channel_set_variable(channel, "broadcast_resync_count", tmp);
			broadcast_member_set_exit_variables(b, m, channel, join_us);
			broadcast_listener_del(b, m);
		}
	}

	/* Systemic safety: never leave this member installed as current_speaker once its session (and the
	 * audio_buffer/audio_in_mutex the producer reads) is about to free. No-op on the normal path where
	 * speaker_main_wait already cleared it. */
	broadcast_clear_speaker_if_current(b, m);
	broadcast_member_cleanup_codecs(m);
	broadcast_release(b);
}

SWITCH_STANDARD_API(broadcast_api_function)
{
	return broadcast_api_dispatch(stream, cmd, SWITCH_FALSE);
}

SWITCH_MODULE_LOAD_FUNCTION(mod_broadcast_load)
{
	switch_application_interface_t *app_interface;
	switch_api_interface_t *api_interface;

	memset(&broadcast_globals, 0, sizeof(broadcast_globals));
	broadcast_globals.pool = pool;
	broadcast_globals.running = 1;
	broadcast_globals.module_load_us = switch_micro_time_now();

	switch_core_hash_init(&broadcast_globals.broadcast_hash);
	switch_thread_rwlock_create(&broadcast_globals.hash_rwlock, pool);
	switch_thread_rwlock_create(&broadcast_globals.config_rwlock, pool);

	if (switch_core_new_memory_pool(&broadcast_globals.config_pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_broadcast: config pool create failed\n");
		return SWITCH_STATUS_GENERR;
	}
	if (broadcast_config_load(broadcast_globals.config_pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_broadcast: config load failed\n");
		switch_core_destroy_memory_pool(&broadcast_globals.config_pool);
		return SWITCH_STATUS_GENERR;
	}

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	SWITCH_ADD_APP(app_interface, "broadcast", "Join a broadcast", "One-to-many audio broadcast",
				   broadcast_app_function, "<name>[@profile][+key=val,...]", SAF_NONE);

	SWITCH_ADD_API(api_interface, "broadcast", "Broadcast control", broadcast_api_function, "broadcast <cmd> [args]");

	if (switch_loadable_module_exists("mod_lua") != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "mod_broadcast: mod_lua is not loaded; Lua DTMF bindings will no-op at runtime\n");
	}
	if (switch_loadable_module_exists("mod_v8") != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "mod_broadcast: mod_v8 is not loaded; Javascript DTMF bindings will no-op at runtime\n");
	}

	broadcast_event_reserve_all();
	if (broadcast_event_emitter_start() != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_broadcast: emitter start failed\n");
		return SWITCH_STATUS_GENERR;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "%s loaded\n", BROADCAST_MODULE_VERSION);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_broadcast_shutdown)
{
	broadcast_globals.running = 0;

	/* Wait for the runtime housekeeping thread to finish its current pass and exit before we
	   free the hash / rwlocks it dereferences. Without this, the loop can be inside
	   broadcast_runtime_housekeeping() (rdlock on hash_rwlock) when the frees below run,
	   producing a use-after-free SIGSEGV on every unload/reload/FS shutdown. */
	{
		int guard = 0;
		while (broadcast_globals.runtime_thread_running && guard++ < 500) {
			switch_yield(20000); /* 20ms; bounded ~10s so a wedged thread can't hang unload */
		}
	}

	/* Join the event emitter BEFORE destroying broadcasts: the emitter iterates broadcast_hash and reads
	   each b->event_ring, but broadcast_destroy frees b->pool (which owns event_ring/event_mutex/b itself)
	   outside hash_rwlock. If destroy_all runs while the emitter is live, the emitter reads a freed pool ->
	   SIGSEGV in broadcast_event_emitter_run (broadcast_events.c). Stopping (joining) the emitter first
	   removes it from the picture before any pool is freed. */
	broadcast_event_emitter_stop();
	broadcast_destroy_all();
	broadcast_event_free_all();
	broadcast_config_unload();
	if (broadcast_globals.config_rwlock) {
		switch_thread_rwlock_destroy(broadcast_globals.config_rwlock);
		broadcast_globals.config_rwlock = NULL;
	}
	if (broadcast_globals.broadcast_hash) {
		switch_core_hash_destroy(&broadcast_globals.broadcast_hash);
		broadcast_globals.broadcast_hash = NULL;
	}
	if (broadcast_globals.hash_rwlock) {
		switch_thread_rwlock_destroy(broadcast_globals.hash_rwlock);
		broadcast_globals.hash_rwlock = NULL;
	}
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_RUNTIME_FUNCTION(mod_broadcast_runtime)
{
	broadcast_globals.runtime_thread_running = 1;
	while (broadcast_globals.running) {
		int i;
		/* ~5s between passes, but in short slices so shutdown (running=0) is seen promptly
		   and we never start a new housekeeping pass after shutdown has begun. */
		for (i = 0; i < 50 && broadcast_globals.running; i++) {
			switch_yield(100000);
		}
		if (!broadcast_globals.running) {
			break;
		}
		broadcast_runtime_housekeeping();
	}
	broadcast_globals.runtime_thread_running = 0;
	return SWITCH_STATUS_TERM;
}
