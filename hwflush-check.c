/*
 * Copyright (c) 2011-2013 Parallels Inc.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

Compile:
	gcc  hwflush-check.c -o hwflush-check -lpthread
Usage example:
-------
  1. On a server with the hostname test_server, run: hwflush-check -l
  2. On a client, run: hwflush-check -s test_server -d /mnt/test -t 100
  3. Turn off the client, and then turn it on again.
  4. Restart the client: hwflush-check -s test_server -d /mnt/test -t 100
  5. Check the server output for lines containing the message "cache error detected!"

*/
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdint.h>
#include <signal.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <unistd.h>
#include <malloc.h>
#include <stdarg.h>
#include <sys/time.h>


enum prealloc_type {
	PA_NONE = 0,
	PA_POSIX_FALLOC = 1,
	PA_WRITE = 2,
	PA_LAST = PA_WRITE,
};

enum {
	SYNC_MFSYNC,
	SYNC_MFDATASYNC,
	SYNC_FSYNC,
	SYNC_FDATASYNC,
	SYNC_SYNC,
};

static int alloc_type = PA_POSIX_FALLOC;
static int sync_type = SYNC_FDATASYNC;
static int use_dio = 0;
static int send_exit = 0;
static int is_server = 0;
static int is_check_stage = 0;
static int is_prepare = 0;
static char *host = NULL;
static char *port = "32000";
static char *dir = NULL;
/* block size should be a multiply of 8 */
static off_t blocksize = 16 * 1024 - 104;
static off_t blocksmax = 1024 + 1;
static unsigned int threads = 32;
#define THREADS_MAX	1024

static int exit_flag = 0;
static int do_mfsync_ioc(int *fd, int *datasync, int nr);

static void logtime(FILE * fp)
{
	struct timeval tv;
	char buff[128];
	struct tm tm_local;

	gettimeofday(&tv, NULL);
	strftime(buff, sizeof(buff), "%d-%m-%y %H:%M:%S", localtime_r(&tv.tv_sec, &tm_local));
	fprintf(fp, "%s.%03u ", buff, (unsigned int)tv.tv_usec / 1000);
}

static void logerr(const char * fmt, ...)
{
	va_list args;
	logtime(stderr);
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
}

static void logout(const char * fmt, ...)
{
	va_list args;
	logtime(stdout);
	va_start(args, fmt);
	vfprintf(stdout, fmt, args);
	va_end(args);
}

/* returns 0 if ok or -errno if error */
int swrite(int fd, void *buf, int sz)
{
	int w = sz;

	while (w) {
		int n = write(fd, buf, w);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -EIO;
		buf += n;
		w -= n;
	}
	return sz;
}

/* returns number of bytes read */
int sread(int fd, void *buf, int sz)
{
	int r = 0;
	while (sz) {
		int n = read(fd, buf, sz);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			break;
		buf += n;
		r += n;
		sz -= n;
	}
	return r;
}

static int connect_to_server(void)
{
	struct addrinfo *result, *rp, hints;
	int sock = -1;
	int ret;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_V4MAPPED | AI_ADDRCONFIG;

	/* resolve address */
	ret = getaddrinfo(host, port, &hints, &result);
	if (ret != 0) {
		logerr("getaddrinfo() failed: %s\n", gai_strerror(ret));
		return -1;
	}

	/* getaddrinfo() returns a list of address structures.
	   Try each address until we successfully connect(2).
	   If socket(2) (or connect(2)) fails, we (close the socket
	   and) try the next address. */
	for (rp = result; rp != NULL; rp = rp->ai_next) {
		sock = socket(rp->ai_family, rp->ai_socktype,
				rp->ai_protocol);
		if (sock < 0) {
			logerr("Could not create socket: %s\n", strerror(errno));
			continue;
		}

		if (connect(sock, rp->ai_addr, rp->ai_addrlen) < 0) {
			logerr("connect() failed: %s\n", strerror(errno));
			close(sock);
			sock = -1;
			continue;
		}

		logerr("Connected to %s:%s\n", host, port);
		break;	/* Success */
	}

	if (rp == NULL) /* No address succeeded */
		logerr("Could not connect to server\n");

	/* addrinfo is not needed any longer, free it */
	freeaddrinfo(result);

	return sock;
}

static uint64_t find_last_counter(int fd, char *buf, off_t *offset)
{
	uint64_t cnt = 0;
	off_t i, len;

	for (i = 0; i < blocksmax; i++) {
		uint64_t t;
		unsigned int c, j;

		len = sread(fd, buf, blocksize);
		if (len < 0) {
			logerr("read() failed: %s\n", strerror(-len));
			break;
		}
		if (len != blocksize) {
			logerr("Failed to read block %llu\n",
					(unsigned long long)i);
			break;
		}

		t = *(uint64_t*)buf;
		if (cnt >= t)
			break;

		/* validate block */
		memset(&c, t & 0xff, sizeof(c));
		for (j = sizeof(t); j < blocksize; j += sizeof(c))
			if (c != *(unsigned int*)(buf + j))
				break;
		if (j < blocksize) {
			logerr("Block %llu with number %llu is invalid "
				"at %d, blocksize %llu \n", (unsigned long long)i,
				(unsigned long long)t, j,
				(unsigned long long)blocksize);
			break;
		}

		/* ok, block is good, store counter */
		cnt = t;
	}

	*offset = blocksize * i;

	return cnt;
}

/* press Ctrl-C twice on freeze */
static void sighandler(int sig)
{
	if (exit_flag) {
		signal(sig, SIG_DFL);
		raise(sig);
	}
	exit_flag = 1;
}

struct client {
	int sock;
	pthread_mutex_t mutex;
};

struct worker {
	pthread_t thr;
	uint32_t id;
	int32_t ret;
	struct client *cl;
};

struct  cache_state {
	uint64_t gen;
	uint32_t error;
};

enum {
	REP_FL_UPDATE = 1, /* Update generation packet */
	REP_FL_SYNC = 2,   /* Sender want reply*/
	REP_FL_ACK = 4,    /* This is ACK packet */
	REP_FL_ERR = 8,    /* Indicate error  */
	REP_FL_STOP = 0x10,/* Recepient should stop */
};

struct report {
	uint32_t id;
	uint32_t flags;
	uint64_t gen;
} __attribute__((aligned(8)));

int do_sync(int fd) {

	int datasync = 0;
	switch (sync_type) {
	// TODO: batch several files and perform one call
	case SYNC_MFSYNC:
		return do_mfsync_ioc(&fd, &datasync, 1);
	case SYNC_MFDATASYNC:
		datasync = 1;
		return do_mfsync_ioc(&fd, &datasync, 1);
	case SYNC_FSYNC:
		return fsync(fd);
	case SYNC_FDATASYNC:
		return fdatasync(fd);
	case SYNC_SYNC:
		sync();
		return 0;
	default:
		return -EINVAL;
	}
}

static void *run_client_thread(void *arg)
{
	struct worker *w = arg;
	int ret = 0;
	int fd;
	int o_flags;
	off_t offset = 0;
	char *buf;
	char file[strlen(dir) + 6];
	struct report rp = {
		.id = w->id,
		.flags = 0,
		.gen = 0
	};
	struct report reply;

	buf = valloc(blocksize);
	if (!buf) {
		ret = -ENOMEM;
		logerr("malloc() failed\n");
		goto out;
	}

	snprintf(file, sizeof(file), "%s/%04u", dir, w->id);
	/* first try to find last used counter */
	o_flags = O_RDWR;
	if (use_dio)
		o_flags |= O_DIRECT;
	fd = open(file, o_flags, 0666);
	if (fd < 0) {
		if (is_check_stage) {
			rp.gen = 0;
			logerr("Failed to open file '%s': %s\n", file, strerror(errno));
			goto send_report;
		}
		if ((errno != ENOENT) || ((fd = creat(file, 0666)) < 0)) {
			ret = fd;
			logerr("Failed to open file '%s': %s\n", file, strerror(errno));
			goto out_free;
		}
		switch (alloc_type) {
		case PA_NONE:
			break;
		case PA_POSIX_FALLOC:
			if (posix_fallocate(fd, 0, blocksize * blocksmax) < 0) {
				ret = -ENOSPC;
				logerr("fallocate() failed: %s\n",
					strerror(errno));
				goto out_close_fd;
			}
			break;
		case PA_WRITE: {
			off_t num, count = blocksize * blocksmax;
			int ret;
			memset(buf, 0, blocksize);
			while (count) {
				num = blocksize < count ? blocksize : count;
				ret = write(fd, buf, num);
				if (ret < 0) {
					logerr("write() failed: %s\n",
						strerror(errno));
					goto out_close_fd;
				}
				count -= ret;
			}
			lseek(fd, 0, SEEK_SET);
			break;
		}
		default:
			ret = -EINVAL;
			logerr("Incorrect prealloc type ");
			goto out_close_fd;
			break;
		}
		rp.gen = 0;
	} else {
		rp.gen = find_last_counter(fd, buf, &offset);
		if ((ret = lseek(fd, offset, SEEK_SET)) < 0) {
			logerr("lseek() failed: %s\n", strerror(errno));
			goto out_close_fd;
		}
		logerr("id %u: latest valid id %llu\n", w->id, (unsigned long long)rp.gen);

	}
send_report:
	rp.id = w->id;
	rp.flags = REP_FL_SYNC;
	if (send_exit)
		rp.flags |= REP_FL_STOP;

	pthread_mutex_lock(&w->cl->mutex);
	ret = swrite(w->cl->sock, &rp, sizeof(rp));
	if (ret < 0 ) {
		logerr("Failed to write to socket: %s\n", strerror(-ret));
		goto out_unlock;
	}
	ret = sread(w->cl->sock, &reply, sizeof(reply));
	pthread_mutex_unlock(&w->cl->mutex);
	if (ret != sizeof(reply)) {
		logerr("Corrupted msg from server id:%d, got:%d expect:%ld\n",
			rp.id, ret, sizeof(reply));
		ret = -EINVAL;
		goto out_close_fd;
	}
	if (reply.id != w->id || !(reply.flags & REP_FL_ACK)) {
		ret = -EINVAL;
		logerr("Bad replay from server id:%d \n", rp.id);
		goto out_close_fd;
	}
	ret = 0;

	if (reply.flags & REP_FL_ERR) {
		ret = -EIO;
		logout("id %d: Server reported cache error, "
		       " server idx %llu > disk idx  %llu \n",
		       w->id, (unsigned long long)reply.gen,
		       (unsigned long long)rp.gen);
		goto out_close_fd;
	}

	if (is_check_stage || send_exit)
		goto out_close_fd;

	if ((ret = do_sync(fd))) {
		logerr("do_sync(2) failed: %s\n", strerror(errno));
		goto out_close_fd;
	}
	if (is_prepare)
		goto out_close_fd;

	rp.flags = REP_FL_UPDATE;
	while (!exit_flag) {
		int r;

		if (offset >= blocksize * blocksmax) {
			offset = 0;
			lseek(fd, 0, SEEK_SET);
		}

		rp.gen++;
		*(uint64_t*)buf = rp.gen;
		memset(buf + sizeof(rp.gen), rp.gen & 0xff, blocksize - sizeof(rp.gen));
		r = swrite(fd, buf, blocksize);
		if (r != blocksize) {
			logerr("Failed to write to file '%s': %s\n", file, strerror(-r));
			rp.flags |= REP_FL_ERR | REP_FL_STOP;
		}
		ret = do_sync(fd);
		if (ret < 0) {
			logerr("do_sync() failed: %s\n", strerror(errno));
			rp.flags |= REP_FL_ERR | REP_FL_STOP;
		}

		pthread_mutex_lock(&w->cl->mutex);
		r = swrite(w->cl->sock, &rp, sizeof(rp));
		pthread_mutex_unlock(&w->cl->mutex);
		if (r < 0) {
			logerr("Failed to write to socket: %s\n", strerror(-r));
			break;
		}

		offset += blocksize;
	}

out_close_fd:
	if (fd >= 0)
		close(fd);
out_free:
	free(buf);
out:
	pthread_mutex_lock(&w->cl->mutex);
	w->ret = ret;
	pthread_mutex_unlock(&w->cl->mutex);
	return NULL;
out_unlock:
	pthread_mutex_unlock(&w->cl->mutex);
	goto out_close_fd;
}

static int run_client(void)
{
	struct stat st;
	int ret = 0;
	int flag = 1;
	int i;
	struct client clnt;
	struct worker *thrs;

	if (stat(dir, &st) < 0) {
		if (errno != ENOENT) {
			logerr("stat() for '%s' failed: %s\n", dir, strerror(errno));
			return -1;
		}
		if (mkdir(dir, 0777) < 0) {
			logerr("Failed to create directory '%s': %s\n", dir, strerror(errno));
			return -1;
		}
	} else if (!S_ISDIR(st.st_mode)) {
		logerr("'%s' is not a directory\n", dir);
		return -1;
	}

	clnt.sock = connect_to_server();
	if (clnt.sock < 0)
		return -1;

	if (setsockopt(clnt.sock, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(int)) < 0) {
		logerr("setsockopt(TCP_NODELAY) failed: %s\n", strerror(errno));
		ret = -1;
		goto out_close_sock;
	}

	/* make things fancier for the server */
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
	signal(SIGPIPE, SIG_IGN);

	thrs = malloc(threads * sizeof(struct worker));
	if (!thrs) {
		logerr("malloc() failed\n");
		ret = -1;
		goto out_close_sock;
	}

	pthread_mutex_init(&clnt.mutex, NULL);

	for (i = 0; i < threads; i++) {
		thrs[i].id = i;
		thrs[i].cl = &clnt;
		thrs[i].ret = 0;
		if (pthread_create(&thrs[i].thr, NULL, run_client_thread, (void*)&thrs[i])) {
			logerr("Failed to start thread %u\n", i);
			ret = -1;
			break;
		}
	}

	for (i--; i >= 0; i--)
		pthread_join(thrs[i].thr, NULL);

	for (i = 0; i < threads; i++) {
		if (thrs[i].ret != 0) {
			logerr("Thread %d failed with:%d %s\n", thrs[i].id,
			       thrs[i].ret, strerror(-thrs[i].ret));
			if (!ret)
				ret = thrs[i].ret;
		}
	}

	free(thrs);
out_close_sock:
	close(clnt.sock);

	return ret;
}

static int prepare_for_listening(void)
{
	struct addrinfo *result, *rp, hints;
	int sock = -1;
	int ret;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE; /* For wildcard IP address */

	ret = getaddrinfo(NULL, port, &hints, &result);
	if (ret != 0) {
		logerr("getaddrinfo: %s\n", gai_strerror(ret));
		return -1;
	}

	/* getaddrinfo() returns a list of address structures.
	   Try each address until we successfully bind(2).
	   If socket(2) (or bind(2)) fails, we (close the socket
	   and) try the next address. */

	for (rp = result; rp != NULL; rp = rp->ai_next) {
		int flag = 1;

		sock = socket(rp->ai_family, rp->ai_socktype,
				rp->ai_protocol);
		if (sock < 0) {
			logerr("Could not create socket: %s\n", strerror(errno));
			continue;
		}

		if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&flag, sizeof(int)) < 0) {
			logerr("setsockopt(SO_REUSEADDR) failed: %s\n", strerror(errno));
			close(sock);
			sock = -1;
			continue;
		}

		if (bind(sock, rp->ai_addr, rp->ai_addrlen) < 0) {
			logerr("bind() failed: %s\n", strerror(errno));
			close(sock);
			sock = -1;
			continue;
		}

		logerr("Listening on port %s\n", port);
		break; /* Success */
	}

	if (rp == NULL) /* No address succeeded */
		logerr("Could not bind\n");

	freeaddrinfo(result); /* No longer needed */

	return sock;
}

static int set_sock_keepalive(int sock)
{
	int val = 1;

	/* enable TCP keepalives on socket */
	if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &val,
				sizeof(val)) < 0) {
		logerr("setsockopt() failed: %s\n", strerror(errno));
		return -1;
	}
	/* set idle timeout to 1 second */
	if (setsockopt(sock, SOL_TCP, TCP_KEEPIDLE, &val,
				sizeof(val)) < 0) {
		logerr("setsockopt() failed: %s\n", strerror(errno));
		return -1;
	}
	/* set consecutive interval to 1 second */
	if (setsockopt(sock, SOL_TCP, TCP_KEEPINTVL, &val,
				sizeof(val)) < 0) {
		logerr("setsockopt() failed: %s\n", strerror(errno));
		return -1;
	}
	if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&val, sizeof(int)) < 0) {
		logerr("setsockopt(TCP_NODELAY) failed: %s\n", strerror(errno));
		return -1;
	}

	/* set number of keepalives before dropping to 3 */
	val = 3;
	if (setsockopt(sock, SOL_TCP, TCP_KEEPCNT, &val,
				sizeof(val)) < 0) {
		logerr("setsockopt() failed: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int run_server(void)
{
	int sock;
	struct sockaddr_storage peer_addr;
	socklen_t peer_addr_len;
	char boundaddr[NI_MAXHOST] = "";
	ssize_t nread;
	struct cache_state *cache;
	struct report rp;
	int ret = 0;

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	sock = prepare_for_listening();
	if (sock < 0)
		return -1;

	if (listen(sock, 5) < 0) {
		logerr("listen() failed: %s\n", strerror(errno));
		ret = -1;
		goto out_close_sock;
	}

	cache = calloc(THREADS_MAX, sizeof(*cache));
	if (!cache) {
		logerr("calloc() failed\n");
		ret = -1;
		goto out_close_sock;
	}

	while (!exit_flag) {
		char claddr[NI_MAXHOST];
		int conn;

		peer_addr_len = sizeof(struct sockaddr_storage);
		conn = accept(sock, (struct sockaddr *) &peer_addr, &peer_addr_len);
		if (conn < 0) {
			logerr("accept() failed: %s\n", strerror(errno));
			ret = -1;
			break;
		}

		ret = set_sock_keepalive(conn);
		if (ret < 0) {
			close(conn);
			break;
		}

		ret = getnameinfo((struct sockaddr *) &peer_addr,
				peer_addr_len, claddr, NI_MAXHOST,
				NULL, 0, NI_NUMERICHOST);
		if (ret < 0) {
			logerr("getnameinfo() failed: %s\n", gai_strerror(ret));
			close(conn);
			break;
		}

		if (boundaddr[0] == 0) {
			strncpy(boundaddr, claddr, NI_MAXHOST-1);
			logerr("Accepting messages from %s\n", boundaddr);
		} else {
			if (strncmp(boundaddr, claddr, NI_MAXHOST) != 0) {
				logerr("Skip connection from invalid address %s\n", claddr);
				close(conn);
				continue;
			}
			logerr("Restarted connection from %s\n", boundaddr);
		}

		while (!exit_flag) {
			uint64_t expected;
			int new_error = 0;

			nread = sread(conn, &rp, sizeof(rp));
			if (nread < 0) {
				logerr("read() failed: %s\n", strerror(-nread));
				break;
			}
			if (nread == 0)
				break;
			if (nread != sizeof(rp)) {
				logerr("Failed to read counter\n");
				break;
			}

			if (rp.id >= THREADS_MAX) {
				logerr("Bad id received: %u\n", rp.id);
				break;
			}
			if (rp.flags & REP_FL_UPDATE)
				expected = cache[rp.id].gen + 1;
			else /* simple check */
				expected = cache[rp.id].gen;

			if (rp.gen < expected) {
				logout("id %u: %llu > %llu, cache error detected!\n",
				       rp.id, (unsigned long long)expected,
				       (unsigned long long)rp.gen);
				new_error = 1;
				cache[rp.id].error = 1;
				ret = 1;
			} else if (rp.gen > expected)
				logerr("id %u: %llu -> %llu, probably missed some packets\n",
						rp.id, (unsigned long long)cache[rp.id].gen,
						(unsigned long long)rp.gen);

			if (rp.flags & REP_FL_ERR) {
				logerr("id %u: transaction id:%llu failed on client (gen %llu)\n",
					rp.id, (unsigned long long)cache[rp.id].gen,
					(unsigned long long)rp.gen);
				new_error = 1;
				cache[rp.id].error = 1;
				ret = 1;
			}
			if (rp.flags & REP_FL_UPDATE && !new_error)
				cache[rp.id].gen = rp.gen;
			if (rp.flags & REP_FL_STOP)
				exit_flag = 1;

			if (rp.flags & REP_FL_SYNC) {
				struct report reply;
				int r;
				reply.flags = REP_FL_ACK;
				reply.gen = expected;
				reply.id = rp.id;
				if (new_error)
					reply.flags |= REP_FL_ERR;
				r = swrite(conn, &reply, sizeof(reply));
				if (r < 0 && !ret)
					ret = r;
			}
		}
		close(conn);
		logerr("Connection closed\n");
	}
	free(cache);
out_close_sock:
	close(sock);
	return ret;
}

#define EXT4_IOC_MFSYNC                 _IO('f', 43)
struct ext4_ioc_mfsync_info {
        uint32_t size;
        uint32_t fd[0];
};



static int do_mfsync_ioc(int *fd, int *datasync, int nr)
{
	struct ext4_ioc_mfsync_info *mfsync_ioc;
	int i;
	size_t msize;
	msize = sizeof(*mfsync_ioc) + sizeof(mfsync_ioc->fd[0]) * nr;

	mfsync_ioc = malloc(msize);
	memset(mfsync_ioc, 0, msize);

	for (i = 0; i < nr; i++) {
		mfsync_ioc->fd[i] = fd[i];
		if (datasync[i])
			mfsync_ioc->fd[i] |= (1 << 31);
	}
	mfsync_ioc->size = nr;
	return ioctl(fd[0], EXT4_IOC_MFSYNC, mfsync_ioc);
}


static const char *progname(const char *prog)
{
	char *s = strrchr(prog, '/');
	return s ? s+1 : prog;
}

static void usage(const char *prog)
{
	fprintf(stderr, "Flush test tool.\n");
	fprintf(stderr, "Usage: %s [options...]\n", progname(prog));
	fprintf(stderr, "Options:\n"
			"  -l, --listen          Run as a server.\n"
			"  -c, --check           Check data\n"
			"  -x, --exit            Send exit signal to server\n"
			"  -P, --prepare         Perform only preparation stage\n"
			"  -s, --server=IP       Set server host name or IP address\n"
			"  -p, --port=PORT       Set server port\n"
			"  -d, --dir=DIR         Set test directory\n"
			"  -D, --fdatasync       Use fdatasync(2) \n"
			"  -F, --fsync           Use fsync(2) \n"
			"  -S, --sync            Use sync(2) \n"
			"  -M, --mfsync          Use mfsync \n"
			"  -m, --mfdatasync      Use mfdatasync \n"
			"  -b, --blocksize=SIZE  Set block size\n"
			"  -n, --blocksmax=NUM   Set maximum number of blocks\n"
			"  -t, --threads=NUM     Set number of client threads to use\n"
			"  -O, --direct          Open files with O_DIRECT"
			"  -a, --alloc_type=NUM  Set prealloc type 0:NONE, 1:posix_falloc, 2:write\n"
			"  -h, --help            Show usage information\n"
	       );

	exit(-1);
}

static const struct option long_opts[] = {
	{"listen",	0, 0, 'l'},
	{"check",	0, 0, 'c'},
	{"prepare",	0, 0, 'P'},
	{"exit",	0, 0, 'x'},
	{"server",	1, 0, 's'},
	{"port",	1, 0, 'p'},
	{"dir",		1, 0, 'd'},
	{"blocksize",	1, 0, 'b'},
	{"blocksmax",	1, 0, 'n'},
	{"threads",	1, 0, 't'},
	{"alloc_type",	1, 0, 'a'},
	{"direct",	0, 0, 'O'},
	{"help",	0, 0, 'h'},
	{"fdatasync",	0, 0, 'D'},
	{"fsync",	0, 0, 'F'},
	{"sync",	0, 0, 'S'},
	{"mfsync",	0, 0, 'M'},
	{"mfdatasync",	0, 0, 'm'},
	{0, 0, 0, 0}
};

int main(int argc, char *argv[])
{
	int ch;

	/* process options, stop at first nonoption */
	while ((ch = getopt_long(argc, argv, "DFMPSclms:p:d:a:b:f:n:t:h", long_opts, NULL)) != -1) {
		switch (ch) {
		case 'l':
			is_server = 1;
			break;
		case 'x':
			send_exit = 1;
			break;
		case 'c':
			is_check_stage = 1;
			break;
		case 'P':
			is_prepare = 1;
			break;
		case 'O':
			use_dio = 1;
			break;

		case 's':
			host = optarg;
			break;
		case 'p':
			port = optarg;
			break;
		case 'd':
			dir = optarg;
			break;
		case 'a':
			alloc_type = atoi(optarg);
			if (alloc_type > PA_LAST) {
				fprintf(stderr, "Invalid prealloc type\n");
				usage(argv[0]);
			}
			break;

		case 'M':
			sync_type = SYNC_MFSYNC;
			break;
		case 'm':
			sync_type = SYNC_MFDATASYNC;
			break;
		case 'S':
			sync_type = SYNC_SYNC;
			break;
		case 'F':
			sync_type = SYNC_FSYNC;
			break;
		case 'D':
			sync_type = SYNC_FDATASYNC;
			break;

		case 'b': {
			char *p;
			blocksize = strtoull(optarg, &p, 10);
			if (p[0] != '\0') {
				fprintf(stderr, "Invalid block size\n");
				usage(argv[0]);
			}
			blocksize &= ~7LL;
			break;
		}
		case 'n': {
			char *p;
			blocksmax = strtoull(optarg, &p, 10);
			if (p[0] != '\0') {
				fprintf(stderr, "Invalid maximum number of blocks\n");
				usage(argv[0]);
			}
			break;
		}
		case 't': {
			char *p;
			threads = strtoul(optarg, &p, 10);
			if (p[0] != '\0') {
				fprintf(stderr, "Invalid number of threads\n");
				usage(argv[0]);
			}
			if (threads > THREADS_MAX) {
				fprintf(stderr, "Number of threads is too big\n");
				usage(argv[0]);
			}
			break;
		}
		default:
			usage(argv[0]);
			return 1;
		}
	}

	setlinebuf(stdout);

	if (!is_server) {
		if (host == NULL) {
			fprintf(stderr, "Please specify server address\n");
			usage(argv[0]);
		}
		if (dir == NULL) {
			fprintf(stderr, "Please specify test directory\n");
			usage(argv[0]);
		}
		return run_client();
	} else
		return run_server();
}
