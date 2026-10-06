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

// 0 ok, else a CURLcode. Waits for the connection for at most the time left (the stack's own connect timeout is far longer).
static int Connect(const url_t *u, long deadline, sock_t *out, char *errbuf, size_t errsize)
{
	struct sockaddr_in sa;
	sock_t s;
	int port = atoi(u->port);
	unsigned long ip;
	fd_set wfds, efds;
	struct timeval tv;
	long left;
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
		for (;;)
		{
			left = deadline - NowMs();
			if (left <= 0)
			{
				Fail(errbuf, errsize, "Connection timed out connecting to %s", u->host);
				sock_close(s);
				return CURLE_OPERATION_TIMEDOUT;
			}
			FD_ZERO(&wfds);
			FD_SET(s, &wfds);
			FD_ZERO(&efds);
			FD_SET(s, &efds); // Winsock reports a refused connection here
			tv.tv_sec = (left > 500 ? 500 : left) / 1000;
			tv.tv_usec = ((left > 500 ? 500 : left) % 1000) * 1000;
			rc = select((int)s + 1, NULL, &wfds, &efds, &tv);
			if (rc > 0)
			{
				int err = 0;
				socklen_t len = sizeof err;

				getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
				if (err)
				{
					Fail(errbuf, errsize, "Failed to connect to %s (error %d)", u->host, err);
					sock_close(s);
					return CURLE_COULDNT_CONNECT;
				}
				break;
			}
		}
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
		if (select((int)s + 1, NULL, &wfds, NULL, &tv) <= 0)
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
		if (select((int)s + 1, &rfds, NULL, NULL, &tv) <= 0)
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
