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
 * broadcast_listener.c — Listener bridge loop (spec §6.4, §10.4).
 *
 * DESIGN NOTE: last_resync_evt_us, dtmf_buf_time_us, and dtmf_buf are written only on the listener
 * bridge thread; no admin reader today. If a future API exposes them to other threads, use atomic
 * load/store (same pattern as producer cflags / member consumer_seq).
 *
 */
#include "mod_broadcast.h"

static void b_listener_dispatch_control(member_obj_t *m, broadcast_control_binding_t *bind)
{
	broadcast_obj_t *b = m->broadcast;
	bctl_action_t act = bind->action;

	switch (act) {
	case BCTL_ACTION_HANGUP:
		switch_channel_set_variable(m->channel, "broadcast_ended_by_self", "true");
		switch_channel_set_variable(m->channel, "broadcast_left_reason", "self-hangup");
		switch_channel_hangup(m->channel, SWITCH_CAUSE_NORMAL_CLEARING);
		break;
	case BCTL_ACTION_LEAVE:
		switch_channel_set_variable(m->channel, "broadcast_ended_by_self", "true");
		switch_channel_set_variable(m->channel, "broadcast_left_reason", "self-leave");
		broadcast_mflags_and_rel(m, (uint32_t)~MFLAG_RUNNING);
		break;
	case BCTL_ACTION_REQUEST_SPEAKER:
		broadcast_enqueue_event(b, BEVT_SPEAKER_REQUEST, m->id, NULL);
		break;
	case BCTL_ACTION_ENERGY_UP: {
		uint32_t t = broadcast_member_energy_threshold_load_acq(m);
		if (t > 100) {
			broadcast_member_energy_threshold_store_rel(m, t - 50);
		}
		break;
	}
	case BCTL_ACTION_ENERGY_DOWN: {
		uint32_t t = broadcast_member_energy_threshold_load_acq(m);
		if (t < 5000) {
			broadcast_member_energy_threshold_store_rel(m, t + 50);
		}
		break;
	}
	case BCTL_ACTION_MUTE_TOGGLE: {
		if (broadcast_member_role_load_acq(m) == BMEMBER_ROLE_SPEAKER) {
			uint32_t mf = broadcast_mflags_load_acq(m);
			if (mf & MFLAG_MUTED_SPEAKER) {
				broadcast_mflags_and_rel(m, (uint32_t)~MFLAG_MUTED_SPEAKER);
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m->session), SWITCH_LOG_INFO, "Speaker m_%u unmuted via DTMF\n", m->id);
			} else {
				broadcast_mflags_or_rel(m, MFLAG_MUTED_SPEAKER);
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m->session), SWITCH_LOG_INFO, "Speaker m_%u muted via DTMF\n", m->id);
			}
		}
		break;
	}
	case BCTL_ACTION_VOL_UP: {
		if (m->vol_idx < 12) {
			m->vol_idx++;
		}
		m->write_gain = (m->vol_idx == 0) ? 1.0f : 1.1f;
		m->read_gain = (m->vol_idx == 0) ? 1.0f : 1.1f;
		break;
	}
	case BCTL_ACTION_VOL_DOWN: {
		if (m->vol_idx > -12) {
			m->vol_idx--;
		}
		m->write_gain = (m->vol_idx == 0) ? 1.0f : 1.1f;
		m->read_gain = (m->vol_idx == 0) ? 1.0f : 1.1f;
		break;
	}
	case BCTL_ACTION_VOL_RESET: {
		m->vol_idx = 0;
		m->write_gain = 1.0f;
		m->read_gain = 1.0f;
		break;
	}
	case BCTL_ACTION_EXECUTE_APP: {
		if (!zstr(bind->arg)) {
			char *app = strdup(bind->arg);
			if (app) {
				char *args = strchr(app, ' ');
				if (args) {
					*args = '\0';
					args++;
					while (*args == ' ') { args++; }
				}
				switch_core_session_execute_application(m->session, app, args);
				free(app);
			}
		}
		break;
	}
	case BCTL_ACTION_LUA: {
		if (!zstr(bind->arg)) {
			if (switch_loadable_module_exists("mod_lua") == SWITCH_STATUS_SUCCESS) {
				switch_core_session_execute_application(m->session, "lua", bind->arg);
			} else {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m->session), SWITCH_LOG_ERROR, "Cannot execute Lua DTMF binding: mod_lua not loaded\n");
			}
		}
		break;
	}
	case BCTL_ACTION_JS: {
		if (!zstr(bind->arg)) {
			if (switch_loadable_module_exists("mod_v8") == SWITCH_STATUS_SUCCESS) {
				switch_core_session_execute_application(m->session, "javascript", bind->arg);
			} else {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m->session), SWITCH_LOG_ERROR, "Cannot execute Javascript DTMF binding: mod_v8 not loaded\n");
			}
		}
		break;
	}
	case BCTL_ACTION_TRANSFER: {
		if (!zstr(bind->arg)) {
			char *args = strdup(bind->arg);
			if (args) {
				char *ext = NULL, *dp = "XML", *ctx = "default";
				char *p = args;
				char *tmp_dp = NULL;
				char *tmp_ctx = NULL;
				ext = switch_str_nil(strtok_r(p, " ", &p));
				if (!zstr(ext)) {
					tmp_dp = strtok_r(NULL, " ", &p);
					if (tmp_dp) {
						dp = tmp_dp;
						tmp_ctx = strtok_r(NULL, " ", &p);
						if (tmp_ctx) {
							ctx = tmp_ctx;
						}
					}
					switch_ivr_session_transfer(m->session, ext, dp, ctx);
				}
				free(args);
			}
		}
		break;
	}
	case BCTL_ACTION_EVENT: {
		char payload[512];
		switch_snprintf(payload, sizeof(payload), "%s|%s", bind->digits, bind->arg ? bind->arg : "");
		broadcast_enqueue_event(b, BEVT_DTMF_ACTION, m->id, payload);
		break;
	}
	default:
		break;
	}
}

static void b_listener_match_controls(member_obj_t *m, const char *buf)
{
	broadcast_control_group_t *g;
	broadcast_control_binding_t *bind;

	if (!m->broadcast->profile) {
		return;
	}
	switch_thread_rwlock_rdlock(broadcast_globals.config_rwlock);
	g = broadcast_config_find_controls_rdlocked(m->broadcast->profile->caller_controls);
	if (!g) {
		switch_thread_rwlock_unlock(broadcast_globals.config_rwlock);
		return;
	}
	for (bind = g->bindings; bind; bind = bind->next) {
		if (!strcmp(buf, bind->digits)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m->session), SWITCH_LOG_NOTICE, "DTMF match: room=%s member=%u digits=%s action=%d\n", m->broadcast->name, m->id, buf, bind->action);
			b_listener_dispatch_control(m, bind);
			switch_thread_rwlock_unlock(broadcast_globals.config_rwlock);
			return;
		}
	}
	switch_thread_rwlock_unlock(broadcast_globals.config_rwlock);
}

/*
 * broadcast_listener_handle_dtmf — Caller-controls dispatch (spec §10.5).
 */
void broadcast_listener_handle_dtmf(member_obj_t *m, const char digit)
{
	size_t len;
	switch_time_t now = switch_micro_time_now();

	if (!digit) {
		return;
	}
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m->session), SWITCH_LOG_NOTICE, "DTMF received: room=%s member=%u digit=%c\n", m->broadcast->name, m->id, digit);
	len = strlen(m->dtmf_buf);
	if (now - m->dtmf_buf_time_us > 1500000) {
		m->dtmf_buf[0] = '\0';
		len = 0;
	}
	m->dtmf_buf_time_us = now;
	if (len + 1 >= sizeof(m->dtmf_buf)) {
		m->dtmf_buf[0] = '\0';
		len = 0;
	}
	m->dtmf_buf[len] = digit;
	m->dtmf_buf[len + 1] = '\0';
	b_listener_match_controls(m, m->dtmf_buf);
}

/*
 * broadcast_listener_run — Blocking output loop for listener role (spec §10.4).
 * No broadcast mutex on read path; uses atomic producer_seq and ring slot seq.
 */
void broadcast_listener_run(member_obj_t *m)
{
	broadcast_obj_t *b = m->broadcast;
	switch_core_session_t *session = m->session;
	switch_channel_t *channel = m->channel;
	switch_frame_t write_frame = {0};
	uint8_t silence_buf[BROADCAST_MAX_FRAME_BYTES];
	uint8_t encode_scratch[BROADCAST_MAX_FRAME_BYTES];
	const char *timer_name;
	uint32_t max_lag;
	switch_dtmf_t dtmf;
	int nat_window = 1;
	uint32_t nat_ticks = 0;
	uint32_t nat_ticks_max;

	m->in_listener_bridge = 1;

	memset(silence_buf, 0, sizeof(silence_buf));
	memset(&write_frame, 0, sizeof(write_frame));
	write_frame.codec = &m->write_codec;
	write_frame.data = encode_scratch;
	write_frame.buflen = sizeof(encode_scratch);
	write_frame.rate = b->rate;
	write_frame.samples = b->samples_per_frame;
	write_frame.datalen = b->bytes_per_frame;

	/* DESIGN NOTE (scalability + NAT): the steady-state listener hot path is WRITE-ONLY, paced by this
	 * private output timer and reading a shared lock-free ring — O(1) per listener, no per-listener decode
	 * or mix. That is what lets one broadcast fan out to far more listeners than mod_conference (whose
	 * O(N^2) personalized mixing is the real saturation point; we deliberately avoid it).
	 *
	 * The catch: FreeSWITCH performs NAT RTP auto-adjust (switch_rtp.c "Auto Changing ... port", which
	 * rewrites our send target from the peer's advertised SDP port to the real source port a NAT is using)
	 * ONLY inside the RTP read path, over a short opening window (~20 packets). A pure write-only loop never
	 * reads, so a NAT'd listener keeps getting audio blasted at its dead SDP port and hears silence.
	 *
	 * Resolution: read_frame ONLY during the opening window (until the core reports a latch via
	 * rtp_auto_adjust_audio, or nat_ticks_max elapses), then drop to the write-only timer loop for the rest
	 * of the call. Reads add no decode (listeners have no read codec) and no shared lock, so even the window
	 * is cheap; steady state is byte-for-byte the original write-only design, so peak listener capacity is
	 * unchanged. NOTE: because RFC2833 DTMF is also parsed in the read path, caller-controls react during
	 * the opening window; SIP INFO DTMF (signaling path) is unaffected. */
	timer_name = (b->profile && b->profile->timer_name) ? b->profile->timer_name : "timerfd";
	if (switch_core_timer_init(&m->output_timer, timer_name, b->interval_ms, b->samples_per_frame, m->pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "listener timer '%s' failed, using soft\n", timer_name);
		if (switch_core_timer_init(&m->output_timer, "soft", b->interval_ms, b->samples_per_frame, m->pool) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "listener timer init failed\n");
			return;
		}
	}

	/* ~3s of opening reads: far longer than the core's ~20-packet auto-adjust window, but a hard cap so a
	 * non-NAT (or silent) listener still leaves the read phase promptly. */
	nat_ticks_max = b->interval_ms ? (3000 / b->interval_ms) : 150;

	max_lag = b->profile && b->profile->max_lag_frames ? b->profile->max_lag_frames : BROADCAST_DEFAULT_MAX_LAG_FRAMES;

	for (;;) {
		uint32_t mf = broadcast_mflags_load_acq(m);
		switch_status_t wst;
		switch_status_t rst;
		switch_frame_t *read_frame = NULL;

		if (!(mf & MFLAG_RUNNING) || !switch_channel_ready(channel) || broadcast_destroyed_load_acq(b)) {
			break;
		}

		if (nat_window) {
			/* Opening phase: pace on the session read so the core drains inbound RTP and can NAT-auto-adjust
			 * our send address. Leave the phase once the core reports a latch, or the cap elapses. */
			rst = switch_core_session_read_frame(session, &read_frame, SWITCH_IO_FLAG_NONE, 0);
			if (!SWITCH_READ_ACCEPTABLE(rst)) {
				break;
			}
			if (++nat_ticks >= nat_ticks_max || switch_channel_get_variable(channel, "rtp_auto_adjust_audio")) {
				nat_window = 0;
				/* Align the steady-state timer to now so its first next() doesn't catch up from init time. */
				switch_core_timer_sync(&m->output_timer);
			}
		} else {
			/* Steady state: write-only, O(1) per listener, no per-listener read/decode (max fan-out). */
			switch_core_timer_next(&m->output_timer);
		}

		while (switch_channel_dequeue_dtmf(channel, &dtmf) == SWITCH_STATUS_SUCCESS) {
			broadcast_listener_handle_dtmf(m, dtmf.digit);
		}

		mf = broadcast_mflags_load_acq(m);
		if ((mf & MFLAG_KICKED) || !(mf & MFLAG_RUNNING) || broadcast_destroyed_load_acq(b)) {
			break;
		}
		if (!switch_channel_ready(channel)) {
			break;
		}

		{
			uint64_t cs = broadcast_member_consumer_seq_load_rlx(m);
			uint64_t pseq = __atomic_load_n(&b->producer_seq, __ATOMIC_ACQUIRE);

			if (cs < pseq) {
				if (pseq - cs > max_lag) {
					char lagbuf[24];
					uint64_t lag = pseq - cs;
					cs = pseq - 1;
					(void)broadcast_member_resync_fetch_inc_rel(m);
					switch_atomic_inc(&b->stat_listener_resyncs);
					switch_snprintf(lagbuf, sizeof(lagbuf), "%" SWITCH_UINT64_T_FMT, lag);
					/* DESIGN NOTE: spec §19.9 coalesce resync events to 1/sec per listener */
					{
						switch_time_t now_us = switch_micro_time_now();
						if (now_us - m->last_resync_evt_us > 1000000) {
							m->last_resync_evt_us = now_us;
							broadcast_enqueue_event(b, BEVT_LISTENER_RESYNC, m->id, lagbuf);
						}
					}
				}

				{
					ring_frame_t *slot = &b->ring[cs % b->ring_size];
					uint64_t slot_seq = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);

					if (slot_seq == cs) {
						memcpy(encode_scratch, slot->data, slot->datalen);
						write_frame.datalen = slot->datalen;
						write_frame.samples = slot->samples;
					} else {
						memcpy(encode_scratch, silence_buf, b->bytes_per_frame);
						write_frame.datalen = b->bytes_per_frame;
						write_frame.samples = b->samples_per_frame;
						cs = pseq - 1;
					}
					cs++;
				}
				broadcast_member_consumer_seq_store_rel(m, cs);
			} else {
				memcpy(encode_scratch, silence_buf, b->bytes_per_frame);
				write_frame.datalen = b->bytes_per_frame;
				write_frame.samples = b->samples_per_frame;
			}
		}

		/* If write_gain != 1.0, scale the L16 samples in encode_scratch before write */
		if (m->write_gain != 1.0f) {
			switch_change_sln_volume_granular((int16_t *)encode_scratch, write_frame.samples, m->vol_idx);
		}

		broadcast_member_last_seq_advance_store_rel(m, switch_micro_time_now());
		wst = switch_core_session_write_frame(session, &write_frame, SWITCH_IO_FLAG_NONE, 0);
		if (wst != SWITCH_STATUS_SUCCESS) {
			break;
		}
	}

	broadcast_mflags_and_rel(m, (uint32_t)~MFLAG_RUNNING);
	switch_core_timer_destroy(&m->output_timer);
	m->in_listener_bridge = 0;
}
