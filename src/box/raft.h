#pragma once
/*
 * Copyright 2010-2020, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <stdint.h>
#include <stdbool.h>
#include "tarantool_ev.h"

#if defined(__cplusplus)
extern "C" {
#endif

struct raft_request;
struct vclock;

enum raft_state {
	RAFT_STATE_FOLLOWER = 1,
	RAFT_STATE_CANDIDATE = 2,
	RAFT_STATE_LEADER = 3,
};

extern const char *raft_state_strs[];

struct raft {
	/** Instance ID of leader of the current term. */
	uint32_t leader;
	/** State of the instance. */
	enum raft_state state;
	/**
	 * Volatile part of the Raft state, whose WAL write may be
	 * still in-progress, and yet the state may be already
	 * used. Volatile state is never sent to anywhere, but the
	 * state machine makes decisions based on it. That is
	 * vital.
	 * As an example, volatile vote needs to be used to reject
	 * votes inside a term, where the instance already voted
	 * (even if the vote WAL write is not finished yet).
	 * Otherwise the instance would try to write several votes
	 * inside one term.
	 */
	uint64_t volatile_term;
	uint32_t volatile_vote;
	/**
	 * Flag whether Raft is enabled. When disabled, it still
	 * persists terms so as to quickly enroll into the cluster
	 * when (if) it is enabled. In everything else disabled
	 * Raft does not affect instance work.
	 */
	bool is_enabled;
	/**
	 * Flag whether the node can become a leader. It is an
	 * accumulated value of configuration options Raft enabled
	 * Raft candidate. If at least one is false - the instance
	 * is not a candidate.
	 */
	bool is_candidate;
	/** Flag whether the instance is allowed to be a leader. */
	bool is_cfg_candidate;
	/**
	 * Flag whether Raft currently tries to write something into WAL. It
	 * happens asynchronously, not right after Raft state is updated.
	 */
	bool is_write_in_progress;
	/**
	 * Persisted Raft state. These values are used when need to tell current
	 * Raft state to other nodes.
	 */
	uint64_t term;
	uint32_t vote;
	/** Bit 1 means that a vote from that instance was obtained. */
	uint32_t vote_mask;
	/** Number of votes for this instance. Valid only in candidate state. */
	int vote_count;
	/** State machine timed event trigger. */
	struct ev_timer timer;
	/**
	 * Dump of Raft state in the end of event loop, when it is changed.
	 */
	struct ev_check io;
	/** Configured election timeout in seconds. */
	double election_timeout;
};

extern struct raft raft;

void
raft_new_term(uint64_t min_new_term);

void
raft_vote(uint32_t vote_for);

static inline bool
raft_is_ro(void)
{
	return raft.is_enabled && raft.state != RAFT_STATE_LEADER;
}

static inline bool
raft_is_source_allowed(uint32_t source_id)
{
	return !raft.is_enabled || raft.leader == source_id;
}

static inline bool
raft_is_enabled(void)
{
	return raft.is_enabled;
}

/** Process a raft entry stored in WAL/snapshot. */
void
raft_process_recovery(const struct raft_request *req);

/**
 * Process a raft status message coming from the network.
 * @param req Raft request.
 * @param source Instance ID of the message sender.
 */
void
raft_process_msg(const struct raft_request *req, uint32_t source);

void
raft_process_heartbeat(uint32_t source);

/**
 * Broadcast the changes in this instance's raft status to all
 * the followers.
 */
void
raft_cfg_is_enabled(bool is_enabled);

void
raft_cfg_is_candidate(bool is_candidate);

void
raft_cfg_election_timeout(double timeout);

void
raft_cfg_election_quorum(void);

void
raft_cfg_death_timeout(void);

/** Save complete Raft state into the request. */
void
raft_serialize(struct raft_request *req, struct vclock *vclock);

/**
 * Broadcast the changes in this instance's raft status to all
 * the followers.
 */
void
raft_broadcast(const struct raft_request *req);

/**
 * Bootstrap the current instance as the first leader of the cluster. That is
 * done bypassing the Raft election protocol, by just assigning this node a
 * leader role. That is needed, because when the cluster is not bootstrapped, it
 * is necessary to find a node, which will generate a replicaset UUID, write it
 * into _cluster space, and register all the other nodes in _cluster.
 * Until it is done, all nodes but one won't boot. Their WALs won't work. And
 * therefore they won't be able to participate in leader election. That
 * effectively makes the cluster dead from the beginning unless the first
 * bootstrapped node won't declare itself a leader without elections.
 *
 * XXX: That does not solve the problem, when the first node boots, creates a
 * snapshot, and then immediately dies. After recovery it won't declare itself a
 * leader. Therefore if quorum > 1, the cluster won't proceed to registering
 * any replicas and becomes completely dead. Perhaps that must be solved by
 * truncating quorum down to number of records in _cluster.
 */
void
raft_bootstrap_leader(void);

void
raft_init(void);

#if defined(__cplusplus)
}
#endif
