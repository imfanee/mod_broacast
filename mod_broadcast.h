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
 * mod_broadcast.h — Shared types, constants, and public module API.
 *
 */
#ifndef MOD_BROADCAST_H
#define MOD_BROADCAST_H

#include <switch.h>

#define BROADCAST_MODULE_VERSION "mod_broadcast v1.0"

#ifndef BROADCAST_DEFAULT_RING_SIZE
#define BROADCAST_DEFAULT_RING_SIZE 50
#endif
#ifndef BROADCAST_DEFAULT_MAX_LAG_FRAMES
#define BROADCAST_DEFAULT_MAX_LAG_FRAMES 10
#endif
#ifndef BROADCAST_MAX_FRAME_BYTES
#define BROADCAST_MAX_FRAME_BYTES 1920
#endif
#ifndef BROADCAST_DEFAULT_INTERVAL_MS
#define BROADCAST_DEFAULT_INTERVAL_MS 20
#endif
#ifndef BROADCAST_DESTROY_KICK_STACK
#define BROADCAST_DESTROY_KICK_STACK 512
#endif
#ifndef BROADCAST_DEFAULT_RATE
#define BROADCAST_DEFAULT_RATE 8000
#endif
#ifndef BROADCAST_EVENT_RING_SIZE
#define BROADCAST_EVENT_RING_SIZE 1024
#endif
/*
 * Lock order:
 *   hash > config_rwlock > control > speaker > listener > recording > audio_in_mutex
 *
 * Lock rank (lowest = outermost / acquire first): hash_rwlock(0) < config_rwlock(0.5) < per-broadcast
 * control_mutex(1) < speaker_lock(2) < listener_lock(3) < recording_lock(4) < audio_in_mutex(5).
 * event_mutex, play_mutex, moh_mutex, id_mutex: rank below listener_lock unless documented otherwise.
 */

typedef struct broadcast_obj broadcast_obj_t;
typedef struct member_obj member_obj_t;
typedef struct ring_frame ring_frame_t;
typedef struct rec_consumer rec_consumer_t;
typedef struct evt_msg evt_msg_t;
typedef struct broadcast_profile broadcast_profile_t;
typedef struct broadcast_control_group broadcast_control_group_t;
typedef struct broadcast_control_binding broadcast_control_binding_t;

typedef enum {
	BMEMBER_ROLE_SPEAKER = 1,
	BMEMBER_ROLE_LISTENER = 2,
	BMEMBER_ROLE_OBSERVER = 3
} bmember_role_t;

typedef enum {
	BEVT_BROADCAST_CREATE = 1,
	BEVT_BROADCAST_DESTROY,
	BEVT_SPEAKER_SET,
	BEVT_SPEAKER_CLEAR,
	BEVT_SPEAKER_TALKING,
	BEVT_SPEAKER_SILENT,
	BEVT_LISTENER_JOIN,
	BEVT_LISTENER_LEAVE,
	BEVT_LISTENER_RESYNC,
	BEVT_LISTENER_KICKED,
	BEVT_RECORDING_START,
	BEVT_RECORDING_STOP,
	BEVT_PAUSE,
	BEVT_RESUME,
	BEVT_LOCK,
	BEVT_UNLOCK,
	BEVT_SPEAKER_REQUEST,
	BEVT_RECORDING_FAILED,
	BEVT_PRODUCER_STALL,
	BEVT_HEARTBEAT,
	BEVT_DESTROY_GRACE,
	BEVT_DTMF_ACTION
} bevt_type_t;

typedef enum {
	BREC_TYPE_FILE = 1,
	BREC_TYPE_SHOUT = 2,
	BREC_TYPE_STREAM = 3
} brec_type_t;

typedef enum {
	BCTL_ACTION_NONE = 0,
	BCTL_ACTION_HANGUP,
	BCTL_ACTION_REQUEST_SPEAKER,
	BCTL_ACTION_ENERGY_UP,
	BCTL_ACTION_ENERGY_DOWN,
	BCTL_ACTION_LEAVE,
	BCTL_ACTION_MUTE_TOGGLE,
	BCTL_ACTION_VOL_UP,
	BCTL_ACTION_VOL_DOWN,
	BCTL_ACTION_VOL_RESET,
	BCTL_ACTION_EXECUTE_APP,
	BCTL_ACTION_LUA,
	BCTL_ACTION_JS,
	BCTL_ACTION_TRANSFER,
	BCTL_ACTION_EVENT
} bctl_action_t;

#define BFLAG_RUNNING (1U << 0)
#define BFLAG_DESTRUCT (1U << 1)
#define BFLAG_LOCKED (1U << 2)
#define BFLAG_RECORDING (1U << 3)
#define BFLAG_PAUSE (1U << 4)
#define BFLAG_DYNAMIC (1U << 5)
#define BFLAG_PLAY_FILE (1U << 6)

/* cflags: acquire-load in producer / listeners; release RMW from admin paths (pairs with producer). */
#define broadcast_cflags_load_acq(_b) \
	__atomic_load_n((uint32_t *)(void *)&(_b)->cflags, __ATOMIC_ACQUIRE)
#define broadcast_cflags_or_rel(_b, _bits) \
	__atomic_fetch_or((uint32_t *)(void *)&(_b)->cflags, (uint32_t)(_bits), __ATOMIC_RELEASE)
#define broadcast_cflags_and_rel(_b, _mask) \
	__atomic_fetch_and((uint32_t *)(void *)&(_b)->cflags, (uint32_t)(_mask), __ATOMIC_RELEASE)

/* destroyed: release store in broadcast_destroy; acquire-load in bridge threads / enqueue. */
#define broadcast_destroyed_load_acq(_b) \
	__atomic_load_n((int *)(void *)&(_b)->destroyed, __ATOMIC_ACQUIRE)
#define broadcast_destroyed_store_rel(_b, _v) \
	__atomic_store_n((int *)(void *)&(_b)->destroyed, (_v), __ATOMIC_RELEASE)

/* producer_running: release store from destroy / producer exit; acquire-load in producer loop and housekeeping. */
#define broadcast_producer_running_load_acq(_b) \
	__atomic_load_n((int *)(void *)&(_b)->producer_running, __ATOMIC_ACQUIRE)
#define broadcast_producer_running_store_rel(_b, _v) \
	__atomic_store_n((int *)(void *)&(_b)->producer_running, (_v), __ATOMIC_RELEASE)

/* mflags: release RMW from admin / lifecycle; acquire-load in bridge / input threads. */
#define broadcast_mflags_load_acq(_m) \
	__atomic_load_n((uint32_t *)(void *)&(_m)->mflags, __ATOMIC_ACQUIRE)
#define broadcast_mflags_or_rel(_m, _bits) \
	__atomic_fetch_or((uint32_t *)(void *)&(_m)->mflags, (uint32_t)(_bits), __ATOMIC_RELEASE)
#define broadcast_mflags_and_rel(_m, _mask) \
	__atomic_fetch_and((uint32_t *)(void *)&(_m)->mflags, (uint32_t)(_mask), __ATOMIC_RELEASE)

/* member consumer_seq: listener thread writes; API acquire-load for lag (64-bit safe on 32-bit). */
#define broadcast_member_consumer_seq_load_acq(_m) \
	__atomic_load_n(&(_m)->consumer_seq, __ATOMIC_ACQUIRE)
#define broadcast_member_consumer_seq_load_rlx(_m) \
	__atomic_load_n(&(_m)->consumer_seq, __ATOMIC_RELAXED)
#define broadcast_member_consumer_seq_store_rel(_m, _v) \
	__atomic_store_n(&(_m)->consumer_seq, (_v), __ATOMIC_RELEASE)

/* resync_count: listener increments; API / events / channel vars acquire-load. */
#define broadcast_member_resync_fetch_inc_rel(_m) \
	__atomic_fetch_add(&(_m)->resync_count, 1U, __ATOMIC_RELEASE)
#define broadcast_member_resync_load_acq(_m) \
	__atomic_load_n(&(_m)->resync_count, __ATOMIC_ACQUIRE)

/* last_seen_seq_advance: wall time (µs) updated every listener tick for ops vs wedge detection (spec §5.4). */
#define broadcast_member_last_seq_advance_store_rel(_m, _us) \
	__atomic_store_n((int64_t *)(void *)&(_m)->last_seen_seq_advance, (int64_t)(_us), __ATOMIC_RELEASE)
#define broadcast_member_last_seq_advance_load_acq(_m) \
	((switch_time_t)__atomic_load_n((int64_t *)(void *)&(_m)->last_seen_seq_advance, __ATOMIC_ACQUIRE))

/* Speaker input thread: input_running + input_thread pointer (idempotent stop_input). */
#define broadcast_member_input_running_load_acq(_m) \
	__atomic_load_n((int *)(void *)&(_m)->input_running, __ATOMIC_ACQUIRE)
#define broadcast_member_input_running_store_rel(_m, _v) \
	__atomic_store_n((int *)(void *)&(_m)->input_running, (_v), __ATOMIC_RELEASE)
#define broadcast_member_input_thread_load_acq(_m) \
	__atomic_load_n(&(_m)->input_thread, __ATOMIC_ACQUIRE)
#define broadcast_member_input_thread_exchange_null(_m) \
	__atomic_exchange_n(&(_m)->input_thread, NULL, __ATOMIC_ACQ_REL)

/* Energy / VAD fields: input thread vs listener DTMF vs future admin readers. */
#define broadcast_member_energy_last_score_store_rel(_m, _v) \
	__atomic_store_n(&(_m)->energy_last_score, (_v), __ATOMIC_RELEASE)
#define broadcast_member_energy_last_score_load_acq(_m) \
	__atomic_load_n(&(_m)->energy_last_score, __ATOMIC_ACQUIRE)
#define broadcast_member_talking_state_store_rel(_m, _v) \
	__atomic_store_n((int *)(void *)&(_m)->talking_state, (int)(_v), __ATOMIC_RELEASE)
#define broadcast_member_talking_state_load_acq(_m) \
	((switch_bool_t)__atomic_load_n((int *)(void *)&(_m)->talking_state, __ATOMIC_ACQUIRE))
#define broadcast_member_silent_since_store_rel(_m, _us) \
	__atomic_store_n((int64_t *)(void *)&(_m)->silent_since_us, (int64_t)(_us), __ATOMIC_RELEASE)
#define broadcast_member_silent_since_load_acq(_m) \
	((switch_time_t)__atomic_load_n((int64_t *)(void *)&(_m)->silent_since_us, __ATOMIC_ACQUIRE))
#define broadcast_member_energy_threshold_load_acq(_m) \
	__atomic_load_n(&(_m)->energy_threshold, __ATOMIC_ACQUIRE)
#define broadcast_member_energy_threshold_store_rel(_m, _v) \
	__atomic_store_n(&(_m)->energy_threshold, (_v), __ATOMIC_RELEASE)

/* member role: release store on transitions; acquire-load for admin / cross-bridge (v2 promotion). */
#define broadcast_member_role_store_rel(_m, _r) \
	__atomic_store_n((int *)(void *)&(_m)->role, (int)(_r), __ATOMIC_RELEASE)
#define broadcast_member_role_load_acq(_m) \
	((bmember_role_t)__atomic_load_n((int *)(void *)&(_m)->role, __ATOMIC_ACQUIRE))

/* in_speaker_wait: set for broadcast_speaker_main_wait lifetime; destroy polls before freeing broadcast (pool UAF). */
#define broadcast_member_in_speaker_wait_store_rel(_m, _v) \
	__atomic_store_n((int *)(void *)&(_m)->in_speaker_wait, (_v), __ATOMIC_RELEASE)
#define broadcast_member_in_speaker_wait_load_acq(_m) \
	__atomic_load_n((int *)(void *)&(_m)->in_speaker_wait, __ATOMIC_ACQUIRE)

#define broadcast_member_in_listener_bridge_load_acq(_m) \
	__atomic_load_n((int *)(void *)&(_m)->in_listener_bridge, __ATOMIC_ACQUIRE)

#define MFLAG_RUNNING (1U << 0)
#define MFLAG_ITHREAD (1U << 1)
#define MFLAG_KICKED (1U << 2)
#define MFLAG_GHOST (1U << 3)
#define MFLAG_RECORDING_TARGET (1U << 4)
#define MFLAG_ROLE_TRANSITION_TO_SPEAKER (1U << 5)
#define MFLAG_MUTED_SPEAKER (1U << 6)

struct ring_frame {
	uint64_t seq;
	uint32_t datalen;
	uint32_t samples;
	uint8_t data[BROADCAST_MAX_FRAME_BYTES];
	uint8_t silence;
	uint8_t _pad[7];
};

struct broadcast_control_binding {
	char digits[16];
	bctl_action_t action;
	char *arg;
	broadcast_control_binding_t *next;
};

struct broadcast_control_group {
	char *name;
	broadcast_control_binding_t *bindings;
	broadcast_control_group_t *next;
};

struct broadcast_profile {
	char *name;
	switch_memory_pool_t *pool;
	uint32_t rate;
	uint32_t interval_ms;
	uint32_t channels;
	uint32_t ring_size;
	uint32_t max_lag_frames;
	switch_bool_t speaker_override;
	uint32_t silence_window_ms;
	char *silence_policy;
	char *moh_sound;
	uint32_t max_listeners;
	uint32_t listener_pre_buffer_ms;
	char *timer_name;
	switch_bool_t energy_detection;
	uint32_t comfort_noise_level;
	switch_bool_t agc;
	char *enter_sound;
	char *exit_sound;
	char *kicked_sound;
	char *locked_sound;
	char *speaker_joined_sound;
	switch_bool_t verbose_events;
	uint32_t join_event_delay_ms;
	char *caller_controls;
	switch_bool_t auto_record;
	char *record_template;
	/* 0 = no heartbeat events; else interval in seconds for broadcast::heartbeat via housekeeping. */
	uint32_t heartbeat_interval_sec;
	broadcast_profile_t *next;
};

struct broadcast_obj {
	char *name;
	char uuid_str[37];
	switch_memory_pool_t *pool;
	broadcast_profile_t *profile;
	uint32_t rate;
	uint32_t interval_ms;
	uint32_t samples_per_frame;
	uint32_t bytes_per_frame;
	uint8_t channels;
	ring_frame_t *ring;
	uint32_t ring_size;
	uint64_t producer_seq;
	switch_thread_t *producer_thread;
	volatile int producer_running;
	switch_thread_rwlock_t *speaker_lock;
	member_obj_t *current_speaker;
	switch_thread_rwlock_t *listener_lock;
	member_obj_t *listener_head;
	uint32_t listener_count;
	switch_thread_rwlock_t *recording_lock;
	rec_consumer_t *recording_head;
	switch_mutex_t *control_mutex;
	switch_mutex_t *id_mutex;
	uint32_t next_member_id;
	volatile uint32_t cflags;
	char *silence_policy;
	switch_mutex_t *event_mutex;
	evt_msg_t *event_ring;
	uint64_t event_prod_idx;
	uint64_t event_cons_idx;
	uint32_t event_ring_size;
	volatile switch_atomic_t stat_ticks;
	volatile switch_atomic_t stat_missed_ticks;
	volatile switch_atomic_t stat_speaker_silent_ticks;
	volatile switch_atomic_t stat_listener_resyncs;
	volatile switch_atomic_t stat_listeners_joined;
	volatile switch_atomic_t stat_listeners_left;
	volatile switch_atomic_t stat_events_dropped;
#ifdef BROADCAST_ENABLE_HISTOGRAM
	struct {
		uint64_t bucket_us_50;
		uint64_t bucket_us_100;
		uint64_t bucket_us_200;
		uint64_t bucket_us_500;
		uint64_t bucket_us_1000;
		uint64_t bucket_us_5000;
		uint64_t bucket_us_20000;
		uint64_t bucket_us_over;
	} producer_tick_hist;
#endif
	uint64_t stat_tick_drift_us_max;
	switch_time_t producer_first_tick_us;
	switch_time_t last_producer_stall_evt_us;
	switch_time_t last_heartbeat_us;
	uint32_t stat_peak_listeners;
	switch_time_t created_at;
	switch_time_t speaker_active_since;
	volatile int destroyed;
	switch_mutex_t *play_mutex;
	switch_file_handle_t *play_fh;
	uint8_t *silence_zero_buf;
	switch_timer_t producer_tick_timer;
	switch_time_t producer_last_tick_start_us;
	switch_time_t producer_max_tick_us;
	switch_time_t producer_max_tick_seen_us;
	uint64_t producer_tick_count_for_avg;
	uint64_t producer_tick_sum_us;
	switch_mutex_t *moh_mutex;
	switch_file_handle_t moh_fh;
	switch_bool_t moh_open;
	/* Session/API holds via broadcast_find_and_ref; broadcast_create seeds 1; broadcast_destroy waits until <= 1. */
	volatile int32_t live_refs;
};

struct member_obj {
	uint32_t id;
	char *uuid;
	bmember_role_t role;
	switch_memory_pool_t *pool;
	broadcast_obj_t *broadcast;
	switch_core_session_t *session;
	switch_channel_t *channel;
	switch_codec_t read_codec;
	switch_codec_t write_codec;
	switch_codec_implementation_t read_impl;
	switch_buffer_t *audio_buffer;
	switch_mutex_t *audio_in_mutex;
	switch_thread_t *input_thread;
	volatile int input_running;
	uint64_t consumer_seq;
	switch_timer_t output_timer;
	uint32_t resync_count;
	volatile uint32_t mflags;
	member_obj_t *next;
	member_obj_t *prev;
	switch_time_t joined_at;
	switch_time_t last_seen_seq_advance;
	switch_time_t last_resync_evt_us;
	volatile int destroyed;
	char dtmf_buf[16];
	switch_time_t dtmf_buf_time_us;
	char *display_name;
	char *tag;
	uint32_t energy_accum;
	uint32_t energy_last_score;
	switch_time_t silent_since_us;
	switch_bool_t talking_state;
	uint32_t energy_threshold;
	volatile int in_listener_bridge;
	volatile int in_speaker_wait;
	/* Wall time (µs) when this member became current_speaker; 0 if never speaker. */
	switch_time_t speaker_since_us;
	float write_gain;
	float read_gain;
	int32_t vol_idx;
};

struct rec_consumer {
	broadcast_obj_t *broadcast;
	char *target;
	brec_type_t type;
	switch_file_handle_t fh;
	uint64_t consumer_seq;
	switch_thread_t *writer_thread;
	volatile int running;
	switch_memory_pool_t *pool;
	rec_consumer_t *next;
};

struct evt_msg {
	bevt_type_t type;
	uint32_t member_id;
	uint64_t seq;
	switch_time_t when;
	char data[256];
};

struct broadcast_globals {
	switch_memory_pool_t *pool;
	switch_memory_pool_t *config_pool;
	switch_thread_rwlock_t *config_rwlock;
	switch_hash_t *broadcast_hash;
	switch_thread_rwlock_t *hash_rwlock;
	switch_thread_t *event_emitter_thread;
	volatile int emitter_running;
	char *default_profile_name;
	switch_xml_t config_xml;
	broadcast_profile_t *profiles;
	broadcast_control_group_t *control_groups;
	uint32_t max_broadcasts;
	uint32_t event_ring_size;
	volatile switch_atomic_t total_broadcasts_created;
	volatile switch_atomic_t total_listeners_served;
	/* Count of broadcasts in broadcast_hash; O(1) max-broadcasts check (replaces full hash walk on create). */
	volatile switch_atomic_t active_broadcasts;
	switch_time_t module_load_us;
	volatile int running;
	/* Set by mod_broadcast_runtime while its housekeeping loop is alive; shutdown waits on
	   this before freeing hash_rwlock/broadcast_hash the loop dereferences (avoids UAF SIGSEGV). */
	volatile int runtime_thread_running;
};

extern struct broadcast_globals broadcast_globals;

/* broadcast_config.c */
switch_status_t broadcast_config_load(switch_memory_pool_t *pool);
void broadcast_config_unload(void);
switch_status_t broadcast_config_reload(void);
broadcast_profile_t *broadcast_config_clone_profile(const char *name, switch_memory_pool_t *pool);
/* Returned pointer is valid only while config_rwlock rdlock is held.
 * Clone with broadcast_profile_clone before releasing. */
broadcast_profile_t *broadcast_config_find_profile(const char *name);
/*
 * broadcast_config_find_controls_rdlocked — Returns caller-controls group from the live config_pool.
 * Locking: caller MUST hold broadcast_globals.config_rwlock for read (or write) for the entire
 * use of the returned pointer (including any traversal of bindings).
 */
broadcast_control_group_t *broadcast_config_find_controls_rdlocked(const char *name);
broadcast_profile_t *broadcast_profile_clone(const broadcast_profile_t *src, switch_memory_pool_t *pool);

/* broadcast_core.c */
broadcast_obj_t *broadcast_find(const char *name);
broadcast_obj_t *broadcast_find_and_ref(const char *name);
void broadcast_release(broadcast_obj_t *b);
broadcast_obj_t *broadcast_create(const char *name, const char *profile_name, switch_memory_pool_t *parent_pool,
								  switch_core_session_t *optional_session_for_expand);
switch_status_t broadcast_destroy_ex(broadcast_obj_t *b, uint32_t grace_ms, const char *announce_file);
#define broadcast_destroy(_b) broadcast_destroy_ex((_b), 0, NULL)
void broadcast_destroy_all(void);
void broadcast_kick_listener(member_obj_t *m, const char *reason);
void broadcast_kick_all_listeners(broadcast_obj_t *b);
void broadcast_hash_rdlock(void);
void broadcast_hash_rdunlock(void);
void broadcast_hash_wrlock(void);
void broadcast_hash_wrunlock(void);
void broadcast_runtime_housekeeping(void);

/* broadcast_member.c */
switch_status_t broadcast_member_prepare(broadcast_obj_t *b, member_obj_t *m, bmember_role_t role);
switch_status_t broadcast_member_ensure_speaker_audio(broadcast_obj_t *b, member_obj_t *m);
void broadcast_member_cleanup_codecs(member_obj_t *m);
switch_status_t broadcast_listener_add(broadcast_obj_t *b, member_obj_t *m);
switch_status_t broadcast_listener_del(broadcast_obj_t *b, member_obj_t *m);
switch_status_t broadcast_listener_unlink_for_promotion(broadcast_obj_t *b, member_obj_t *m);
uint32_t broadcast_member_next_id(broadcast_obj_t *b);

/* broadcast_producer.c */
void *SWITCH_THREAD_FUNC broadcast_producer_run(switch_thread_t *thread, void *obj);

/* broadcast_speaker.c */
void *SWITCH_THREAD_FUNC broadcast_speaker_input_run(switch_thread_t *thread, void *obj);
switch_status_t broadcast_speaker_start_input(member_obj_t *m);
void broadcast_speaker_stop_input(member_obj_t *m);
void broadcast_speaker_main_wait(member_obj_t *m);
switch_status_t broadcast_request_promotion(broadcast_obj_t *b, member_obj_t *m);
switch_status_t broadcast_set_speaker(broadcast_obj_t *b, member_obj_t *new_speaker);
switch_status_t broadcast_clear_speaker(broadcast_obj_t *b);
void broadcast_clear_speaker_if_current(broadcast_obj_t *b, member_obj_t *m);

/* broadcast_listener.c */
void broadcast_listener_run(member_obj_t *m);
void broadcast_listener_handle_dtmf(member_obj_t *m, const char digit);

/* broadcast_record.c */
switch_status_t broadcast_record_start(broadcast_obj_t *b, const char *target, switch_core_session_t *expand_session);
switch_status_t broadcast_record_stop(broadcast_obj_t *b, const char *target);
void broadcast_record_stop_all(broadcast_obj_t *b);
void *SWITCH_THREAD_FUNC broadcast_record_writer_run(switch_thread_t *thread, void *obj);

/* broadcast_events.c */
void broadcast_enqueue_event(broadcast_obj_t *b, bevt_type_t type, uint32_t member_id, const char *data);
void broadcast_fire_critical_event(broadcast_obj_t *b, bevt_type_t type, uint32_t member_id, const char *data);
switch_status_t broadcast_event_emitter_start(void);
void broadcast_event_emitter_stop(void);
void *SWITCH_THREAD_FUNC broadcast_event_emitter_run(switch_thread_t *thread, void *obj);
void broadcast_event_reserve_all(void);
void broadcast_event_free_all(void);

/* broadcast_api.c */
switch_status_t broadcast_api_dispatch(switch_stream_handle_t *stream, const char *cmd, switch_bool_t json_mode);

#endif
