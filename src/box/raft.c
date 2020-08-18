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
#include "raft.h"

#include "error.h"
#include "journal.h"
#include "xrow.h"
#include "small/region.h"
#include "replication.h"
#include "relay.h"

const char *raft_state_strs[] = {
	NULL,
	"follower",
	"candidate",
	"leader",
};

/** Raft state of this instance. */
struct raft raft = {
	.leader = 0,
	.state = RAFT_STATE_FOLLOWER,
	.is_enabled = false,
	.is_candidate = false,
	.term = 0,
	.vote = 0,
};

void
raft_process_recovery(const struct raft_request *req)
{
	if (req->term != 0)
		raft.term = req->term;
	if (req->vote != 0)
		raft.vote = req->vote;
	/*
	 * Role is never persisted. If recovery is happening, the
	 * node was restarted, and the former role can be false
	 * anyway.
	 */
	assert(req->state == 0);
	/*
	 * Vclock is always persisted by some other subsystem - WAL, snapshot.
	 * It is used only to decide to whom to give the vote during election,
	 * as a part of the volatile state.
	 */
	assert(req->vclock == NULL);
	/* Raft is not enabled until recovery is finished. */
	assert(!raft_is_enabled());
}

void
raft_process_msg(const struct raft_request *req)
{
	if (req->term > raft.term) {
		// Update term.
		// The logic will be similar, but the code
		// below is for testing purposes.
		raft.term = req->term;
	}
	if (req->vote > 0) {
		// Check whether the vote's for us.
	}
	switch (req->state) {
	case RAFT_STATE_FOLLOWER:
	    break;
	case RAFT_STATE_CANDIDATE:
	    // Perform voting logic.
	    break;
	case RAFT_STATE_LEADER:
	    // Switch to a new leader.
	    break;
	default:
	    break;
	}
}

void
raft_serialize(struct raft_request *req, struct vclock *vclock)
{
	memset(req, 0, sizeof(*req));
	req->term = raft.term;
	req->vote = raft.vote;
	req->state = raft.state;
	/*
	 * Raft does not own vclock, so it always expects it passed externally.
	 */
	req->vclock = vclock;
}

static void
raft_write_cb(struct journal_entry *entry)
{
	fiber_wakeup(entry->complete_data);
}

static void
raft_write_request(const struct raft_request *req)
{
	struct region *region = &fiber()->gc;
	uint32_t svp = region_used(region);
	struct xrow_header row;
	char buf[sizeof(struct journal_entry) +
		 sizeof(struct xrow_header *)];
	struct journal_entry *entry = (struct journal_entry *)buf;
	entry->rows[0] = &row;

	if (xrow_encode_raft(&row, region, req) != 0)
		goto fail;
	journal_entry_create(entry, 1, xrow_approx_len(&row), raft_write_cb,
			     fiber());

	if (journal_write(entry) != 0 || entry->res < 0) {
		diag_set(ClientError, ER_WAL_IO);
		diag_log();
		goto fail;
	}

	raft_broadcast(req);

	region_truncate(region, svp);
	return;
fail:
	/*
	 * XXX: the stub is supposed to be removed once it is defined what to do
	 * when a raft request WAL write fails.
	 */
	panic("Could not write a raft request to WAL\n");
}

void
raft_new_term(uint64_t min_new_term)
{
	if (raft.term < min_new_term)
		raft.term = min_new_term + 1;
	else
		++raft.term;

	struct raft_request req;
	memset(&req, 0, sizeof(req));
	req.term = raft.term;
	raft_write_request(&req);
}

void
raft_cfg_is_enabled(bool is_enabled)
{
	raft.is_enabled = is_enabled;
}

void
raft_cfg_is_candidate(bool is_candidate)
{
	raft.is_candidate = is_candidate;
}

void
raft_cfg_election_timeout(double timeout)
{
	raft.election_timeout = timeout;
}

void
raft_cfg_election_quorum(void)
{
}

void
raft_cfg_death_timeout(void)
{
}

void
raft_vote(uint32_t vote_for)
{
	raft.vote = vote_for;

	struct raft_request req;
	memset(&req, 0, sizeof(req));
	req.vote = vote_for;
	raft_write_request(&req);
}

void
raft_broadcast(const struct raft_request *req)
{
	replicaset_foreach(replica) {
		if (replica->relay != NULL && replica->id != REPLICA_ID_NIL &&
		    relay_get_state(replica->relay) == RELAY_FOLLOW) {
			relay_push_raft(replica->relay, req);
		}
	}
}
