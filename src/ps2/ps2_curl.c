// PS2-130: HTTP/1.1 client (GET/POST) behind the libcurl-easy subset of ps2_curl.h. See there for what is and is not supported.
// The engine part (PS2Http_Request) talks BSD sockets only; PS2Net_Up() brings the IP stack up on the PS2, tools/ps2/ps2_http_hosttest.c
// builds it for Windows (Winsock) against a local Python server.

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
typedef int socklen_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#define strncasecmp _strnicmp
#else
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
#endif

#ifdef _EE
#include <kernel.h>
#include <timer.h>
#include "../doomtype.h"
#include "ps2_net.h"
#endif

#include "ps2_curl.h"

#define MAXBODY (512 * 1024)   // the largest response kept (a master server list is a few KB)
#define MAXHEAD 8192
#define DEFAULT_TIMEOUT 30

struct ps2_curl
{
	char url[1024];
	char *postfields;
	long postsize;
	int post;
	long timeout;
	long follow, maxredirs;
	long ipresolve;
	char useragent[256];
	char *errbuf;
	ps2curl_write_fn write_fn;
	void *write_data;
	long status;
};

typedef struct
{
	char *p;
	size_t n, cap;
} buf_t;

static long NowMs(void)
{
#if defined (_EE)
	return (long)(GetTimerSystemTime() / (kBUSCLK / 1000));
#elif defined (_WIN32)
	return (long)GetTickCount64();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

#ifdef _EE
// PS2-NET-3 (OPT12): lwIP's select() with a time-out waits on the SDK's alarm library (WaitSemaEx), the one whose lost wake-ups hung the game thread (docs/GATES/g1/opt12-NET.md).
// The game thread polls instead: select() with a zero time-out every millisecond (PS2_SleepUs) until the time is up.
#include "ps2_sys.h"
static int SelectWait(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv)
{
	const long end = NowMs() + (tv ? (long)tv->tv_sec * 1000 + tv->tv_usec / 1000 : 0);

	for (;;)
	{
		fd_set r1, w1, e1;
		struct timeval zero = {0, 0};
		int rc;

		if (r) r1 = *r;
		if (w) w1 = *w;
		if (e) e1 = *e;
		rc = select(nfds, r ? &r1 : NULL, w ? &w1 : NULL, e ? &e1 : NULL, &zero);
		if (rc != 0 || NowMs() >= end)
		{
			if (rc > 0)
			{
				if (r) *r = r1;
				if (w) *w = w1;
				if (e) *e = e1;
			}
			return rc;
		}
		PS2_SleepUs(1000);
	}
}
#else
#define SelectWait select
#endif

#ifdef __GNUC__
static void Fail(char *errbuf, size_t errsize, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#endif

static void Fail(char *errbuf, size_t errsize, const char *fmt, ...)
{
	va_list ap;

	if (!errbuf || !errsize)
		return;
	va_start(ap, fmt);
	vsnprintf(errbuf, errsize, fmt, ap);
	va_end(ap);
}

static int Append(buf_t *b, const char *data, size_t n)
{
	if (b->n + n + 1 > b->cap)
	{
		size_t cap = b->cap ? b->cap : 2048;
		char *np;

		while (cap < b->n + n + 1)
			cap *= 2;
		if (cap > MAXBODY + MAXHEAD + 1024)
			return 0;
		np = realloc(b->p, cap);
		if (!np)
			return 0;
		b->p = np;
		b->cap = cap;
	}
	memcpy(b->p + b->n, data, n);
	b->n += n;
	b->p[b->n] = '\0';
	return 1;
}

typedef struct
{
	int https;
	char host[256];
	char port[8];
	char path[1536];
} url_t;

static int ParseUrl(const char *url, url_t *u)
{
	const char *s = url, *hs, *he, *ps;
	size_t n;

	memset(u, 0, sizeof *u);
	if (!strncasecmp(s, "http://", 7))
		s += 7;
	else if (!strncasecmp(s, "https://", 8))
	{
		u->https = 1;
		s += 8;
	}
	else
		return CURLE_UNSUPPORTED_PROTOCOL;
	hs = s;
	he = hs + strcspn(hs, ":/?#");
	n = (size_t)(he - hs);
	if (!n || n >= sizeof u->host)
		return CURLE_URL_MALFORMAT;
	memcpy(u->host, hs, n);
	snprintf(u->port, sizeof u->port, "%s", u->https ? "443" : "80");
	if (*he == ':')
	{
		ps = he + 1;
		he = ps + strcspn(ps, "/?#");
		n = (size_t)(he - ps);
		if (!n || n >= sizeof u->port)
			return CURLE_URL_MALFORMAT;
		memcpy(u->port, ps, n);
		u->port[n] = '\0';
	}
	if (*he == '\0' || *he == '#')
		strcpy(u->path, "/");
	else
	{
		n = strcspn(he, "#");
		if (n >= sizeof u->path)
			return CURLE_URL_MALFORMAT;
		if (*he == '?')
		{
			u->path[0] = '/';
			memcpy(u->path + 1, he, n);
			u->path[n + 1] = '\0';
		}
		else
		{
			memcpy(u->path, he, n);
			u->path[n] = '\0';
		}
	}
	return CURLE_OK;
}

// Opens the socket and starts the connection: 0 ok (*done = 1 when connected already, else poll with ConnectPoll), else a CURLcode.
static int ConnectBegin(const url_t *u, sock_t *out, int *done, char *errbuf, size_t errsize)
{
	struct sockaddr_in sa;
	sock_t s;
	int port = atoi(u->port);
	unsigned long ip;
	int rc;

	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons((unsigned short)port);
	ip = inet_addr(u->host);
	if (ip != INADDR_NONE)
		sa.sin_addr.s_addr = ip;
	else
	{
		struct hostent *he = gethostbyname(u->host);

		if (!he || he->h_addrtype != AF_INET || !he->h_addr_list || !he->h_addr_list[0])
		{
			Fail(errbuf, errsize, "Could not resolve host: %s", u->host);
			return CURLE_COULDNT_RESOLVE_HOST;
		}
		memcpy(&sa.sin_addr, he->h_addr_list[0], 4);
	}
	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s == SOCK_BAD)
	{
		Fail(errbuf, errsize, "socket() failed (errno %d)", errno);
		return CURLE_COULDNT_CONNECT;
	}
#ifdef _WIN32
	{
		u_long nb = 1;
		ioctlsocket(s, FIONBIO, &nb);
	}
#else
	{
		int fl = fcntl(s, F_GETFL, 0);
		fcntl(s, F_SETFL, fl | O_NONBLOCK);
	}
#endif
	*done = 0;
	rc = connect(s, (struct sockaddr *)&sa, sizeof sa);
	if (rc != 0)
	{
#ifdef _WIN32
		const int e = WSAGetLastError();
		const int inprogress = e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
		const int e = errno;
		const int inprogress = e == EINPROGRESS || e == EWOULDBLOCK || e == EALREADY;
#endif
		if (!inprogress)
		{
			Fail(errbuf, errsize, "Failed to connect to %s (error %d)", u->host, e);
			sock_close(s);
			return CURLE_COULDNT_CONNECT;
		}
	}
	else
		*done = 1;
	*out = s;
	return CURLE_OK;
}

// Waits up to waitms for the connection started by ConnectBegin: 0 ok (*done says whether it is up), else a CURLcode (the socket is closed).
static int ConnectPoll(sock_t s, const char *host, long waitms, int *done, char *errbuf, size_t errsize)
{
	fd_set wfds, efds;
	struct timeval tv;
	int rc;

	FD_ZERO(&wfds);
	FD_SET(s, &wfds);
	FD_ZERO(&efds);
	FD_SET(s, &efds); // Winsock reports a refused connection here
	tv.tv_sec = waitms / 1000;
	tv.tv_usec = (waitms % 1000) * 1000;
	rc = SelectWait((int)s + 1, NULL, &wfds, &efds, &tv);
	*done = 0;
	if (rc > 0)
	{
		int err = 0;
		socklen_t len = sizeof err;

		getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
		if (err)
		{
			Fail(errbuf, errsize, "Failed to connect to %s (error %d)", host, err);
			sock_close(s);
			return CURLE_COULDNT_CONNECT;
		}
		*done = 1;
	}
	return CURLE_OK;
}

// 0 ok, else a CURLcode. Waits for the connection for at most the time left (the stack's own connect timeout is far longer).
static int Connect(const url_t *u, long deadline, sock_t *out, char *errbuf, size_t errsize)
{
	sock_t s;
	int rc, done;

	rc = ConnectBegin(u, &s, &done, errbuf, errsize);
	if (rc)
		return rc;
	while (!done)
	{
		const long left = deadline - NowMs();

		if (left <= 0)
		{
			Fail(errbuf, errsize, "Connection timed out connecting to %s", u->host);
			sock_close(s);
			return CURLE_OPERATION_TIMEDOUT;
		}
		rc = ConnectPoll(s, u->host, left > 500 ? 500 : left, &done, errbuf, errsize);
		if (rc)
			return rc;
	}
	*out = s;
	return CURLE_OK;
}

static int SendAll(sock_t s, const char *p, size_t n, long deadline)
{
	while (n)
	{
		fd_set wfds;
		struct timeval tv;
		long left = deadline - NowMs();
		int k;

		if (left <= 0)
			return CURLE_OPERATION_TIMEDOUT;
		FD_ZERO(&wfds);
		FD_SET(s, &wfds);
		tv.tv_sec = (left > 500 ? 500 : left) / 1000;
		tv.tv_usec = ((left > 500 ? 500 : left) % 1000) * 1000;
		if (SelectWait((int)s + 1, NULL, &wfds, NULL, &tv) <= 0)
			continue;
		k = (int)send(s, p, (int)(n > 1400 ? 1400 : n), 0);
		if (k <= 0)
			return CURLE_SEND_ERROR;
		p += k;
		n -= (size_t)k;
	}
	return CURLE_OK;
}

static char *FindHeaderEnd(buf_t *b)
{
	return b->p && b->n >= 4 ? strstr(b->p, "\r\n\r\n") : NULL;
}

// Header value of "name:" in the head block (case-insensitive); NULL if absent. Not NUL-terminated at the value's end: *len.
static const char *Header(const char *head, const char *name, size_t *len)
{
	const size_t nl = strlen(name);
	const char *p = head;

	while ((p = strstr(p, "\r\n")) != NULL)
	{
		p += 2;
		if (!strncasecmp(p, name, nl) && p[nl] == ':')
		{
			const char *v = p + nl + 1, *e;

			while (*v == ' ' || *v == '\t')
				v++;
			e = strstr(v, "\r\n");
			*len = e ? (size_t)(e - v) : strlen(v);
			return v;
		}
	}
	return NULL;
}

// Decodes a chunked body in place into out; 1 when the final chunk was seen, 0 when more data is needed, -1 on a malformed stream
static int Dechunk(const char *in, size_t n, buf_t *out)
{
	size_t i = 0;

	out->n = 0;
	for (;;)
	{
		unsigned long sz = 0;
		size_t j = i;
		int digits = 0;

		while (j < n && isxdigit((unsigned char)in[j]))
		{
			sz = sz * 16 + (unsigned long)(isdigit((unsigned char)in[j]) ? in[j] - '0' : (tolower((unsigned char)in[j]) - 'a' + 10));
			j++;
			digits++;
			if (sz > MAXBODY)
				return -1;
		}
		if (!digits)
			return j >= n ? 0 : -1;
		while (j < n && in[j] != '\n')
			j++;
		if (j >= n)
			return 0;
		j++;
		if (sz == 0)
			return 1; // the trailer is not needed
		if (j + sz + 2 > n)
			return 0;
		if (!Append(out, in + j, sz))
			return -1;
		i = j + sz + 2;
	}
}

// One request on a new connection. Result in *head (status line + headers, up to the blank line) and *body (decoded).
static int Exchange(const url_t *u, int is_post, const char *post, long postsize, const char *useragent, long deadline,
	buf_t *head, buf_t *body, int *status, char *errbuf, size_t errsize)
{
	sock_t s = SOCK_BAD;
	char req[2048];
	int rc, len;
	buf_t raw = {0};
	char *he;
	size_t hl;
	int chunked = 0;
	long clen = -1;

	rc = Connect(u, deadline, &s, errbuf, errsize);
	if (rc)
		return rc;
	len = snprintf(req, sizeof req, "%s %s HTTP/1.1\r\nHost: %s%s%s\r\nUser-Agent: %s\r\nAccept: */*\r\nConnection: close\r\n",
		is_post ? "POST" : "GET", u->path, u->host,
		((u->https ? strcmp(u->port, "443") : strcmp(u->port, "80")) ? ":" : ""),
		((u->https ? strcmp(u->port, "443") : strcmp(u->port, "80")) ? u->port : ""),
		useragent && useragent[0] ? useragent : "SRB2");
	if (is_post)
		len += snprintf(req + len, sizeof req - (size_t)len, "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %ld\r\n", postsize);
	len += snprintf(req + len, sizeof req - (size_t)len, "\r\n");
	if (len <= 0 || (size_t)len >= sizeof req)
	{
		sock_close(s);
		return CURLE_URL_MALFORMAT;
	}
	rc = SendAll(s, req, (size_t)len, deadline);
	if (!rc && is_post && postsize > 0)
		rc = SendAll(s, post, (size_t)postsize, deadline);
	if (rc)
	{
		if (rc == CURLE_OPERATION_TIMEDOUT)
			Fail(errbuf, errsize, "Operation timed out sending the request");
		else
			Fail(errbuf, errsize, "Failure when sending the request");
		sock_close(s);
		return rc;
	}

	// read until the message is complete (Content-Length / last chunk) or the server closes the connection
	he = NULL;
	for (;;)
	{
		fd_set rfds;
		struct timeval tv;
		long left = deadline - NowMs();
		char tmp[1400];
		int k;

		if (he)
		{
			const size_t have = raw.n - (size_t)(he + 4 - raw.p);

			if (chunked)
			{
				buf_t t = {0};
				int d = Dechunk(he + 4, have, &t);

				free(t.p);
				if (d != 0)
					break; // complete or malformed (handled below)
			}
			else if (clen >= 0 && have >= (size_t)clen)
				break;
		}
		if (left <= 0)
		{
			Fail(errbuf, errsize, "Operation timed out with %s bytes received", he ? "some" : "no");
			free(raw.p);
			sock_close(s);
			return CURLE_OPERATION_TIMEDOUT;
		}
		FD_ZERO(&rfds);
		FD_SET(s, &rfds);
		tv.tv_sec = (left > 500 ? 500 : left) / 1000;
		tv.tv_usec = ((left > 500 ? 500 : left) % 1000) * 1000;
		if (SelectWait((int)s + 1, &rfds, NULL, NULL, &tv) <= 0)
			continue;
		k = (int)recv(s, tmp, sizeof tmp, 0);
		if (k < 0)
		{
			Fail(errbuf, errsize, "Failure when receiving data from the peer");
			free(raw.p);
			sock_close(s);
			return CURLE_RECV_ERROR;
		}
		if (k == 0)
			break; // closed: the body ends here
		if (!Append(&raw, tmp, (size_t)k) || raw.n > MAXBODY + MAXHEAD)
		{
			free(raw.p);
			sock_close(s);
			return CURLE_OUT_OF_MEMORY;
		}
		if (!he)
		{
			he = FindHeaderEnd(&raw);
			if (he)
			{
				size_t vl;
				const char *v;

				he[2] = '\0'; // terminates the last header line for Header(); restored below
				v = Header(raw.p, "Transfer-Encoding", &vl);
				chunked = v && vl >= 7 && !strncasecmp(v, "chunked", 7);
				v = Header(raw.p, "Content-Length", &vl);
				clen = v ? atol(v) : -1;
				he[2] = '\r';
			}
		}
	}
	sock_close(s);

	he = FindHeaderEnd(&raw);
	if (!he)
	{
		Fail(errbuf, errsize, "Empty or malformed reply from the server");
		free(raw.p);
		return CURLE_RECV_ERROR;
	}
	hl = (size_t)(he - raw.p);
	if (strncmp(raw.p, "HTTP/1.", 7) != 0 || hl < 12)
	{
		Fail(errbuf, errsize, "Received HTTP/0.9 or garbage when HTTP/1.x was expected");
		free(raw.p);
		return CURLE_RECV_ERROR;
	}
	*status = atoi(raw.p + 9);
	head->n = 0;
	if (!Append(head, raw.p, hl + 2))
	{
		free(raw.p);
		return CURLE_OUT_OF_MEMORY;
	}
	head->p[hl + 2] = '\0';
	{
		size_t vl;
		const char *v = Header(head->p, "Transfer-Encoding", &vl);

		chunked = v && vl >= 7 && !strncasecmp(v, "chunked", 7);
		v = Header(head->p, "Content-Length", &vl);
		clen = v ? atol(v) : -1;
	}
	body->n = 0;
	{
		const char *bp = he + 4;
		size_t bn = raw.n - (size_t)(bp - raw.p);

		if (chunked)
		{
			if (Dechunk(bp, bn, body) < 0)
			{
				Fail(errbuf, errsize, "Malformed chunked encoding from the server");
				free(raw.p);
				return CURLE_RECV_ERROR;
			}
		}
		else
		{
			if (clen >= 0 && (size_t)clen < bn)
				bn = (size_t)clen;
			if (bn && !Append(body, bp, bn))
			{
				free(raw.p);
				return CURLE_OUT_OF_MEMORY;
			}
		}
	}
	free(raw.p);
	if (!body->p)
		Append(body, "", 0);
	return CURLE_OK;
}

CURLcode PS2Http_Request(const char *url, const char *post, long postsize, int is_post, long timeout, int maxredirs, int follow,
	const char *useragent, ps2curl_write_fn write_fn, void *userdata, long *status, char *errbuf, size_t errsize)
{
	url_t u;
	char cur[1536];
	const long deadline = NowMs() + (timeout > 0 ? timeout : DEFAULT_TIMEOUT) * 1000;
	int redirs = 0, rc, st = 0;

	if (errbuf && errsize)
		errbuf[0] = '\0';
	snprintf(cur, sizeof cur, "%s", url);
	for (;;)
	{
		buf_t head = {0}, body = {0};

		rc = ParseUrl(cur, &u);
		if (rc)
		{
			if (rc == CURLE_UNSUPPORTED_PROTOCOL)
				Fail(errbuf, errsize, "Protocol of \"%.60s\" not supported (only http://)", cur);
			else
				Fail(errbuf, errsize, "URL using bad/illegal format or missing URL");
			return (CURLcode)rc;
		}
		if (u.https)
		{
			Fail(errbuf, errsize, "Protocol \"https\" not supported (the PS2 build has no TLS: use an http:// master server)");
			return CURLE_UNSUPPORTED_PROTOCOL;
		}
		rc = Exchange(&u, is_post, post, postsize, useragent, deadline, &head, &body, &st, errbuf, errsize);
		if (rc)
		{
			free(head.p);
			free(body.p);
			return (CURLcode)rc;
		}
		if (follow && (st == 301 || st == 302 || st == 303 || st == 307 || st == 308))
		{
			size_t vl;
			const char *v = Header(head.p, "Location", &vl);

			if (v && vl)
			{
				char loc[1536];

				if (redirs >= maxredirs)
				{
					Fail(errbuf, errsize, "Maximum (%d) redirects followed", (int)maxredirs);
					free(head.p);
					free(body.p);
					return CURLE_TOO_MANY_REDIRECTS;
				}
				if (vl >= sizeof loc)
					vl = sizeof loc - 1;
				memcpy(loc, v, vl);
				loc[vl] = '\0';
				if (!strncasecmp(loc, "http://", 7) || !strncasecmp(loc, "https://", 8))
					snprintf(cur, sizeof cur, "%s", loc);
				else if (loc[0] == '/')
					snprintf(cur, sizeof cur, "http://%s:%s%s", u.host, u.port, loc);
				else
				{
					char *slash = strrchr(u.path, '/');
					const size_t dir = slash ? (size_t)(slash - u.path) + 1 : 0;

					snprintf(cur, sizeof cur, "http://%s:%s%.*s%s", u.host, u.port, (int)dir, u.path, loc);
				}
				// 301/302/303 turn a POST into a GET (as curl does); 307/308 keep it
				if (st != 307 && st != 308)
					is_post = 0;
				free(head.p);
				free(body.p);
				redirs++;
				continue;
			}
		}
		*status = st;
		if (write_fn && body.n)
		{
			size_t off = 0;

			while (off < body.n)
			{
				const size_t n = body.n - off > 4096 ? 4096 : body.n - off;

				if (write_fn(body.p + off, 1, n, userdata) != n)
				{
					Fail(errbuf, errsize, "Failure writing output to destination");
					free(head.p);
					free(body.p);
					return CURLE_WRITE_ERROR;
				}
				off += n;
			}
		}
		free(head.p);
		free(body.p);
		return CURLE_OK;
	}
}

// ===== PS2-137: streaming GET, stepped from the game loop (add-on download from the server's HTTP source) =====
// PS2Http_Request keeps the whole body (512 KB at most) and blocks. An add-on is megabytes and the connection screen must go on drawing and
// polling the pad, so this one is a state machine: every PS2HttpGet_Step() does what the sockets allow right now (select with a zero timeout,
// as the game's UDP code does under lwIP) and returns; the body goes through write_fn in pieces as it arrives.

enum { G_START, G_CONNECT, G_SEND, G_HEAD, G_BODY, G_DONE };
enum { CS_SIZE, CS_DATA, CS_CRLF };

struct ps2_httpget
{
	int state;
	sock_t s;
	url_t u;
	char cur[1536];
	char useragent[256];
	int redirs, maxredirs;
	long stall_ms, last_progress;
	char req[1536];
	int reqlen, reqsent;
	char head[MAXHEAD];
	int headlen;
	long status, clen, got;
	int chunked, cs, sizelen;
	long chunkleft;
	char sizeline[24];
};

ps2_httpget_t *PS2HttpGet_Open(const char *url, long stall_seconds, int maxredirs, const char *useragent)
{
	ps2_httpget_t *g = calloc(1, sizeof *g);

	if (!g)
		return NULL;
	g->s = SOCK_BAD;
	snprintf(g->cur, sizeof g->cur, "%s", url);
	snprintf(g->useragent, sizeof g->useragent, "%s", useragent && useragent[0] ? useragent : "SRB2");
	g->maxredirs = maxredirs;
	g->stall_ms = (stall_seconds > 0 ? stall_seconds : 20) * 1000;
	g->last_progress = NowMs();
	g->clen = -1;
	return g;
}

void PS2HttpGet_Close(ps2_httpget_t *g)
{
	if (!g)
		return;
	if (g->s != SOCK_BAD)
		sock_close(g->s);
	free(g);
}

static int GetFail(ps2_httpget_t *g, int code, char *errbuf, size_t errsize, const char *msg)
{
	if (g->s != SOCK_BAD)
	{
		sock_close(g->s);
		g->s = SOCK_BAD;
	}
	g->state = G_DONE;
	if (msg)
		Fail(errbuf, errsize, "%s", msg);
	return -code;
}

// Delivers a piece of the message body. 0 = go on, 1 = the body is complete, < 0 = -CURLcode
static int GetFeed(ps2_httpget_t *g, const char *p, size_t n, ps2curl_write_fn fn, void *ud)
{
	if (!g->chunked)
	{
		if (g->clen >= 0 && (long)n > g->clen - g->got)
			n = (size_t)(g->clen - g->got);
		if (n && fn && fn((char *)p, 1, n, ud) != n)
			return -CURLE_WRITE_ERROR;
		g->got += (long)n;
		return g->clen >= 0 && g->got >= g->clen ? 1 : 0;
	}
	while (n)
	{
		if (g->cs == CS_SIZE)
		{
			const char c = *p++;

			n--;
			if (c == '\n')
			{
				g->sizeline[g->sizelen] = '\0';
				g->chunkleft = strtol(g->sizeline, NULL, 16); // a ";extension" ends the number by itself
				g->sizelen = 0;
				if (g->chunkleft < 0)
					return -CURLE_RECV_ERROR;
				if (!g->chunkleft)
					return 1; // the last chunk: the trailer is not needed
				g->cs = CS_DATA;
			}
			else if (c != '\r' && g->sizelen < (int)sizeof g->sizeline - 1)
				g->sizeline[g->sizelen++] = c;
		}
		else if (g->cs == CS_DATA)
		{
			const size_t take = (long)n > g->chunkleft ? (size_t)g->chunkleft : n;

			if (fn && fn((char *)p, 1, take, ud) != take)
				return -CURLE_WRITE_ERROR;
			g->got += (long)take;
			g->chunkleft -= (long)take;
			p += take;
			n -= take;
			if (!g->chunkleft)
				g->cs = CS_CRLF;
		}
		else // CS_CRLF: the line end after the chunk data
		{
			if (*p++ == '\n')
				g->cs = CS_SIZE;
			n--;
		}
	}
	return 0;
}

// The reply header is complete in g->head (g->headlen bytes, "\r\n\r\n" at he). 0 = go on to the body, 1 = redirected (state G_START again), < 0 = -CURLcode
static int GetHeader(ps2_httpget_t *g, char *he, char *errbuf, size_t errsize)
{
	size_t vl;
	const char *v;

	if (strncmp(g->head, "HTTP/1.", 7) != 0 || g->headlen < 12)
		return GetFail(g, CURLE_RECV_ERROR, errbuf, errsize, "Received HTTP/0.9 or garbage when HTTP/1.x was expected");
	g->status = atoi(g->head + 9);
	he[2] = '\0'; // terminates the last header line for Header()
	v = Header(g->head, "Transfer-Encoding", &vl);
	g->chunked = v && vl >= 7 && !strncasecmp(v, "chunked", 7);
	v = Header(g->head, "Content-Length", &vl);
	g->clen = v ? atol(v) : -1;
	if (g->status >= 300 && g->status < 400 && g->status != 304)
	{
		v = Header(g->head, "Location", &vl);
		if (v && vl && vl < 1400)
		{
			char loc[1536];

			if (g->redirs >= g->maxredirs)
				return GetFail(g, CURLE_TOO_MANY_REDIRECTS, errbuf, errsize, "Maximum redirects followed");
			memcpy(loc, v, vl);
			loc[vl] = '\0';
			if (!strncasecmp(loc, "http://", 7) || !strncasecmp(loc, "https://", 8))
				snprintf(g->cur, sizeof g->cur, "%s", loc);
			else if (loc[0] == '/')
				snprintf(g->cur, sizeof g->cur, "http://%s:%s%s", g->u.host, g->u.port, loc);
			else
			{
				char *slash = strrchr(g->u.path, '/');
				const size_t dir = slash ? (size_t)(slash - g->u.path) + 1 : 0;

				snprintf(g->cur, sizeof g->cur, "http://%s:%s%.*s%s", g->u.host, g->u.port, (int)dir, g->u.path, loc);
			}
			sock_close(g->s);
			g->s = SOCK_BAD;
			g->redirs++;
			g->state = G_START;
			return 1;
		}
	}
	if (g->status < 200 || g->status >= 300)
	{
		char msg[64];

		snprintf(msg, sizeof msg, "The requested URL returned error: %ld", g->status);
		return GetFail(g, CURLE_HTTP_RETURNED_ERROR, errbuf, errsize, msg);
	}
	return 0;
}

int PS2HttpGet_Step(ps2_httpget_t *g, ps2curl_write_fn fn, void *ud, long *status, long *total, long *got, char *errbuf, size_t errsize)
{
	int round, rc;

	if (errbuf && errsize)
		errbuf[0] = '\0';
	for (round = 0; round < 32; round++)
	{
		rc = 1;
		if (g->state != G_START && g->state != G_DONE && NowMs() - g->last_progress > g->stall_ms)
			rc = GetFail(g, CURLE_OPERATION_TIMEDOUT, errbuf, errsize, "Operation timed out: no data for a long time");
		else
			switch (g->state)
			{
				case G_START:
				{
					int done;

					rc = ParseUrl(g->cur, &g->u);
					if (rc == CURLE_UNSUPPORTED_PROTOCOL || (!rc && g->u.https))
					{
						rc = GetFail(g, CURLE_UNSUPPORTED_PROTOCOL, errbuf, errsize, "Protocol not supported (the PS2 build has no TLS: only http://)");
						break;
					}
					if (rc)
					{
						rc = GetFail(g, rc, errbuf, errsize, "URL using bad/illegal format or missing URL");
						break;
					}
					rc = ConnectBegin(&g->u, &g->s, &done, errbuf, errsize);
					if (rc)
					{
						g->s = SOCK_BAD;
						g->state = G_DONE;
						rc = -rc;
						break;
					}
					g->reqlen = snprintf(g->req, sizeof g->req, "GET %s HTTP/1.1\r\nHost: %s%s%s\r\nUser-Agent: %s\r\nAccept: */*\r\nConnection: close\r\n\r\n",
						g->u.path, g->u.host, strcmp(g->u.port, "80") ? ":" : "", strcmp(g->u.port, "80") ? g->u.port : "", g->useragent);
					if (g->reqlen <= 0 || g->reqlen >= (int)sizeof g->req)
					{
						rc = GetFail(g, CURLE_URL_MALFORMAT, errbuf, errsize, "URL too long");
						break;
					}
					g->reqsent = 0;
					g->headlen = 0;
					g->status = 0;
					g->clen = -1;
					g->got = 0;
					g->chunked = 0;
					g->cs = CS_SIZE;
					g->sizelen = 0;
					g->state = done ? G_SEND : G_CONNECT;
					g->last_progress = NowMs();
					rc = 2; // go on in this call
					break;
				}
				case G_CONNECT:
				{
					int done;

					rc = ConnectPoll(g->s, g->u.host, 0, &done, errbuf, errsize);
					if (rc)
					{
						g->s = SOCK_BAD;
						g->state = G_DONE;
						rc = -rc;
						break;
					}
					if (!done)
					{
						rc = 1;
						break;
					}
					g->state = G_SEND;
					g->last_progress = NowMs();
					rc = 2;
					break;
				}
				case G_SEND:
				{
					fd_set wfds;
					struct timeval tv = {0, 0};
					int k;

					FD_ZERO(&wfds);
					FD_SET(g->s, &wfds);
					if (select((int)g->s + 1, NULL, &wfds, NULL, &tv) <= 0)
					{
						rc = 1;
						break;
					}
					k = (int)send(g->s, g->req + g->reqsent, (size_t)(g->reqlen - g->reqsent), 0);
					if (k <= 0)
					{
						rc = GetFail(g, CURLE_SEND_ERROR, errbuf, errsize, "Failure when sending the request");
						break;
					}
					g->reqsent += k;
					g->last_progress = NowMs();
					if (g->reqsent >= g->reqlen)
						g->state = G_HEAD;
					rc = 2;
					break;
				}
				case G_HEAD:
				case G_BODY:
				{
					fd_set rfds;
					struct timeval tv = {0, 0};
					char tmp[4096];
					int k, fed = 0, want = (int)sizeof tmp;

					FD_ZERO(&rfds);
					FD_SET(g->s, &rfds);
					if (select((int)g->s + 1, &rfds, NULL, NULL, &tv) <= 0)
					{
						rc = 1;
						break;
					}
					if (g->state == G_HEAD && want > (int)sizeof g->head - 1 - g->headlen)
						want = (int)sizeof g->head - 1 - g->headlen;
					k = (int)recv(g->s, tmp, (size_t)want, 0);
					if (k < 0)
					{
						rc = GetFail(g, CURLE_RECV_ERROR, errbuf, errsize, "Failure when receiving data from the peer");
						break;
					}
					if (k == 0) // the server closed the connection
					{
						if (g->state == G_BODY && !g->chunked && g->clen < 0)
						{
							sock_close(g->s);
							g->s = SOCK_BAD;
							g->state = G_DONE;
							rc = 0;
						}
						else
							rc = GetFail(g, CURLE_RECV_ERROR, errbuf, errsize, g->state == G_HEAD ? "Empty reply from server" : "Transfer closed with data outstanding");
						break;
					}
					g->last_progress = NowMs();
					rc = 2;
					if (g->state == G_BODY)
						fed = GetFeed(g, tmp, (size_t)k, fn, ud);
					else
					{
						char *he;

						memcpy(g->head + g->headlen, tmp, (size_t)k);
						g->headlen += k;
						g->head[g->headlen] = '\0';
						he = strstr(g->head, "\r\n\r\n");
						if (!he)
						{
							if (g->headlen >= (int)sizeof g->head - 1)
								rc = GetFail(g, CURLE_RECV_ERROR, errbuf, errsize, "The reply header is too large");
							break;
						}
						{
							const size_t bodystart = (size_t)(he + 4 - g->head);
							const int hrc = GetHeader(g, he, errbuf, errsize);

							if (hrc < 0)
							{
								rc = hrc;
								break;
							}
							if (hrc > 0)
								break; // redirected
							g->state = G_BODY;
							if ((size_t)g->headlen > bodystart)
								fed = GetFeed(g, g->head + bodystart, (size_t)g->headlen - bodystart, fn, ud);
							else if (g->clen == 0 && !g->chunked)
								fed = 1;
						}
					}
					if (fed < 0)
						rc = GetFail(g, -fed, errbuf, errsize, "Failure writing output to destination");
					else if (fed > 0)
					{
						sock_close(g->s);
						g->s = SOCK_BAD;
						g->state = G_DONE;
						rc = 0;
					}
					break;
				}
				default:
					rc = 0;
					break;
			}
		if (rc != 2)
			break;
	}
	if (status)
		*status = g->status;
	if (total)
		*total = g->clen;
	if (got)
		*got = g->got;
	return rc == 2 ? 1 : rc;
}

// ===== the libcurl-easy shape =====

CURLcode curl_global_init(long flags)
{
	(void)flags;
#ifdef _WIN32
	{
		WSADATA wsa;

		WSAStartup(MAKEWORD(2, 2), &wsa);
	}
#endif
	return CURLE_OK;
}

void curl_global_cleanup(void)
{
}

CURL *curl_easy_init(void)
{
	CURL *c = calloc(1, sizeof *c);

	if (c)
	{
		c->timeout = 0;
		c->maxredirs = 30;
	}
	return c;
}

void curl_easy_cleanup(CURL *c)
{
	free(c);
}

CURLcode curl_easy_setopt(CURL *c, CURLoption opt, ...)
{
	va_list ap;
	CURLcode rc = CURLE_OK;

	va_start(ap, opt);
	switch (opt)
	{
		case CURLOPT_URL:
			snprintf(c->url, sizeof c->url, "%s", va_arg(ap, const char *));
			break;
		case CURLOPT_FOLLOWLOCATION:
			c->follow = va_arg(ap, long);
			break;
		case CURLOPT_IPRESOLVE:
			c->ipresolve = va_arg(ap, long);
			break;
		case CURLOPT_INTERFACE:
			(void)va_arg(ap, const char *);
			break;
		case CURLOPT_TIMEOUT:
			c->timeout = va_arg(ap, long);
			break;
		case CURLOPT_MAXREDIRS:
			c->maxredirs = va_arg(ap, long);
			break;
		case CURLOPT_WRITEFUNCTION:
			c->write_fn = va_arg(ap, ps2curl_write_fn);
			break;
		case CURLOPT_WRITEDATA:
			c->write_data = va_arg(ap, void *);
			break;
		case CURLOPT_USERAGENT:
			snprintf(c->useragent, sizeof c->useragent, "%s", va_arg(ap, const char *));
			break;
		case CURLOPT_ERRORBUFFER:
			c->errbuf = va_arg(ap, char *);
			break;
		case CURLOPT_VERBOSE:
			(void)va_arg(ap, long);
			break;
		case CURLOPT_STDERR:
			(void)va_arg(ap, void *);
			break;
		case CURLOPT_POSTFIELDS:
			c->postfields = va_arg(ap, char *);
			c->post = 1;
			if (c->postfields && c->postsize == 0)
				c->postsize = -1; // length by strlen at perform time
			break;
		case CURLOPT_POST:
			c->post = va_arg(ap, long) != 0;
			break;
		case CURLOPT_POSTFIELDSIZE:
			c->postsize = va_arg(ap, long);
			break;
		default:
			rc = CURLE_UNKNOWN_OPTION;
			break;
	}
	va_end(ap);
	return rc;
}

CURLcode curl_easy_perform(CURL *c)
{
	long size = c->postsize;

	if (!c->url[0])
		return CURLE_URL_MALFORMAT;
#ifdef _EE
	if (!PS2Net_Up())
	{
		if (c->errbuf)
			snprintf(c->errbuf, CURL_ERROR_SIZE, "The network is not available");
		return CURLE_COULDNT_CONNECT;
	}
#endif
	if (c->post && c->postfields && size < 0)
		size = (long)strlen(c->postfields);
	return PS2Http_Request(c->url, c->postfields, size, c->post, c->timeout, (int)c->maxredirs, (int)c->follow, c->useragent,
		c->write_fn, c->write_data, &c->status, c->errbuf, CURL_ERROR_SIZE);
}

CURLcode curl_easy_getinfo(CURL *c, CURLINFO info, ...)
{
	va_list ap;
	CURLcode rc = CURLE_OK;

	va_start(ap, info);
	if (info == CURLINFO_RESPONSE_CODE)
		*va_arg(ap, long *) = c->status;
	else
		rc = CURLE_UNKNOWN_OPTION;
	va_end(ap);
	return rc;
}

char *curl_easy_escape(CURL *c, const char *s, int length)
{
	const size_t n = length > 0 ? (size_t)length : strlen(s);
	char *out = malloc(n * 3 + 1), *o = out;
	size_t i;

	(void)c;
	if (!out)
		return NULL;
	for (i = 0; i < n; i++)
	{
		const unsigned char ch = (unsigned char)s[i];

		if (isalnum(ch) || ch == '-' || ch == '.' || ch == '_' || ch == '~')
			*o++ = (char)ch;
		else
			o += sprintf(o, "%%%02X", ch);
	}
	*o = '\0';
	return out;
}

char *curl_easy_unescape(CURL *c, const char *s, int length, int *outlength)
{
	const size_t n = length > 0 ? (size_t)length : strlen(s);
	char *out = malloc(n + 1), *o = out;
	size_t i;

	(void)c;
	if (!out)
		return NULL;
	for (i = 0; i < n; i++)
	{
		if (s[i] == '%' && i + 2 < n && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2]))
		{
			const char hex[3] = {s[i + 1], s[i + 2], 0};

			*o++ = (char)strtol(hex, NULL, 16);
			i += 2;
		}
		else
			*o++ = s[i];
	}
	*o = '\0';
	if (outlength)
		*outlength = (int)(o - out);
	return out;
}

void curl_free(void *p)
{
	free(p);
}
