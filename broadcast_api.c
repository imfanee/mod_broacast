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
 * broadcast_api.c — CLI/API dispatcher (spec §13) and JSON output.
 *
 * JSON schema (stable keys, UTF-8, one JSON object per response line):
 *   broadcast --json list       -> {"command":"list","broadcasts":[{"name","uuid","profile","listeners","speaker_id","recording"}...],"count":N}
 *   broadcast --json <n> info    -> {"command":"info","name","uuid","profile","profile_rate","profile_interval_ms",
 *                                   "profile_channels","created_at_us","created_utc","uptime_sec","uptime_hms",
 *                                   "listeners","listeners_peak","speaker_id","speaker_uuid","speaker_display",
 *                                   "speaker_active_sec","producer_seq","ticks","missed_ticks","missed_pct",
 *                                   "producer_ticks_per_sec","avg_tick_us","max_tick_us","max_tick_age_sec",
 *                                   "drift_max_us","resyncs","joined","left","events_dropped","cflags_hex",
 *                                   "cflags_str","state","recordings":[{"path","bytes_approx"}...]}
 *   broadcast --json <n> stats  -> {"command":"stats","ticks","missed_ticks",...}
 *   broadcast --json <n> listeners -> {"command":"listeners","members":[{"id","uuid","lag","resync","last_tick_ms_ago"}...]}
 *   broadcast --json version   -> {"command":"version","version":"..."}
 *   broadcast --json reload    -> {"command":"reload","result":"ok"|"error"}
 *   broadcast --json stats     -> module aggregate counters (see b_api_module_stats)
 *   broadcast --json <n> listener <id> -> single listener detail
 *   broadcast --json <n> producer histogram -> bucket counts when BROADCAST_ENABLE_HISTOGRAM
 *   Other subcommands            -> {"command":"<name>","result":"ok"|"error","detail":"..."}
 *
 */
#include "mod_broadcast.h"
#include <inttypes.h>
#include <string.h>

static void b_fmt_utc_from_us(char *buf, size_t len, switch_time_t us)
{
	switch_time_exp_t te = {0};
	switch_size_t rs = 0;

	if (switch_time_exp_lt(&te, us) != SWITCH_STATUS_SUCCESS) {
		switch_copy_string(buf, "?", len);
		return;
	}
	switch_strftime(buf, &rs, len, "%Y-%m-%d %H:%M:%S UTC", &te);
}

static void b_fmt_uptime_hms(char *buf, size_t len, switch_time_t delta_us)
{
	uint64_t t = (uint64_t)(delta_us / 1000000);
	uint64_t h = t / 3600;
	uint64_t m = (t % 3600) / 60;
	uint64_t s = t % 60;

	switch_snprintf(buf, len, "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64, h, m, s);
}

static void b_cflags_to_str(char *buf, size_t len, uint32_t cf)
{
	size_t o = 0;

	buf[0] = '\0';
#define CF_ADD(lbl) do { const char *_l = (lbl); size_t _ll = strlen(_l); if (o > 0 && o + 1 < len) buf[o++] = '|'; if (o + _ll < len) { memcpy(buf + o, _l, _ll); o += _ll; buf[o] = '\0'; } } while (0)
	if (cf & BFLAG_RUNNING) {
		CF_ADD("RUNNING");
	}
	if (cf & BFLAG_DESTRUCT) {
		CF_ADD("DESTRUCT");
	}
	if (cf & BFLAG_LOCKED) {
		CF_ADD("LOCKED");
	}
	if (cf & BFLAG_RECORDING) {
		CF_ADD("RECORDING");
	}
	if (cf & BFLAG_PAUSE) {
		CF_ADD("PAUSE");
	}
	if (cf & BFLAG_DYNAMIC) {
		CF_ADD("DYNAMIC");
	}
	if (cf & BFLAG_PLAY_FILE) {
		CF_ADD("PLAY_FILE");
	}
#undef CF_ADD
}

static uint32_t b_parse_member_id(const char *s)
{
	if (zstr(s)) {
		return 0;
	}
	if (!strncasecmp(s, "m_", 2)) {
		s += 2;
	}
	return (uint32_t) atoi(s);
}

static member_obj_t *b_find_member_by_id_str(broadcast_obj_t *b, const char *sid)
{
	uint32_t id = b_parse_member_id(sid);
	member_obj_t *m;
	if (!id) {
		return NULL;
	}
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

/* Find a member by id-string and PIN its session so the member (whose fields live in the session
 * pool) cannot be freed while the caller operates on it — b_find_member_by_id_str alone returns an
 * unlocked pointer that a concurrently-leaving member can free (e.g. promote_listener installs it as
 * current_speaker, then the producer reads a freed audio_buffer -> SIGSEGV). On success returns the
 * member and sets *sessout to the pinned session; the caller MUST switch_core_session_rwunlock(*sessout).
 * Returns NULL (and *sessout == NULL) if not found or the session is already gone. */
static member_obj_t *b_find_member_pinned(broadcast_obj_t *b, const char *sid, switch_core_session_t **sessout)
{
	uint32_t id = b_parse_member_id(sid);
	member_obj_t *m;
	char uuidbuf[64] = "";
	switch_core_session_t *sess;

	*sessout = NULL;
	if (!id) {
		return NULL;
	}
	/* Copy the member's uuid under the owning lock; do not retain the raw member pointer. */
	switch_thread_rwlock_rdlock(b->listener_lock);
	for (m = b->listener_head; m; m = m->next) {
		if (m->id == id) {
			if (m->uuid) { switch_copy_string(uuidbuf, m->uuid, sizeof(uuidbuf)); }
			break;
		}
	}
	switch_thread_rwlock_unlock(b->listener_lock);
	if (!uuidbuf[0]) {
		switch_thread_rwlock_rdlock(b->speaker_lock);
		if (b->current_speaker && b->current_speaker->id == id && b->current_speaker->uuid) {
			switch_copy_string(uuidbuf, b->current_speaker->uuid, sizeof(uuidbuf));
		}
		switch_thread_rwlock_unlock(b->speaker_lock);
	}
	if (!uuidbuf[0]) {
		return NULL;
	}
	/* Pin the session: its pool (and thus this member) cannot be freed until we rwunlock. */
	sess = switch_core_session_locate(uuidbuf);
	if (!sess) {
		return NULL;
	}
	/* Session pinned -> member is alive; re-resolve the pointer under the lock. */
	m = b_find_member_by_id_str(b, sid);
	if (!m) {
		switch_core_session_rwunlock(sess);
		return NULL;
	}
	*sessout = sess;
	return m;
}

static void b_api_list(switch_stream_handle_t *stream, const char *filter, switch_bool_t json)
{
	switch_hash_index_t *hi;
	uint32_t n = 0;
	int needs_comma = 0;

	if (json) {
		stream->write_function(stream, "{\"command\":\"list\",\"broadcasts\":[");
	}
	switch_thread_rwlock_rdlock(broadcast_globals.hash_rwlock);
	for (hi = switch_core_hash_first(broadcast_globals.broadcast_hash); hi; hi = switch_core_hash_next(&hi)) {
		void *val = NULL;
		broadcast_obj_t *b;

		switch_core_hash_this(hi, NULL, NULL, &val);
		b = (broadcast_obj_t *) val;
		if (!b) {
			continue;
		}
		if (!zstr(filter) && strcasecmp(b->name, filter)) {
			continue;
		}
		n++;
		if (json) {
			uint32_t spid = 0;
			switch_thread_rwlock_rdlock(b->speaker_lock);
			if (b->current_speaker) {
				spid = b->current_speaker->id;
			}
			switch_thread_rwlock_unlock(b->speaker_lock);
			stream->write_function(stream, "%s{\"name\":\"%s\",\"uuid\":\"%s\",\"profile\":\"%s\",\"listeners\":%u,\"speaker_id\":%u,\"recording\":%s}",
								   needs_comma ? "," : "", b->name, b->uuid_str, b->profile ? b->profile->name : "",
								   b->listener_count, spid, (broadcast_cflags_load_acq(b) & BFLAG_RECORDING) ? "true" : "false");
			needs_comma = 1;
		} else {
			stream->write_function(stream, "%s  listeners=%u\n", b->name, b->listener_count);
		}
	}
	switch_thread_rwlock_unlock(broadcast_globals.hash_rwlock);
	if (json) {
		stream->write_function(stream, "],\"count\":%u}\n", n);
	} else {
		if (n == 0) {
			stream->write_function(stream, "0 broadcasts active\n");
		} else {
			stream->write_function(stream, "%u broadcasts active\n", n);
		}
	}
}

static void b_api_module_stats(switch_stream_handle_t *stream, switch_bool_t json)
{
	uint32_t total_listeners = 0;
	uint32_t nbc = 0;
	switch_hash_index_t *hi;

	switch_thread_rwlock_rdlock(broadcast_globals.hash_rwlock);
	for (hi = switch_core_hash_first(broadcast_globals.broadcast_hash); hi; hi = switch_core_hash_next(&hi)) {
		void *val = NULL;
		broadcast_obj_t *b;
		switch_core_hash_this(hi, NULL, NULL, &val);
		b = (broadcast_obj_t *) val;
		if (!b || broadcast_destroyed_load_acq(b)) {
			continue;
		}
		nbc++;
		switch_thread_rwlock_rdlock(b->listener_lock);
		total_listeners += b->listener_count;
		switch_thread_rwlock_unlock(b->listener_lock);
	}
	switch_thread_rwlock_unlock(broadcast_globals.hash_rwlock);

	if (json) {
		stream->write_function(stream,
							   "{\"command\":\"stats\",\"total_broadcasts_created\":%u,\"active_broadcasts\":%u,"
							   "\"total_listeners_served\":%u,\"listeners_now\":%u,\"emitter_running\":%s,"
							   "\"module_uptime_sec\":%" SWITCH_TIME_T_FMT "}\n",
							   (unsigned)switch_atomic_read(&broadcast_globals.total_broadcasts_created),
							   (unsigned)switch_atomic_read(&broadcast_globals.active_broadcasts),
							   (unsigned)switch_atomic_read(&broadcast_globals.total_listeners_served),
							   (unsigned)total_listeners,
							   broadcast_globals.emitter_running ? "true" : "false",
							   (switch_time_t)((switch_micro_time_now() - broadcast_globals.module_load_us) / 1000000));
	} else {
		stream->write_function(stream,
							   "total_broadcasts_created=%u active_broadcasts=%u total_listeners_served=%u listeners_now=%u emitter=%s uptime_sec=%"
							   SWITCH_TIME_T_FMT "\n",
							   (unsigned)switch_atomic_read(&broadcast_globals.total_broadcasts_created),
							   (unsigned)switch_atomic_read(&broadcast_globals.active_broadcasts),
							   (unsigned)switch_atomic_read(&broadcast_globals.total_listeners_served),
							   (unsigned)total_listeners,
							   broadcast_globals.emitter_running ? "yes" : "no",
							   (switch_time_t)((switch_micro_time_now() - broadcast_globals.module_load_us) / 1000000));
	}
}

static void b_api_listener_detail(broadcast_obj_t *b, const char *sid, switch_stream_handle_t *stream, switch_bool_t json)
{
	uint32_t id = b_parse_member_id(sid);
	switch_time_t now = switch_micro_time_now();
	uint64_t pseq = __atomic_load_n(&b->producer_seq, __ATOMIC_ACQUIRE);
	member_obj_t *m;
	int found = 0;
	uint32_t mid = 0;
	char uuid_buf[64] = "", tag_buf[256] = "", disp_buf[256] = "";
	switch_time_t joined_at = 0;
	uint64_t cs = 0, lag;
	uint32_t rsc = 0;
	switch_time_t lastadv = 0;
	int64_t tick_ms_ago = -1;

	/* Copy the member's fields under the owning lock; its strings live in the session pool and can be
	   freed the instant it leaves, so we must not dereference the pointer after releasing the lock. */
	if (id) {
		switch_thread_rwlock_rdlock(b->listener_lock);
		for (m = b->listener_head; m; m = m->next) {
			if (m->id == id) {
				mid = m->id;
				if (m->uuid) { switch_copy_string(uuid_buf, m->uuid, sizeof(uuid_buf)); }
				if (m->tag) { switch_copy_string(tag_buf, m->tag, sizeof(tag_buf)); }
				if (m->display_name) { switch_copy_string(disp_buf, m->display_name, sizeof(disp_buf)); }
				joined_at = m->joined_at;
				cs = broadcast_member_consumer_seq_load_acq(m);
				rsc = broadcast_member_resync_load_acq(m);
				lastadv = broadcast_member_last_seq_advance_load_acq(m);
				found = 1;
				break;
			}
		}
		switch_thread_rwlock_unlock(b->listener_lock);
		if (!found) {
			switch_thread_rwlock_rdlock(b->speaker_lock);
			if (b->current_speaker && b->current_speaker->id == id) {
				m = b->current_speaker;
				mid = m->id;
				if (m->uuid) { switch_copy_string(uuid_buf, m->uuid, sizeof(uuid_buf)); }
				if (m->tag) { switch_copy_string(tag_buf, m->tag, sizeof(tag_buf)); }
				if (m->display_name) { switch_copy_string(disp_buf, m->display_name, sizeof(disp_buf)); }
				joined_at = m->joined_at;
				cs = broadcast_member_consumer_seq_load_acq(m);
				rsc = broadcast_member_resync_load_acq(m);
				lastadv = broadcast_member_last_seq_advance_load_acq(m);
				found = 1;
			}
			switch_thread_rwlock_unlock(b->speaker_lock);
		}
	}

	if (!found) {
		if (json) {
			stream->write_function(stream, "{\"command\":\"listener\",\"result\":\"error\",\"detail\":\"not found\"}\n");
		} else {
			stream->write_function(stream, "-ERR not found\n");
		}
		return;
	}

	if (lastadv > 0 && now >= lastadv) {
		tick_ms_ago = (int64_t)((now - lastadv) / 1000);
	}
	lag = pseq > cs ? pseq - cs : 0;

	if (json) {
		stream->write_function(stream,
							   "{\"command\":\"listener\",\"id\":%u,\"uuid\":\"%s\",\"tag\":\"%s\",\"display_name\":\"%s\","
							   "\"joined_at_us\":%" SWITCH_TIME_T_FMT ",\"duration_ms\":%" SWITCH_TIME_T_FMT ",\"lag\":%" SWITCH_UINT64_T_FMT
							   ",\"resync\":%u,\"last_tick_ms_ago\":%" PRId64 "}\n",
							   mid, uuid_buf, tag_buf, disp_buf,
							   (switch_time_t)joined_at,
							   (switch_time_t)((now - joined_at) / 1000), lag,
							   (unsigned)rsc, tick_ms_ago);
	} else {
		stream->write_function(stream,
							   "Listener m_%u\n  uuid=%s\n  tag=%s\n  display_name=%s\n  joined_at_us=%" SWITCH_TIME_T_FMT "\n  duration_ms=%" SWITCH_TIME_T_FMT
							   "\n  lag_frames=%" SWITCH_UINT64_T_FMT "\n  resync_count=%u\n  last_tick_ms_ago=%" PRId64 "\n",
							   mid, uuid_buf, tag_buf, disp_buf,
							   (switch_time_t)joined_at, (switch_time_t)((now - joined_at) / 1000), lag,
							   (unsigned)rsc, tick_ms_ago);
	}
}

static void b_api_info(broadcast_obj_t *b, switch_stream_handle_t *stream, switch_bool_t json)
{
	uint32_t spid = 0;
	/* Copy speaker strings into local buffers under speaker_lock (below); the speaker member's
	   uuid/display_name live in its session pool and can be freed the instant it leaves, so
	   retaining raw pointers past the unlock is a use-after-free (crash in the write_function). */
	char spdisp_buf[256] = "";
	char spuuid_buf[64] = "";
	const char *spdisp = spdisp_buf;
	const char *spuuid = spuuid_buf;
	switch_time_t avg = 0;
	uint64_t ticks = (uint64_t)switch_atomic_read(&b->stat_ticks);
	uint64_t missed = (uint64_t)switch_atomic_read(&b->stat_missed_ticks);
	switch_time_t now = switch_micro_time_now();
	switch_time_t uptime_us = now - b->created_at;
	double missed_pct = 0.0;
	double tps = 0.0;
	int64_t max_tick_age_sec = -1;
	char created_utc[64];
	char uptime_hms[32];
	char cflagstr[128];
	uint32_t cf = broadcast_cflags_load_acq(b);
	const char *state = "RUNNING";
	char rec_json[1536];
	size_t rj = 0;
	rec_consumer_t *rec;
	char rec_human[512] = "none";

	if (b->producer_tick_count_for_avg) {
		avg = (switch_time_t)(b->producer_tick_sum_us / b->producer_tick_count_for_avg);
	}
	if (b->interval_ms) {
		tps = 1000.0 / (double)b->interval_ms;
	}
	if (ticks > 0) {
		missed_pct = (100.0 * (double)missed) / (double)ticks;
	}
	if (b->producer_max_tick_seen_us > 0 && now >= b->producer_max_tick_seen_us) {
		max_tick_age_sec = (int64_t)((now - b->producer_max_tick_seen_us) / 1000000);
	}
	b_fmt_utc_from_us(created_utc, sizeof(created_utc), b->created_at);
	b_fmt_uptime_hms(uptime_hms, sizeof(uptime_hms), uptime_us);
	b_cflags_to_str(cflagstr, sizeof(cflagstr), cf);
	if (cf & BFLAG_DESTRUCT) {
		state = "DESTROYING";
	} else if (cf & BFLAG_PAUSE) {
		state = "PAUSED";
	}

	switch_thread_rwlock_rdlock(b->speaker_lock);
	if (b->current_speaker) {
		spid = b->current_speaker->id;
		if (b->current_speaker->uuid) {
			switch_copy_string(spuuid_buf, b->current_speaker->uuid, sizeof(spuuid_buf));
		}
		if (b->current_speaker->display_name) {
			switch_copy_string(spdisp_buf, b->current_speaker->display_name, sizeof(spdisp_buf));
		}
	}
	switch_thread_rwlock_unlock(b->speaker_lock);

	rec_json[0] = '[';
	rec_json[1] = '\0';
	rj = 1;
	switch_thread_rwlock_rdlock(b->recording_lock);
	for (rec = b->recording_head; rec; rec = rec->next) {
		uint64_t bytes_approx = (uint64_t)rec->fh.samples_out * (uint64_t)(rec->fh.channels ? rec->fh.channels : 1) * 2ULL;
		const char *tgt = rec->target ? rec->target : "";

		if (rj > 1 && rj + 2 < sizeof(rec_json)) {
			rec_json[rj++] = ',';
			rec_json[rj] = '\0';
		}
		switch_snprintf(rec_json + rj, sizeof(rec_json) - rj,
						"{\"path\":\"%s\",\"bytes_approx\":%" SWITCH_UINT64_T_FMT "}", tgt, bytes_approx);
		rj = strlen(rec_json);
	}
	if (rj + 2 < sizeof(rec_json)) {
		rec_json[rj++] = ']';
		rec_json[rj] = '\0';
	}
	if (b->recording_head && b->recording_head->target) {
		uint64_t ba = (uint64_t)b->recording_head->fh.samples_out * (uint64_t)(b->recording_head->fh.channels ? b->recording_head->fh.channels : 1) * 2ULL;
		switch_snprintf(rec_human, sizeof(rec_human), "%s (~%" SWITCH_UINT64_T_FMT " bytes)", b->recording_head->target, ba);
	}
	switch_thread_rwlock_unlock(b->recording_lock);

	if (json) {
		switch_time_t sp_active_sec = 0;

		switch_thread_rwlock_rdlock(b->speaker_lock);
		if (b->current_speaker && b->speaker_active_since > 0 && now >= b->speaker_active_since) {
			sp_active_sec = (now - b->speaker_active_since) / 1000000;
		}
		switch_thread_rwlock_unlock(b->speaker_lock);

		stream->write_function(stream,
							   "{\"command\":\"info\",\"name\":\"%s\",\"uuid\":\"%s\",\"profile\":\"%s\","
							   "\"profile_rate\":%u,\"profile_interval_ms\":%u,\"profile_channels\":%u,"
							   "\"created_at_us\":%" SWITCH_TIME_T_FMT ",\"created_utc\":\"%s\",\"uptime_sec\":%" SWITCH_TIME_T_FMT ",\"uptime_hms\":\"%s\","
							   "\"listeners\":%u,\"listeners_peak\":%u,\"speaker_id\":%u,\"speaker_uuid\":\"%s\",\"speaker_display\":\"%s\","
							   "\"speaker_active_sec\":%" SWITCH_TIME_T_FMT ",\"producer_seq\":%" SWITCH_UINT64_T_FMT ",\"ticks\":%" SWITCH_UINT64_T_FMT
							   ",\"missed_ticks\":%" SWITCH_UINT64_T_FMT ",\"missed_pct\":%.8f,\"producer_ticks_per_sec\":%.4f,"
							   "\"avg_tick_us\":%" SWITCH_TIME_T_FMT ",\"max_tick_us\":%" SWITCH_TIME_T_FMT ",\"max_tick_age_sec\":%" PRId64 ",\"drift_max_us\":%" SWITCH_UINT64_T_FMT
							   ",\"resyncs\":%u,\"joined\":%u,\"left\":%u,\"events_dropped\":%u,\"cflags_hex\":\"0x%x\",\"cflags_str\":\"%s\","
							   "\"state\":\"%s\",\"recordings\":%s}\n",
							   b->name, b->uuid_str, b->profile ? b->profile->name : "",
							   b->profile ? b->profile->rate : 0, b->profile ? b->profile->interval_ms : 0, b->profile ? b->profile->channels : 0,
							   (switch_time_t)b->created_at, created_utc, (switch_time_t)(uptime_us / 1000000), uptime_hms,
							   b->listener_count, b->stat_peak_listeners, spid, spuuid, spdisp, sp_active_sec,
							   (uint64_t)__atomic_load_n(&b->producer_seq, __ATOMIC_RELAXED), ticks, missed, missed_pct, tps,
							   avg, b->producer_max_tick_us, max_tick_age_sec, (uint64_t)b->stat_tick_drift_us_max,
							   (unsigned)switch_atomic_read(&b->stat_listener_resyncs),
							   (unsigned)switch_atomic_read(&b->stat_listeners_joined), (unsigned)switch_atomic_read(&b->stat_listeners_left),
							   (unsigned)switch_atomic_read(&b->stat_events_dropped), (unsigned)cf, cflagstr, state, rec_json);
	} else {
		switch_time_t sp_active_us = 0;
		char sp_active_hms[32];
		char max_age_buf[64];

		if (max_tick_age_sec >= 0) {
			switch_snprintf(max_age_buf, sizeof(max_age_buf), "%" PRId64 " sec ago", max_tick_age_sec);
		} else {
			switch_copy_string(max_age_buf, "n/a", sizeof(max_age_buf));
		}

		switch_thread_rwlock_rdlock(b->speaker_lock);
		if (b->current_speaker && b->speaker_active_since > 0 && now >= b->speaker_active_since) {
			sp_active_us = now - b->speaker_active_since;
		}
		switch_thread_rwlock_unlock(b->speaker_lock);
		b_fmt_uptime_hms(sp_active_hms, sizeof(sp_active_hms), sp_active_us);

		stream->write_function(stream,
							   "Broadcast: %s\nUUID: %s\nProfile: %s (rate=%u, interval=%ums, channels=%u)\n"
							   "Created: %s (uptime %s)\n"
							   "Speaker: m_%u (display=\"%s\", uuid=%s, active for %s)\n"
							   "Listeners: %u (peak: %u, joined: %u, left: %u)\n"
							   "Producer:\n"
							   "  seq: %" SWITCH_UINT64_T_FMT "\n"
							   "  ticks: %" SWITCH_UINT64_T_FMT " (%.1f/sec)\n"
							   "  missed ticks: %" SWITCH_UINT64_T_FMT " (%.8f%%)\n"
							   "  avg tick: %" SWITCH_TIME_T_FMT " us\n"
							   "  max tick: %" SWITCH_TIME_T_FMT " us (%s)\n"
							   "  max drift: %" SWITCH_UINT64_T_FMT " us\n"
							   "State: %s (flags: %s)\n"
							   "Recording: %s\n"
							   "Resyncs (total): %u\n"
							   "Events dropped: %u\n",
							   b->name, b->uuid_str, b->profile ? b->profile->name : "",
							   b->profile ? b->profile->rate : 0, b->profile ? b->profile->interval_ms : 0, b->profile ? b->profile->channels : 0,
							   created_utc, uptime_hms,
							   spid, spdisp, spuuid, sp_active_hms,
							   b->listener_count, b->stat_peak_listeners,
							   (unsigned)switch_atomic_read(&b->stat_listeners_joined), (unsigned)switch_atomic_read(&b->stat_listeners_left),
							   (uint64_t)__atomic_load_n(&b->producer_seq, __ATOMIC_RELAXED), ticks, tps, missed, missed_pct,
							   avg, b->producer_max_tick_us, max_age_buf,
							   (uint64_t)b->stat_tick_drift_us_max,
							   state, cflagstr,
							   rec_human,
							   (unsigned)switch_atomic_read(&b->stat_listener_resyncs),
							   (unsigned)switch_atomic_read(&b->stat_events_dropped));
	}
}

static void b_api_listeners(broadcast_obj_t *b, switch_stream_handle_t *stream, switch_bool_t json)
{
	member_obj_t *m;
	uint64_t pseq = __atomic_load_n(&b->producer_seq, __ATOMIC_ACQUIRE);
	switch_time_t now_us = switch_micro_time_now();
	int first = 1;

	if (json) {
		stream->write_function(stream, "{\"command\":\"listeners\",\"members\":[");
	}
	switch_thread_rwlock_rdlock(b->listener_lock);
	for (m = b->listener_head; m; m = m->next) {
		uint64_t cs = broadcast_member_consumer_seq_load_acq(m);
		uint64_t lag = pseq > cs ? pseq - cs : 0;
		uint32_t rsc = broadcast_member_resync_load_acq(m);
		switch_time_t lastadv = broadcast_member_last_seq_advance_load_acq(m);
		int64_t tick_ms_ago = -1;
		if (lastadv > 0 && now_us >= lastadv) {
			tick_ms_ago = (int64_t)((now_us - lastadv) / 1000);
		}
		if (json) {
			stream->write_function(stream,
								   "%s{\"id\":%u,\"uuid\":\"%s\",\"lag\":%" SWITCH_UINT64_T_FMT ",\"resync\":%u,\"last_tick_ms_ago\":%" PRId64 "}",
								   first ? "" : ",", m->id, m->uuid, lag, rsc, tick_ms_ago);
			first = 0;
		} else {
			if (tick_ms_ago >= 0) {
				stream->write_function(stream,
									   "m_%u %s lag=%" SWITCH_UINT64_T_FMT " resync=%u last_tick=%" PRId64 "ms_ago\n",
									   m->id, m->uuid, lag, rsc, tick_ms_ago);
			} else {
				stream->write_function(stream,
									   "m_%u %s lag=%" SWITCH_UINT64_T_FMT " resync=%u last_tick=n/a\n",
									   m->id, m->uuid, lag, rsc);
			}
		}
	}
	switch_thread_rwlock_unlock(b->listener_lock);
	if (json) {
		stream->write_function(stream, "]}\n");
	}
}

static void b_api_stats(broadcast_obj_t *b, switch_stream_handle_t *stream, switch_bool_t json)
{
	if (json) {
		stream->write_function(stream,
							   "{\"command\":\"stats\",\"ticks\":%u,\"missed_ticks\":%u,\"speaker_silent_ticks\":%u,"
							   "\"resyncs\":%u,\"joined\":%u,\"left\":%u,\"events_dropped\":%u}\n",
							   switch_atomic_read(&b->stat_ticks), switch_atomic_read(&b->stat_missed_ticks),
							   switch_atomic_read(&b->stat_speaker_silent_ticks), switch_atomic_read(&b->stat_listener_resyncs),
							   switch_atomic_read(&b->stat_listeners_joined), switch_atomic_read(&b->stat_listeners_left),
							   switch_atomic_read(&b->stat_events_dropped));
	} else {
		stream->write_function(stream,
							   "ticks=%u missed=%u silent_ticks=%u resyncs=%u joined=%u left=%u events_dropped=%u\n",
							   switch_atomic_read(&b->stat_ticks), switch_atomic_read(&b->stat_missed_ticks),
							   switch_atomic_read(&b->stat_speaker_silent_ticks), switch_atomic_read(&b->stat_listener_resyncs),
							   switch_atomic_read(&b->stat_listeners_joined), switch_atomic_read(&b->stat_listeners_left),
							   switch_atomic_read(&b->stat_events_dropped));
	}
}

static void b_api_producer(broadcast_obj_t *b, switch_stream_handle_t *stream, switch_bool_t json)
{
	switch_time_t avg = 0;
	if (b->producer_tick_count_for_avg) {
		avg = (switch_time_t)(b->producer_tick_sum_us / b->producer_tick_count_for_avg);
	}
	if (json) {
		stream->write_function(stream,
							   "{\"command\":\"producer\",\"running\":%s,\"producer_seq\":%" SWITCH_UINT64_T_FMT ",\"ticks\":%" SWITCH_UINT64_T_FMT
							   ",\"missed_ticks\":%" SWITCH_UINT64_T_FMT ",\"avg_tick_us\":%" SWITCH_TIME_T_FMT ",\"max_tick_us\":%" SWITCH_TIME_T_FMT
							   ",\"drift_max_us\":%" SWITCH_UINT64_T_FMT "}\n",
							   broadcast_producer_running_load_acq(b) ? "true" : "false",
							   (uint64_t)__atomic_load_n(&b->producer_seq, __ATOMIC_RELAXED),
							   (uint64_t)switch_atomic_read(&b->stat_ticks),
							   (uint64_t)switch_atomic_read(&b->stat_missed_ticks), avg, b->producer_max_tick_us,
							   (uint64_t)b->stat_tick_drift_us_max);
	} else {
		stream->write_function(stream,
							   "producer running=%s seq=%" SWITCH_UINT64_T_FMT " ticks=%" SWITCH_UINT64_T_FMT " missed=%" SWITCH_UINT64_T_FMT
							   " avg_tick_us=%" SWITCH_TIME_T_FMT " max_tick_us=%" SWITCH_TIME_T_FMT " drift_max_us=%" SWITCH_UINT64_T_FMT "\n",
							   broadcast_producer_running_load_acq(b) ? "yes" : "no",
							   (uint64_t)__atomic_load_n(&b->producer_seq, __ATOMIC_RELAXED),
							   (uint64_t)switch_atomic_read(&b->stat_ticks),
							   (uint64_t)switch_atomic_read(&b->stat_missed_ticks), avg, b->producer_max_tick_us,
							   (uint64_t)b->stat_tick_drift_us_max);
	}
}

static void b_api_producer_histogram(broadcast_obj_t *b, switch_stream_handle_t *stream, switch_bool_t json)
{
#ifdef BROADCAST_ENABLE_HISTOGRAM
	if (json) {
		stream->write_function(stream,
							   "{\"command\":\"producer_histogram\",\"b50\":%" SWITCH_UINT64_T_FMT ",\"b100\":%" SWITCH_UINT64_T_FMT
							   ",\"b200\":%" SWITCH_UINT64_T_FMT ",\"b500\":%" SWITCH_UINT64_T_FMT ",\"b1000\":%" SWITCH_UINT64_T_FMT
							   ",\"b5000\":%" SWITCH_UINT64_T_FMT ",\"b20000\":%" SWITCH_UINT64_T_FMT ",\"b_over\":%" SWITCH_UINT64_T_FMT "}\n",
							   b->producer_tick_hist.bucket_us_50, b->producer_tick_hist.bucket_us_100,
							   b->producer_tick_hist.bucket_us_200, b->producer_tick_hist.bucket_us_500,
							   b->producer_tick_hist.bucket_us_1000, b->producer_tick_hist.bucket_us_5000,
							   b->producer_tick_hist.bucket_us_20000, b->producer_tick_hist.bucket_us_over);
	} else {
		stream->write_function(stream,
							   "hist_us: <=50=%" SWITCH_UINT64_T_FMT " <=100=%" SWITCH_UINT64_T_FMT " <=200=%" SWITCH_UINT64_T_FMT
							   " <=500=%" SWITCH_UINT64_T_FMT " <=1k=%" SWITCH_UINT64_T_FMT " <=5k=%" SWITCH_UINT64_T_FMT
							   " <=20k=%" SWITCH_UINT64_T_FMT " over=%" SWITCH_UINT64_T_FMT "\n",
							   b->producer_tick_hist.bucket_us_50, b->producer_tick_hist.bucket_us_100,
							   b->producer_tick_hist.bucket_us_200, b->producer_tick_hist.bucket_us_500,
							   b->producer_tick_hist.bucket_us_1000, b->producer_tick_hist.bucket_us_5000,
							   b->producer_tick_hist.bucket_us_20000, b->producer_tick_hist.bucket_us_over);
	}
#else
	(void)b;
	if (json) {
		stream->write_function(stream, "{\"command\":\"producer_histogram\",\"result\":\"disabled\"}\n");
	} else {
		stream->write_function(stream, "histogram: rebuild mod_broadcast with -DBROADCAST_ENABLE_HISTOGRAM\n");
	}
#endif
}

/*
 * broadcast_api_dispatch — Main API entry (spec §13).
 * Caller: any thread; must not hold broadcast locks.
 */
switch_status_t broadcast_api_dispatch(switch_stream_handle_t *stream, const char *cmd, switch_bool_t json)
{
	char *argv[32] = {0};
	int argc;
	char buf[4096] = "";
	const char *p = cmd;
	switch_bool_t json_mode = json;

	if (zstr(cmd)) {
		stream->write_function(stream, "-ERR\n");
		return SWITCH_STATUS_SUCCESS;
	}

	while (*p == ' ') {
		p++;
	}
	if (!strncasecmp(p, "broadcast ", 10)) {
		p += 10;
	}

	if (!strncasecmp(p, "--json ", 7)) {
		json_mode = SWITCH_TRUE;
		p += 7;
	}

	switch_copy_string(buf, p, sizeof(buf));
	argc = switch_separate_string(buf, ' ', argv, (sizeof(argv) / sizeof(argv[0])) - 1);

	if (argc == 0 || zstr(argv[0])) {
		if (json_mode) {
			stream->write_function(stream, "{\"command\":\"\",\"result\":\"error\",\"detail\":\"usage\"}\n");
		} else {
			stream->write_function(stream, "-ERR usage\n");
		}
		return SWITCH_STATUS_SUCCESS;
	}

	if (!strcasecmp(argv[0], "version")) {
		if (json_mode) {
			stream->write_function(stream, "{\"command\":\"version\",\"version\":\"%s\"}\n", BROADCAST_MODULE_VERSION);
		} else {
			stream->write_function(stream, "%s\n", BROADCAST_MODULE_VERSION);
		}
		return SWITCH_STATUS_SUCCESS;
	}

	if (!strcasecmp(argv[0], "reload")) {
		if (broadcast_config_reload() == SWITCH_STATUS_SUCCESS) {
			if (json_mode) {
				stream->write_function(stream, "{\"command\":\"reload\",\"result\":\"ok\"}\n");
			} else {
				stream->write_function(stream, "+OK\n");
			}
		} else {
			if (json_mode) {
				stream->write_function(stream, "{\"command\":\"reload\",\"result\":\"error\"}\n");
			} else {
				stream->write_function(stream, "-ERR\n");
			}
		}
		return SWITCH_STATUS_SUCCESS;
	}

	if (!strcasecmp(argv[0], "list")) {
		const char *filt = argc > 1 ? argv[1] : NULL;
		b_api_list(stream, filt, json_mode);
		return SWITCH_STATUS_SUCCESS;
	}

	if (!strcasecmp(argv[0], "stats")) {
		b_api_module_stats(stream, json_mode);
		return SWITCH_STATUS_SUCCESS;
	}

	if (argc < 2) {
		stream->write_function(stream, "-ERR\n");
		return SWITCH_STATUS_SUCCESS;
	}

	{
		broadcast_obj_t *b = broadcast_find_and_ref(argv[0]);
		int release_b = 1;

		if (!b) {
			if (json_mode) {
				stream->write_function(stream, "{\"result\":\"error\",\"detail\":\"no such broadcast\"}\n");
			} else {
				stream->write_function(stream, "-ERR no such broadcast\n");
			}
			return SWITCH_STATUS_SUCCESS;
		}

		if (!strcasecmp(argv[1], "info")) {
			b_api_info(b, stream, json_mode);
		} else if (!strcasecmp(argv[1], "listeners")) {
			b_api_listeners(b, stream, json_mode);
		} else if (!strcasecmp(argv[1], "stats")) {
			b_api_stats(b, stream, json_mode);
		} else if (!strcasecmp(argv[1], "producer") && argc >= 3 && !strcasecmp(argv[2], "histogram")) {
			b_api_producer_histogram(b, stream, json_mode);
		} else if (!strcasecmp(argv[1], "producer")) {
			b_api_producer(b, stream, json_mode);
		} else if (!strcasecmp(argv[1], "listener") && argc >= 3) {
			b_api_listener_detail(b, argv[2], stream, json_mode);
		} else if (!strcasecmp(argv[1], "set_speaker") && argc >= 3) {
			switch_core_session_t *ps = NULL;
			member_obj_t *m = b_find_member_pinned(b, argv[2], &ps);
			/* Signal-only: the member's own thread commits the promotion (see broadcast_request_promotion). */
			if (m && broadcast_request_promotion(b, m) == SWITCH_STATUS_SUCCESS) {
				stream->write_function(stream, "+OK\n");
			} else {
				stream->write_function(stream, "-ERR\n");
			}
			if (ps) { switch_core_session_rwunlock(ps); }
		} else if (!strcasecmp(argv[1], "promote_listener") && argc >= 3) {
			switch_core_session_t *ps = NULL;
			member_obj_t *m = b_find_member_pinned(b, argv[2], &ps);
			if (m && broadcast_request_promotion(b, m) == SWITCH_STATUS_SUCCESS) {
				stream->write_function(stream, "+OK\n");
			} else {
				stream->write_function(stream, "-ERR\n");
			}
			if (ps) { switch_core_session_rwunlock(ps); }
		} else if (!strcasecmp(argv[1], "clear_speaker")) {
			broadcast_clear_speaker(b);
			stream->write_function(stream, "+OK\n");
		} else if (!strcasecmp(argv[1], "kick") && argc >= 3) {
			switch_core_session_t *ps = NULL;
			member_obj_t *m = b_find_member_pinned(b, argv[2], &ps);
			if (m && broadcast_member_role_load_acq(m) == BMEMBER_ROLE_LISTENER) {
				broadcast_kick_listener(m, "kicked");
				stream->write_function(stream, "+OK\n");
			} else {
				stream->write_function(stream, "-ERR\n");
			}
			if (ps) { switch_core_session_rwunlock(ps); }
		} else if (!strcasecmp(argv[1], "kick_all")) {
			broadcast_kick_all_listeners(b);
			stream->write_function(stream, "+OK\n");
		} else if (!strcasecmp(argv[1], "lock")) {
			switch_mutex_lock(b->control_mutex);
			broadcast_cflags_or_rel(b, BFLAG_LOCKED);
			switch_mutex_unlock(b->control_mutex);
			broadcast_enqueue_event(b, BEVT_LOCK, 0, NULL);
			stream->write_function(stream, "+OK\n");
		} else if (!strcasecmp(argv[1], "unlock")) {
			switch_mutex_lock(b->control_mutex);
			broadcast_cflags_and_rel(b, (uint32_t)~BFLAG_LOCKED);
			switch_mutex_unlock(b->control_mutex);
			broadcast_enqueue_event(b, BEVT_UNLOCK, 0, NULL);
			stream->write_function(stream, "+OK\n");
		} else if (!strcasecmp(argv[1], "pause")) {
			switch_mutex_lock(b->control_mutex);
			broadcast_cflags_or_rel(b, BFLAG_PAUSE);
			switch_mutex_unlock(b->control_mutex);
			broadcast_enqueue_event(b, BEVT_PAUSE, 0, NULL);
			stream->write_function(stream, "+OK\n");
		} else if (!strcasecmp(argv[1], "resume")) {
			switch_mutex_lock(b->control_mutex);
			broadcast_cflags_and_rel(b, (uint32_t)~BFLAG_PAUSE);
			switch_mutex_unlock(b->control_mutex);
			broadcast_enqueue_event(b, BEVT_RESUME, 0, NULL);
			stream->write_function(stream, "+OK\n");
		} else if (!strcasecmp(argv[1], "record") && argc >= 3) {
			if (broadcast_record_start(b, argv[2], NULL) == SWITCH_STATUS_SUCCESS) {
				stream->write_function(stream, "+OK\n");
			} else {
				stream->write_function(stream, "-ERR\n");
			}
		} else if (!strcasecmp(argv[1], "norecord")) {
			broadcast_record_stop(b, NULL);
			stream->write_function(stream, "+OK\n");
		} else if (!strcasecmp(argv[1], "destroy")) {
			uint32_t grace = 0;
			const char *ann = NULL;
			if (argc >= 3) {
				grace = (uint32_t) atoi(argv[2]);
			}
			if (argc >= 4) {
				ann = argv[3];
			}
			broadcast_destroy_ex(b, grace, ann);
			release_b = 0;
			stream->write_function(stream, "+OK\n");
		} else if (!strcasecmp(argv[1], "dtmf") && argc >= 4) {
			switch_core_session_t *ps = NULL;
			member_obj_t *m = b_find_member_pinned(b, argv[2], &ps);
			if (m && m->session) {
				switch_core_session_send_dtmf_string(m->session, argv[3]);
				stream->write_function(stream, "+OK\n");
			} else {
				stream->write_function(stream, "-ERR\n");
			}
			if (ps) { switch_core_session_rwunlock(ps); }
		} else if (!strcasecmp(argv[1], "play") && argc >= 3) {
			switch_mutex_lock(b->play_mutex);
			if (b->play_fh) {
				switch_core_file_close(b->play_fh);
				b->play_fh = NULL;
			}
			b->play_fh = switch_core_alloc(b->pool, sizeof(switch_file_handle_t));
			memset(b->play_fh, 0, sizeof(*b->play_fh));
			if (switch_core_file_open(b->play_fh, argv[2], b->channels, b->rate,
									  SWITCH_FILE_FLAG_READ | SWITCH_FILE_DATA_SHORT, b->pool) == SWITCH_STATUS_SUCCESS) {
				broadcast_cflags_or_rel(b, BFLAG_PLAY_FILE);
				switch_mutex_unlock(b->play_mutex);
				stream->write_function(stream, "+OK\n");
			} else {
				switch_mutex_unlock(b->play_mutex);
				stream->write_function(stream, "-ERR\n");
			}
		} else if (!strcasecmp(argv[1], "stop_play")) {
			switch_mutex_lock(b->play_mutex);
			if (b->play_fh) {
				switch_core_file_close(b->play_fh);
				b->play_fh = NULL;
			}
			broadcast_cflags_and_rel(b, (uint32_t)~BFLAG_PLAY_FILE);
			switch_mutex_unlock(b->play_mutex);
			stream->write_function(stream, "+OK\n");
		} else {
			stream->write_function(stream, "-ERR unknown subcommand\n");
		}

		if (release_b) {
			broadcast_release(b);
		}
	}

	return SWITCH_STATUS_SUCCESS;
}
