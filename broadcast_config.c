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
 * broadcast_config.c — Parse broadcast.conf.xml; profile and caller-controls registry.
 *
 */
#include "mod_broadcast.h"

static const char *CFG_NAME = "broadcast.conf";

/* Copy string param from XML node into pool, or strdup default if missing */
static const char *b_param(switch_xml_t x_profile, switch_memory_pool_t *pool, const char *name, const char *def)
{
	switch_xml_t param;
	for (param = switch_xml_child(x_profile, "param"); param; param = param->next) {
		const char *var = switch_xml_attr_soft(param, "name");
		if (!strcasecmp(var, name)) {
			const char *val = switch_xml_attr_soft(param, "value");
			if (zstr(val)) {
				return def;
			}
			return switch_core_strdup(pool, val);
		}
	}
	return def ? switch_core_strdup(pool, def) : NULL;
}

static uint32_t b_param_uint(switch_xml_t x_profile, const char *name, uint32_t def)
{
	switch_xml_t param;
	for (param = switch_xml_child(x_profile, "param"); param; param = param->next) {
		const char *var = switch_xml_attr_soft(param, "name");
		if (!strcasecmp(var, name)) {
			const char *val = switch_xml_attr_soft(param, "value");
			if (!zstr(val)) {
				return (uint32_t) atoi(val);
			}
		}
	}
	return def;
}

static switch_bool_t b_param_bool(switch_xml_t x_profile, const char *name, switch_bool_t def)
{
	switch_xml_t param;
	for (param = switch_xml_child(x_profile, "param"); param; param = param->next) {
		const char *var = switch_xml_attr_soft(param, "name");
		if (!strcasecmp(var, name)) {
			const char *val = switch_xml_attr_soft(param, "value");
			if (!zstr(val)) {
				return switch_true(val);
			}
		}
	}
	return def;
}

/* Parse one profile XML node into broadcast_profile_t */
static broadcast_profile_t *b_parse_profile(switch_xml_t x_profile, switch_memory_pool_t *pool)
{
	broadcast_profile_t *p;
	const char *name = switch_xml_attr_soft(x_profile, "name");

	if (zstr(name)) {
		return NULL;
	}

	p = switch_core_alloc(pool, sizeof(*p));
	memset(p, 0, sizeof(*p));
	p->pool = pool;
	p->name = switch_core_strdup(pool, name);
	p->rate = b_param_uint(x_profile, "rate", BROADCAST_DEFAULT_RATE);
	p->interval_ms = b_param_uint(x_profile, "interval", BROADCAST_DEFAULT_INTERVAL_MS);
	p->channels = b_param_uint(x_profile, "channels", 1);
	p->ring_size = b_param_uint(x_profile, "ring-size", BROADCAST_DEFAULT_RING_SIZE);
	p->max_lag_frames = b_param_uint(x_profile, "max-lag-frames", BROADCAST_DEFAULT_MAX_LAG_FRAMES);
	p->speaker_override = b_param_bool(x_profile, "speaker-override", SWITCH_FALSE);
	p->silence_window_ms = b_param_uint(x_profile, "silence-window-ms", 800);
	p->silence_policy = (char *) b_param(x_profile, pool, "silence-policy", "zero");
	p->moh_sound = (char *) b_param(x_profile, pool, "moh-sound", "");
	p->max_listeners = b_param_uint(x_profile, "max-listeners", 2000);
	p->listener_pre_buffer_ms = b_param_uint(x_profile, "listener-pre-buffer-ms", 0);
	p->timer_name = (char *) b_param(x_profile, pool, "timer-name", "timerfd");
	p->energy_detection = b_param_bool(x_profile, "energy-detection", SWITCH_TRUE);
	p->comfort_noise_level = b_param_uint(x_profile, "comfort-noise-level", 0);
	p->agc = b_param_bool(x_profile, "agc", SWITCH_FALSE);
	p->enter_sound = (char *) b_param(x_profile, pool, "enter-sound", "");
	p->exit_sound = (char *) b_param(x_profile, pool, "exit-sound", "");
	p->kicked_sound = (char *) b_param(x_profile, pool, "kicked-sound", "");
	p->locked_sound = (char *) b_param(x_profile, pool, "locked-sound", "");
	p->speaker_joined_sound = (char *) b_param(x_profile, pool, "speaker-joined-sound", "");
	p->verbose_events = b_param_bool(x_profile, "verbose-events", SWITCH_FALSE);
	p->join_event_delay_ms = b_param_uint(x_profile, "join-event-delay-ms", 0);
	p->caller_controls = (char *) b_param(x_profile, pool, "caller-controls", "default");
	p->auto_record = b_param_bool(x_profile, "auto-record", SWITCH_FALSE);
	p->record_template = (char *) b_param(x_profile, pool, "record-template",
		"/var/spool/freeswitch/broadcast/${name}-${strftime(%Y%m%d-%H%M%S)}.wav");
	p->heartbeat_interval_sec = b_param_uint(x_profile, "heartbeat-interval-sec", 60);
	return p;
}

/* DESIGN NOTE: DTMF Action Strings:
 * "hangup" -> Hard hangup.
 * "leave" -> Leave cleanly, returning from dialplan app.
 * "request-speaker" -> Signal speaker request event.
 * "energy-up" / "energy-down" -> Adjust talker energy threshold.
 * "mute-toggle" -> Mute speaker.
 * "vol-up" / "vol-down" / "vol-reset" -> Scale listener write gain or speaker read gain.
 * "execute-app" -> Run a FreeSWITCH application (arg: "<app_name> <app_args>").
 * "lua" -> Run a Lua script (arg: "path/to/script.lua").
 * "js" -> Run a JavaScript/V8 script (arg: "path/to/script.js").
 * "transfer" -> Transfer channel (arg: "<ext> [<dp> [<ctx>]]").
 * "event" -> Fire custom dtmf-action event (arg: custom event data).
 */
static bctl_action_t b_parse_action(const char *s)
{
	if (zstr(s)) {
		return BCTL_ACTION_NONE;
	}
	if (!strcasecmp(s, "hangup")) {
		return BCTL_ACTION_HANGUP;
	}
	if (!strcasecmp(s, "request-speaker")) {
		return BCTL_ACTION_REQUEST_SPEAKER;
	}
	if (!strcasecmp(s, "energy-up")) {
		return BCTL_ACTION_ENERGY_UP;
	}
	if (!strcasecmp(s, "energy-down")) {
		return BCTL_ACTION_ENERGY_DOWN;
	}
	if (!strcasecmp(s, "leave")) {
		return BCTL_ACTION_LEAVE;
	}
	if (!strcasecmp(s, "mute-toggle")) {
		return BCTL_ACTION_MUTE_TOGGLE;
	}
	if (!strcasecmp(s, "vol-up")) {
		return BCTL_ACTION_VOL_UP;
	}
	if (!strcasecmp(s, "vol-down")) {
		return BCTL_ACTION_VOL_DOWN;
	}
	if (!strcasecmp(s, "vol-reset")) {
		return BCTL_ACTION_VOL_RESET;
	}
	if (!strcasecmp(s, "execute-app")) {
		return BCTL_ACTION_EXECUTE_APP;
	}
	if (!strcasecmp(s, "lua")) {
		return BCTL_ACTION_LUA;
	}
	if (!strcasecmp(s, "js")) {
		return BCTL_ACTION_JS;
	}
	if (!strcasecmp(s, "transfer")) {
		return BCTL_ACTION_TRANSFER;
	}
	if (!strcasecmp(s, "event")) {
		return BCTL_ACTION_EVENT;
	}
	return BCTL_ACTION_NONE;
}

static void b_parse_control_groups(switch_xml_t cfg, switch_memory_pool_t *pool)
{
	switch_xml_t cc, group, ctl;
	broadcast_control_group_t *ghead = NULL, *gtail = NULL;

	cc = switch_xml_child(cfg, "caller-controls");
	if (!cc) {
		broadcast_globals.control_groups = NULL;
		return;
	}

	for (group = switch_xml_child(cc, "group"); group; group = group->next) {
		const char *gname = switch_xml_attr_soft(group, "name");
		broadcast_control_group_t *g;
		broadcast_control_binding_t *bhead = NULL, *btail = NULL;

		if (zstr(gname)) {
			continue;
		}
		g = switch_core_alloc(pool, sizeof(*g));
		memset(g, 0, sizeof(*g));
		g->name = switch_core_strdup(pool, gname);

		for (ctl = switch_xml_child(group, "control"); ctl; ctl = ctl->next) {
			const char *digits = switch_xml_attr_soft(ctl, "digits");
			const char *action = switch_xml_attr_soft(ctl, "action");
			const char *data = switch_xml_attr_soft(ctl, "data");
			broadcast_control_binding_t *b;
			bctl_action_t act = b_parse_action(action);

			if (zstr(digits) || act == BCTL_ACTION_NONE) {
				continue;
			}
			b = switch_core_alloc(pool, sizeof(*b));
			memset(b, 0, sizeof(*b));
			switch_copy_string(b->digits, digits, sizeof(b->digits));
			b->action = act;
			if (!zstr(data)) {
				b->arg = switch_core_strdup(pool, data);
			}
			if (!bhead) {
				bhead = btail = b;
			} else {
				btail->next = b;
				btail = b;
			}
		}
		g->bindings = bhead;
		if (!ghead) {
			ghead = gtail = g;
		} else {
			gtail->next = g;
			gtail = g;
		}
	}
	broadcast_globals.control_groups = ghead;
}

static void b_free_profiles_list(broadcast_profile_t *p)
{
	(void) p;
	/* Profiles are stored in module pool; freed on unload by destroying pool is not done here */
}

/*
 * broadcast_config_load — Load broadcast.conf.xml into broadcast_globals.
 * Caller must not hold hash_rwlock.
 * Returns SWITCH_STATUS_SUCCESS on success.
 */
switch_status_t broadcast_config_load(switch_memory_pool_t *pool)
{
	switch_xml_t cfg, xml, settings, profiles, x_profile;
	broadcast_profile_t *phead = NULL, *ptail = NULL;

	if (!(xml = switch_xml_open_cfg(CFG_NAME, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "open of %s failed\n", CFG_NAME);
		return SWITCH_STATUS_FALSE;
	}

	broadcast_globals.config_xml = xml;
	broadcast_globals.default_profile_name = switch_core_strdup(pool, "default");
	broadcast_globals.max_broadcasts = 500;
	broadcast_globals.event_ring_size = BROADCAST_EVENT_RING_SIZE;

	settings = switch_xml_child(cfg, "settings");
	if (settings) {
		switch_xml_t param;
		for (param = switch_xml_child(settings, "param"); param; param = param->next) {
			const char *var = switch_xml_attr_soft(param, "name");
			const char *val = switch_xml_attr_soft(param, "value");
			if (!strcasecmp(var, "default-profile") && !zstr(val)) {
				broadcast_globals.default_profile_name = switch_core_strdup(pool, val);
			} else if (!strcasecmp(var, "max-broadcasts") && !zstr(val)) {
				broadcast_globals.max_broadcasts = (uint32_t) atoi(val);
			} else if (!strcasecmp(var, "event-ring-size") && !zstr(val)) {
				broadcast_globals.event_ring_size = (uint32_t) atoi(val);
			}
		}
	}

	profiles = switch_xml_child(cfg, "profiles");
	if (profiles) {
		for (x_profile = switch_xml_child(profiles, "profile"); x_profile; x_profile = x_profile->next) {
			broadcast_profile_t *p = b_parse_profile(x_profile, pool);
			if (!p) {
				continue;
			}
			if (!phead) {
				phead = ptail = p;
			} else {
				ptail->next = p;
				ptail = p;
			}
		}
	}
	broadcast_globals.profiles = phead;

	b_parse_control_groups(cfg, pool);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "broadcast.conf loaded (%s)\n", broadcast_globals.default_profile_name);
	return SWITCH_STATUS_SUCCESS;
}

void broadcast_config_unload(void)
{
	switch_thread_rwlock_wrlock(broadcast_globals.config_rwlock);
	b_free_profiles_list(broadcast_globals.profiles);
	broadcast_globals.profiles = NULL;
	broadcast_globals.control_groups = NULL;
	broadcast_globals.default_profile_name = NULL;
	if (broadcast_globals.config_xml) {
		switch_xml_free(broadcast_globals.config_xml);
		broadcast_globals.config_xml = NULL;
	}
	if (broadcast_globals.config_pool) {
		switch_core_destroy_memory_pool(&broadcast_globals.config_pool);
		broadcast_globals.config_pool = NULL;
	}
	switch_thread_rwlock_unlock(broadcast_globals.config_rwlock);
}

/*
 * broadcast_config_reload — Re-read XML; replaces profile/control tables only.
 * Running broadcasts keep their snapshot profile pointers (cloned from prior loads).
 */
switch_status_t broadcast_config_reload(void)
{
	switch_memory_pool_t *p = NULL;

	switch_thread_rwlock_wrlock(broadcast_globals.config_rwlock);
	b_free_profiles_list(broadcast_globals.profiles);
	broadcast_globals.profiles = NULL;
	broadcast_globals.control_groups = NULL;
	broadcast_globals.default_profile_name = NULL;
	if (broadcast_globals.config_xml) {
		switch_xml_free(broadcast_globals.config_xml);
		broadcast_globals.config_xml = NULL;
	}
	if (broadcast_globals.config_pool) {
		switch_core_destroy_memory_pool(&broadcast_globals.config_pool);
		broadcast_globals.config_pool = NULL;
	}
	if (switch_core_new_memory_pool(&p) != SWITCH_STATUS_SUCCESS) {
		switch_thread_rwlock_unlock(broadcast_globals.config_rwlock);
		return SWITCH_STATUS_FALSE;
	}
	if (broadcast_config_load(p) != SWITCH_STATUS_SUCCESS) {
		switch_core_destroy_memory_pool(&p);
		switch_thread_rwlock_unlock(broadcast_globals.config_rwlock);
		return SWITCH_STATUS_FALSE;
	}
	broadcast_globals.config_pool = p;
	switch_thread_rwlock_unlock(broadcast_globals.config_rwlock);
	return SWITCH_STATUS_SUCCESS;
}

static broadcast_profile_t *b_config_find_profile_unlocked(const char *name)
{
	broadcast_profile_t *p;
	const char *want = zstr(name) ? broadcast_globals.default_profile_name : name;

	for (p = broadcast_globals.profiles; p; p = p->next) {
		if (!strcasecmp(p->name, want)) {
			return p;
		}
	}
	/* fallback default */
	for (p = broadcast_globals.profiles; p; p = p->next) {
		if (!strcasecmp(p->name, "default")) {
			return p;
		}
	}
	return broadcast_globals.profiles;
}

/*
 * broadcast_config_clone_profile — Snapshot a named profile into dst pool under config_rwlock rdlock.
 *
 * Locking: acquires config_rwlock internally; safe to call without holding hash or per-broadcast locks.
 * Lock rank: config_rwlock (0.5) is below hash_rwlock; do not take hash_wrlock while holding config_rwlock
 * unless global ordering is documented elsewhere.
 *
 * Callers must NOT use a raw pointer from b_config_find_profile_unlocked across reload unless they hold
 * config_rwlock rdlock for the entire use window; prefer this clone API for broadcast_create.
 */
broadcast_profile_t *broadcast_config_clone_profile(const char *name, switch_memory_pool_t *pool)
{
	broadcast_profile_t *src;
	broadcast_profile_t *dst;

	switch_thread_rwlock_rdlock(broadcast_globals.config_rwlock);
	src = b_config_find_profile_unlocked(name);
	dst = src ? broadcast_profile_clone(src, pool) : NULL;
	switch_thread_rwlock_unlock(broadcast_globals.config_rwlock);
	return dst;
}

broadcast_profile_t *broadcast_config_find_profile(const char *name)
{
	return b_config_find_profile_unlocked(name);
}

broadcast_control_group_t *broadcast_config_find_controls_rdlocked(const char *name)
{
	broadcast_control_group_t *g;

	if (zstr(name)) {
		return NULL;
	}
	for (g = broadcast_globals.control_groups; g; g = g->next) {
		if (!strcasecmp(g->name, name)) {
			return g;
		}
	}
	return NULL;
}

/*
 * broadcast_profile_clone — Deep copy profile fields into per-broadcast pool (spec §11.3 snapshot).
 */
broadcast_profile_t *broadcast_profile_clone(const broadcast_profile_t *src, switch_memory_pool_t *pool)
{
	broadcast_profile_t *p;

	if (!src) {
		return NULL;
	}
	p = switch_core_alloc(pool, sizeof(*p));
	memset(p, 0, sizeof(*p));
	p->pool = pool;
	p->name = switch_core_strdup(pool, src->name);
	p->rate = src->rate;
	p->interval_ms = src->interval_ms;
	p->channels = src->channels;
	p->ring_size = src->ring_size;
	p->max_lag_frames = src->max_lag_frames;
	p->speaker_override = src->speaker_override;
	p->silence_window_ms = src->silence_window_ms;
	p->silence_policy = switch_core_strdup(pool, src->silence_policy);
	p->moh_sound = switch_core_strdup(pool, src->moh_sound);
	p->max_listeners = src->max_listeners;
	p->listener_pre_buffer_ms = src->listener_pre_buffer_ms;
	p->timer_name = switch_core_strdup(pool, src->timer_name);
	p->energy_detection = src->energy_detection;
	p->comfort_noise_level = src->comfort_noise_level;
	p->agc = src->agc;
	p->enter_sound = switch_core_strdup(pool, src->enter_sound);
	p->exit_sound = switch_core_strdup(pool, src->exit_sound);
	p->kicked_sound = switch_core_strdup(pool, src->kicked_sound);
	p->locked_sound = switch_core_strdup(pool, src->locked_sound);
	p->speaker_joined_sound = switch_core_strdup(pool, src->speaker_joined_sound);
	p->verbose_events = src->verbose_events;
	p->join_event_delay_ms = src->join_event_delay_ms;
	p->caller_controls = switch_core_strdup(pool, src->caller_controls);
	p->auto_record = src->auto_record;
	p->record_template = switch_core_strdup(pool, src->record_template);
	p->heartbeat_interval_sec = src->heartbeat_interval_sec;
	return p;
}
