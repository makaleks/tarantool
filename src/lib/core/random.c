/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
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
#include "random.h"
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdlib.h>
#include "say.h"

static int rfd;

static uint64_t state[4];

void
random_init(void)
{
	int seed;
	rfd = open("/dev/urandom", O_RDONLY);
	if (rfd == -1)
		rfd = open("/dev/random", O_RDONLY | O_NONBLOCK);
	if (rfd == -1) {
		struct timeval tv;
		gettimeofday(&tv, 0);
		seed = (getpid() << 16) ^ getuid() ^ tv.tv_sec ^ tv.tv_usec;
		goto srand;
	}

	int flags;
	if ( (flags = fcntl(rfd, F_GETFD)) < 0 ||
	     fcntl(rfd, F_SETFD, flags | FD_CLOEXEC) < 0)
		say_syserror("fcntl, fd=%i", rfd);

	ssize_t res = read(rfd, &seed, sizeof(seed));
	(void) res;
srand:
	srandom(seed);
	srand(seed);

	random_bytes((char *)state, 32);
}

void
random_free(void)
{
	if (rfd == -1)
		return;
	close(rfd);
}

void
random_bytes(char *buf, size_t size)
{
	size_t generated = 0;

	if (rfd == -1)
		goto rand;

	int attempt = 0;
	while (generated < size) {
		ssize_t n = read(rfd, buf + generated, size - generated);
		if (n <= 0) {
			if (attempt++ > 5)
				break;
			continue;
		}
		generated += n;
		attempt = 0;
	}
rand:
	/* fill remaining bytes with PRNG */
	while (generated < size)
		buf[generated++] = rand();
}

uint64_t
real_random(void)
{
	uint64_t result;
	random_bytes((char *)(&result), 8);
	return result;
}

static inline
uint64_t rotl(const uint64_t x, int k)
{
	return (x << k) | (x >> (64 - k));
}

uint64_t
pseudo_random(void)
{
	const uint64_t result = rotl(state[0] + state[3], 23) + state[0];
	const uint64_t t = state[1] << 17;
	state[2] ^= state[0];
	state[3] ^= state[1];
	state[1] ^= state[2];
	state[0] ^= state[3];
	state[2] ^= t;
	state[3] = rotl(state[3], 45);
	return result;
}

static inline uint64_t
random_in_range(uint64_t (*rnd)(void), uint64_t range)
{
	uint64_t mask = UINT64_MAX >> __builtin_clzll(range | 1);
	uint64_t result;
	do {
		result = rnd() & mask;
	} while (result > range);
	return result;
}

int64_t
real_random_in_range(int64_t min, int64_t max)
{
	assert(max >= min);
	return min + random_in_range(real_random, (uint64_t) (max - min));
}

int64_t
pseudo_random_in_range(int64_t min, int64_t max)
{
	assert(max >= min);
	return min + random_in_range(pseudo_random, (uint64_t) (max - min));
}
