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
#include "box.h"

#define RAFT_RANDOM_ELECTION_FACTOR 0.1

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
	.volatile_term = 1,
	.volatile_vote = 0,
	.is_enabled = false,
	.is_candidate = false,
	.is_cfg_candidate = false,
	.is_write_in_progress = false,
	.term = 1,
	.vote = 0,
	.vote_mask = 0,
	.vote_count = 0,
	.election_timeout = 5,
};

/**
 * Check if Raft is completely synced with disk. Meaning all its critical values
 * are in WAL. Only in that state the node can become a leader or a candidate.
 * If the node has a not flushed data, it means either the term was bumped, or
 * a new vote was made.
 *
 * In case of term bump it means either there is another node with a newer term,
 * and this one should be a follower; or this node bumped the term itself along
 * with making a vote to start a new election - then it is also a follower which
 * will turn into a candidate when the flush is done.
 *
 * In case of a new not flushed vote it means either this node voted for some
 * other node, and must be a follower; or it voted or self, and also must be a
 * follower, but will become a candidate when the flush is done.
 *
 * In total - when something is not synced with disk, the instance is a follower
 * in any case.
 */
static bool
raft_is_fully_on_disk(void)
{
	return raft.volatile_term == raft.term &&
	       raft.volatile_vote == raft.vote;
}

/**
 * Raft protocol says that election timeout should be a bit randomized so as
 * the nodes wouldn't start election at the same time and end up with not having
 * a quorum for anybody. This implementation randomizes the election timeout by
 * adding {election timeout * random factor} value, where the factor is a
 * constant floating point value > 0.
 */
static inline double
raft_new_random_election_shift(void)
{
	double timeout = raft.election_timeout;
	/* Translate to ms. */
	uint32_t rand_part =
		(uint32_t)(timeout * RAFT_RANDOM_ELECTION_FACTOR * 1000);
	if (rand_part == 0)
		rand_part = 1;
	/*
	 * XXX: this is not giving a good distribution, but it is not so trivial
	 * to implement a correct random value generator. There is a task to
	 * unify all such places. Not critical here.
	 */
	rand_part = rand() % (rand_part + 1);
	return rand_part / 1000.0;
}

/**
 * Raft says that during election a node1 can vote for node2, if node2 has a
 * bigger term, or has the same term but longer log. In case of Tarantool it
 * means the node2 vclock should be >= node1 vclock, in all components. It is
 * not enough to compare only one component. At least because there may be not
 * a previous leader when the election happens first time. Or a node could
 * restart and forget who the previous leader was.
 */
static inline bool
raft_can_vote_for(const struct vclock *v)
{
	if (v == NULL)
		return false;
	int cmp = vclock_compare_ignore0(v, &replicaset.vclock);
	return cmp == 0 || cmp == 1;
}

/** Broadcast an event about this node changed its state to all relays. */
static inline void
raft_broadcast_new_state(void)
{
	struct raft_request req;
	memset(&req, 0, sizeof(req));
	req.term = raft.term;
	req.state = raft.state;
	raft_broadcast(&req);
}

/** Raft state machine methods. 'sm' stands for State Machine. */

/**
 * Start the state machine. When it is stopped, Raft state is updated and
 * goes to WAL when necessary, but it does not affect the instance operation.
 * For example, when Raft is stopped, the instance role does not affect whether
 * it is writable.
 */
static void
raft_sm_start(void);

/**
 * Stop the state machine.
 * - Raft stops affecting the instance operation;
 * - this node can't become a leader anymore;
 * - this node can't vote.
 */
static void
raft_sm_stop(void);

/**
 * When the instance is a follower but is allowed to be a leader, it will wait
 * for death of the current leader to start new election.
 */
static void
raft_sm_wait_leader_dead(void);

/**
 * If election is started by this node, or it voted for some other node started
 * the election, and it can be a leader itself, it will wait until the current
 * election times out. When it happens, the node will start new election.
 */
static void
raft_sm_wait_election_end(void);

/** Bump volatile term and schedule its flush to disk. */
static void
raft_sm_schedule_new_term(uint64_t new_term);

/** Bump volatile vote and schedule its flush to disk. */
static void
raft_sm_schedule_new_vote(uint32_t new_vote);

/** Bump volatile term, vote for self, and schedule their flush to disk. */
static void
raft_sm_schedule_new_election(void);

/**
 * The main trigger of Raft state machine - start new election when the current
 * leader dies, or when there is no a leader and the previous election failed.
 */
static void
raft_sm_schedule_new_election_cb(struct ev_loop *loop, struct ev_timer *timer,
				 int events);

void
raft_process_recovery(const struct raft_request *req)
{
	if (req->term != 0) {
		raft.term = req->term;
		raft.volatile_term = req->term;
	}
	if (req->vote != 0) {
		raft.vote = req->vote;
		raft.volatile_vote = req->vote;
	}
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
raft_process_msg(const struct raft_request *req, uint32_t source)
{
	assert(source > 0);
	assert(source != instance_id);
	/* Outdated request. */
	if (req->term < raft.volatile_term)
		return;

	enum raft_state old_state = raft.state;

	/* Term bump. */
	if (req->term > raft.volatile_term)
		raft_sm_schedule_new_term(req->term);

	/* Vote request during the on-going election. */
	if (req->vote != 0) {
		switch (raft.state) {
		case RAFT_STATE_FOLLOWER:
		case RAFT_STATE_LEADER:
			/*
			 * Can't respond on vote requests when Raft is disabled.
			 */
			if (!raft.is_enabled)
				break;
			/* Check if already voted in this term. */
			if (raft.volatile_vote != 0)
				break;
			/* Not a candidate. Can't accept votes. */
			if (req->vote == instance_id)
				break;
			/* Can't vote for too old or incomparable nodes. */
			if (!raft_can_vote_for(req->vclock))
				break;
			/*
			 * Either the term is new, or didn't vote in the current
			 * term yet. Anyway can vote now.
			 */
			raft.state = RAFT_STATE_FOLLOWER;
			raft_sm_schedule_new_vote(req->vote);
			break;
		case RAFT_STATE_CANDIDATE:
			/* Check if this is a vote for a competing candidate. */
			if (req->vote != instance_id)
				break;
			/*
			 * Vote for self was requested earlier in this round,
			 * and now was answered by some other instance.
			 */
			assert(raft.volatile_vote == instance_id);
			bool was_set = bit_set(&raft.vote_mask, source);
			raft.vote_count += !was_set;
			if (raft.vote_count < replication_synchro_quorum)
				break;
			raft.state = RAFT_STATE_LEADER;
			raft.leader = instance_id;
			break;
		default:
			unreachable();
		}
	}
	/*
	 * If the node does not claim to be a leader, nothing interesting. Terms
	 * and votes are already handled.
	 */
	if (req->state != RAFT_STATE_LEADER)
		goto end;
	/* The node is a leader, but it is already known. */
	if (source == raft.leader)
		goto end;
	/*
	 * XXX: A message from a conflicting leader. Split brain, basically.
	 * Need to decide what to do. Current solution is to do nothing. In
	 * future either this node should try to become a leader, or should stop
	 * all writes and require manual intervention.
	 */
	if (raft.leader != 0)
		goto end;

	/* New leader was elected. */
	raft.state = RAFT_STATE_FOLLOWER;
	raft.leader = source;
end:
	if (raft.state != old_state) {
		/*
		 * If the node stopped being a leader - should become read-only.
		 * If became a leader - should become read-write (if other
		 * subsystems also allow read-write).
		 */
		box_update_ro_summary();
		/*
		 * New term and vote are not broadcasted yet. Firstly their WAL
		 * write should be finished. But the state is volatile. It is ok
		 * to broadcast it now.
		 */
		raft_broadcast_new_state();
	}
}

void
raft_process_heartbeat(uint32_t source)
{
	/*
	 * When not a candidate - don't wait for anything. Therefore does not
	 * care about the leader being dead.
	 */
	if (!raft.is_candidate)
		return;
	/* Don't care about heartbeats when this node is a leader itself. */
	if (raft.state == RAFT_STATE_LEADER)
		return;
	/* Not interested in heartbeats from not a leader. */
	if (raft.leader != source)
		return;
	/*
	 * XXX: it may be expensive to reset the timer like that. It may be less
	 * expensive to let the timer work, and remember last timestamp when
	 * anything was heard from the leader. Then in the timer callback check
	 * the timestamp, and restart the timer, if it is fine.
	 */
	assert(ev_is_active(&raft.timer));
	ev_timer_stop(loop(), &raft.timer);
	raft_sm_wait_leader_dead();
}

void
raft_serialize(struct raft_request *req, struct vclock *vclock)
{
	memset(req, 0, sizeof(*req));
	/*
	 * Volatile state is never used for any communications.
	 * Use only persisted state.
	 */
	req->term = raft.term;
	req->vote = raft.vote;
	req->state = raft.state;
	/*
	 * Raft does not own vclock, so it always expects it passed externally.
	 */
	req->vclock = vclock;
}

/** Wakeup Raft state writer fiber waiting for WAL write end. */
static void
raft_write_cb(struct journal_entry *entry)
{
	fiber_wakeup(entry->complete_data);
}

/** Synchronously write a Raft request into WAL. */
static void
raft_write_request(const struct raft_request *req)
{
	assert(raft.is_write_in_progress);
	/*
	 * Vclock is never persisted by Raft. It is used only to
	 * be sent to network when vote for self.
	 */
	assert(req->vclock == NULL);
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

/**
 * Flush Raft state changes to WAL. The callback resets itself, if during the
 * write more changes appear.
 */
static void
raft_sm_dump_step(ev_loop *loop, ev_check *watcher, int events)
{
	assert(watcher == &raft.io);
	(void) events;
	assert(raft.is_write_in_progress);
	/* During write Raft can't be anything but a follower. */
	assert(raft.state == RAFT_STATE_FOLLOWER);
	struct raft_request req;
	uint64_t old_term = raft.term;
	uint32_t old_vote = raft.vote;
	enum raft_state old_state = raft.state;

	if (raft_is_fully_on_disk()) {
end_dump:
		raft.is_write_in_progress = false;
		ev_check_stop(loop, watcher);
		/*
		 * The state machine is stable. Can see now, to what state to
		 * go.
		 */
		if (!raft.is_candidate) {
			/*
			 * If not a candidate, can't do anything except vote for
			 * somebody (if Raft is enabled). Nothing to do except
			 * staying a follower without timeouts.
			 */
		} else if (raft.leader != 0) {
			/* There is a known leader. Wait until it is dead. */
			raft_sm_wait_leader_dead();
		} else if (raft.vote == instance_id) {
			/* Just wrote own vote. */
			if (replication_synchro_quorum == 1) {
				raft.state = RAFT_STATE_LEADER;
				raft.leader = instance_id;
				/*
				 * Make read-write (if other subsystems allow
				 * that).
				 */
				box_update_ro_summary();
			} else {
				raft.state = RAFT_STATE_CANDIDATE;
				raft.vote_count = 1;
				raft.vote_mask = 0;
				raft_sm_wait_election_end();
			}
		} else if (raft.vote != 0) {
			/*
			 * Voted for some other node. Wait if it manages to
			 * become a leader.
			 */
			raft_sm_wait_election_end();
		} else {
			/* No leaders, no votes. */
			raft_sm_schedule_new_election();
		}
	} else {
		memset(&req, 0, sizeof(req));
		assert(raft.volatile_term >= raft.term);
		/* Term is written always. */
		req.term = raft.volatile_term;
		if (raft.volatile_vote != 0 && raft.volatile_vote != raft.vote)
			req.vote = raft.volatile_vote;

		raft_write_request(&req);

		assert(req.term >= raft.term);
		if (req.term > raft.term) {
			raft.term = req.term;
			raft.vote = 0;
		}
		if (req.vote != 0) {
			assert(raft.vote == 0);
			raft.vote = req.vote;
		}
		if (raft_is_fully_on_disk())
			goto end_dump;
	}

	memset(&req, 0, sizeof(req));
	/* Term is encoded always. */
	req.term = raft.term;
	bool has_changes = old_term != raft.term;
	if (raft.vote != 0 && old_vote != raft.vote) {
		req.vote = raft.vote;
		/*
		 * When vote for self, need to send current vclock too. Two
		 * reasons for that:
		 *
		 * - nodes need to vote for the instance containing the newest
		 *   data. So as not to loose it, because some of it may be
		 *   confirmed by the synchronous replication;
		 *
		 * - replication is basically stopped during election. Other
		 *   nodes can't learn vclock of this instance through regular
		 *   replication.
		 */
		if (raft.vote == instance_id)
			req.vclock = &replicaset.vclock;
		has_changes = true;
	}
	if (raft.state != old_state) {
		req.state = raft.state;
		has_changes = true;
	}
	if (has_changes)
		raft_broadcast(&req);
}

/** Start Raft state flush to disk. */
static void
raft_sm_pause_and_dump(void)
{
	assert(raft.state == RAFT_STATE_FOLLOWER);
	if (raft.is_write_in_progress)
		return;
	ev_timer_stop(loop(), &raft.timer);
	ev_check_start(loop(), &raft.io);
	raft.is_write_in_progress = true;
}

/** Bump term, reset Raft state, and persist that fact. */
static void
raft_sm_schedule_new_term(uint64_t new_term)
{
	assert(new_term > raft.volatile_term);
	assert(raft.volatile_term >= raft.term);
	raft.volatile_term = new_term;
	/* New terms means completely new Raft state. */
	raft.volatile_vote = 0;
	raft.leader = 0;
	raft.state = RAFT_STATE_FOLLOWER;
	raft_sm_pause_and_dump();
}

/** Vote in the current term, and persist that fact. */
static void
raft_sm_schedule_new_vote(uint32_t new_vote)
{
	assert(raft.volatile_vote == 0);
	raft.volatile_vote = new_vote;
	raft_sm_pause_and_dump();
}

/**
 * Bump term and vote for self immediately. After that is persisted, the
 * election timeout will be activated. Unless during that nothing newer happens.
 */
static void
raft_sm_schedule_new_election(void)
{
	assert(raft_is_fully_on_disk());
	assert(raft.is_candidate);
	assert(raft.leader == 0);
	/* Everyone is a follower until its vote for self is persisted. */
	raft_sm_schedule_new_term(raft.term + 1);
	raft_sm_schedule_new_vote(instance_id);
	box_update_ro_summary();
}

void
raft_new_term(uint64_t min_new_term)
{
	uint64_t new_term;
	if (raft.term < min_new_term)
		new_term = min_new_term + 1;
	else
		new_term = raft.term + 1;
	enum raft_state old_state = raft.state;
	raft_sm_schedule_new_term(new_term);
	if (raft.state != old_state)
		raft_broadcast_new_state();
	box_update_ro_summary();
}

static void
raft_sm_schedule_new_election_cb(struct ev_loop *loop, struct ev_timer *timer,
				 int events)
{
	assert(timer == &raft.timer);
	(void)events;
	ev_timer_stop(loop, timer);
	raft_sm_schedule_new_election();
}

static void
raft_sm_wait_leader_dead(void)
{
	assert(!ev_is_active(&raft.timer));
	assert(!ev_is_active(&raft.io));
	assert(!raft.is_write_in_progress);
	assert(raft.is_candidate);
	assert(raft.state == RAFT_STATE_FOLLOWER);
	assert(raft.leader != 0);
	double death_timeout = replication_disconnect_timeout();
	ev_timer_set(&raft.timer, death_timeout, death_timeout);
}

static void
raft_sm_wait_election_end(void)
{
	assert(!ev_is_active(&raft.timer));
	assert(!ev_is_active(&raft.io));
	assert(!raft.is_write_in_progress);
	assert(raft.is_candidate);
	assert(raft.state == RAFT_STATE_FOLLOWER ||
	       (raft.state == RAFT_STATE_CANDIDATE &&
		raft.volatile_vote == instance_id));
	assert(raft.leader == 0);
	double election_timeout = raft.election_timeout +
				  raft_new_random_election_shift();
	ev_timer_set(&raft.timer, election_timeout, election_timeout);
}

static void
raft_sm_start(void)
{
	assert(!ev_is_active(&raft.timer));
	assert(!ev_is_active(&raft.io));
	assert(!raft.is_write_in_progress);
	assert(!raft.is_enabled);
	assert(raft.state == RAFT_STATE_FOLLOWER);
	raft.is_enabled = true;
	raft.is_candidate = raft.is_cfg_candidate;
	if (!raft.is_candidate)
		/* Nop. */;
	else if (raft.leader != 0)
		raft_sm_wait_leader_dead();
	else
		raft_sm_schedule_new_election();
	box_update_ro_summary();
	/*
	 * When Raft is enabled, send the complete state. Because
	 * it wasn't sent in disabled state.
	 */
	struct raft_request req;
	raft_serialize(&req, NULL);
	raft_broadcast(&req);
}

static void
raft_sm_stop(void)
{
	assert(raft.is_enabled);
	raft.is_enabled = false;
	raft.is_candidate = false;
	box_update_ro_summary();
}

void
raft_cfg_is_enabled(bool is_enabled)
{
	if (is_enabled == raft.is_enabled)
		return;

	if (!is_enabled)
		raft_sm_stop();
	else
		raft_sm_start();
}

void
raft_cfg_is_candidate(bool is_candidate)
{
	bool old_is_candidate = raft.is_candidate;
	raft.is_cfg_candidate = is_candidate;
	raft.is_candidate = is_candidate && raft.is_enabled;
	if (raft.is_candidate == old_is_candidate)
		return;

	if (raft.is_candidate) {
		assert(raft.state == RAFT_STATE_FOLLOWER);
		/*
		 * If there is an on-going WAL write, it means
		 * there was some node who sent newer data to this
		 * node.
		 */
		if (raft.leader == 0 && raft_is_fully_on_disk())
			raft_sm_schedule_new_election();
	} else if (raft.state != RAFT_STATE_FOLLOWER) {
		raft.state = RAFT_STATE_FOLLOWER;
		raft_broadcast_new_state();
	}
	box_update_ro_summary();
}

void
raft_cfg_election_timeout(double timeout)
{
	if (timeout == raft.election_timeout)
		return;

	raft.election_timeout = timeout;
	if (raft.vote != 0 && raft.leader == 0) {
		assert(ev_is_active(&raft.timer));
		double timeout = ev_timer_remaining(loop(), &raft.timer) -
				 raft.timer.at + raft.election_timeout;
		ev_timer_stop(loop(), &raft.timer);
		ev_timer_set(&raft.timer, timeout, timeout);
	}
}

void
raft_cfg_election_quorum(void)
{
	if (raft.state != RAFT_STATE_CANDIDATE ||
	    raft.state == RAFT_STATE_LEADER)
		return;
	if (raft.vote_count < replication_synchro_quorum)
		return;
	/*
	 * The node is a candidate. It means its state if fully synced with
	 * disk. Otherwise it would be a follower.
	 */
	assert(!raft.is_write_in_progress);
	raft.state = RAFT_STATE_LEADER;
	raft.leader = instance_id;
	raft_broadcast_new_state();
	box_update_ro_summary();
}

void
raft_cfg_death_timeout(void)
{
	if (raft.state == RAFT_STATE_FOLLOWER && raft.is_candidate &&
	    raft.leader != 0) {
		assert(ev_is_active(&raft.timer));
		double death_timeout = replication_disconnect_timeout();
		double timeout = ev_timer_remaining(loop(), &raft.timer) -
				 raft.timer.at + death_timeout;
		ev_timer_stop(loop(), &raft.timer);
		ev_timer_set(&raft.timer, timeout, timeout);
	}
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

void
raft_bootstrap_leader(void)
{
	assert(raft.is_enabled);
	assert(raft.volatile_term == 0);
	assert(raft.volatile_vote == 0);
	assert(raft.state == RAFT_STATE_FOLLOWER);
	raft.state = RAFT_STATE_LEADER;
	raft_broadcast_new_state();
	box_update_ro_summary();
}

void
raft_init(void)
{
	ev_timer_init(&raft.timer, raft_sm_schedule_new_election_cb, 0, 0);
	ev_check_init(&raft.io, raft_sm_dump_step);
}
