// PS2-130: the subset of the libcurl "easy" interface that src/netcode/http-mserv.c (master server client) uses, implemented as a plain
// HTTP/1.1 client over BSD sockets (the PS2 IP stack, src/ps2/ps2_net.c). http-mserv.c includes this instead of <curl/curl.h> on the PS2.
// Not libcurl: no TLS (https:// URLs fail with CURLE_UNSUPPORTED_PROTOCOL), no cookies/proxies/IPv6, GET and POST with a form body only.
#ifndef __PS2_CURL_H__
#define __PS2_CURL_H__

#include <stddef.h>
#include <stdio.h>

typedef struct ps2_curl CURL;

typedef enum
{
	CURLE_OK = 0,
	CURLE_UNSUPPORTED_PROTOCOL = 1,
	CURLE_FAILED_INIT = 2,
	CURLE_URL_MALFORMAT = 3,
	CURLE_COULDNT_RESOLVE_HOST = 6,
	CURLE_COULDNT_CONNECT = 7,
	CURLE_HTTP_RETURNED_ERROR = 22,
	CURLE_ABORTED_BY_CALLBACK = 42, // the player cancelled on the waiting screen (PS2-NET-9)
	CURLE_WRITE_ERROR = 23,
	CURLE_OUT_OF_MEMORY = 27,
	CURLE_OPERATION_TIMEDOUT = 28,
	CURLE_TOO_MANY_REDIRECTS = 47,
	CURLE_UNKNOWN_OPTION = 48,
	CURLE_SEND_ERROR = 55,
	CURLE_RECV_ERROR = 56
} CURLcode;

typedef enum
{
	CURLOPT_URL = 1,
	CURLOPT_FOLLOWLOCATION,
	CURLOPT_IPRESOLVE,
	CURLOPT_INTERFACE,
	CURLOPT_TIMEOUT,
	CURLOPT_MAXREDIRS,
	CURLOPT_WRITEFUNCTION,
	CURLOPT_WRITEDATA,
	CURLOPT_USERAGENT,
	CURLOPT_ERRORBUFFER,
	CURLOPT_VERBOSE,
	CURLOPT_STDERR,
	CURLOPT_POSTFIELDS,
	CURLOPT_POST,
	CURLOPT_POSTFIELDSIZE
} CURLoption;

typedef enum
{
	CURLINFO_RESPONSE_CODE = 1
} CURLINFO;

#define CURL_GLOBAL_ALL 3
#define CURL_ERROR_SIZE 256
#define CURL_IPRESOLVE_WHATEVER 0
#define CURL_IPRESOLVE_V4 1
#define CURL_IPRESOLVE_V6 2

typedef size_t (*ps2curl_write_fn)(char *data, size_t size, size_t nmemb, void *userdata);

CURLcode curl_global_init(long flags);
void curl_global_cleanup(void);
CURL *curl_easy_init(void);
void curl_easy_cleanup(CURL *curl);
CURLcode curl_easy_setopt(CURL *curl, CURLoption option, ...);
CURLcode curl_easy_perform(CURL *curl);
CURLcode curl_easy_getinfo(CURL *curl, CURLINFO info, ...);
char *curl_easy_escape(CURL *curl, const char *string, int length);
char *curl_easy_unescape(CURL *curl, const char *string, int length, int *outlength);
void curl_free(void *p);

// The HTTP engine on its own (host test: tools/ps2/ps2_http_hosttest.c): the response body of one request, no libcurl types.
// Returns a CURLcode, status = the HTTP status of the last response. Writes through write_fn.
CURLcode PS2Http_Request(const char *url, const char *post, long postsize, int is_post, long timeout, int maxredirs, int follow,
	const char *useragent, ps2curl_write_fn write_fn, void *userdata, long *status, char *errbuf, size_t errsize);

// PS2-137: streaming GET stepped from the game loop (see ps2_curl.c): the add-on download from the server's HTTP source.
// Open() never blocks (nothing is resolved or connected yet). Step() advances what the sockets allow and returns 1 (go on), 0 (the whole body was
// delivered through write_fn) or -CURLcode (a failure; errbuf has the text, a 4xx/5xx answer is CURLE_HTTP_RETURNED_ERROR with its code in *status).
// total is the Content-Length (-1 unknown), got the bytes delivered so far. stall_seconds = how long the connection may be silent (default 20).
typedef struct ps2_httpget ps2_httpget_t;
ps2_httpget_t *PS2HttpGet_Open(const char *url, long stall_seconds, int maxredirs, const char *useragent);
int PS2HttpGet_Step(ps2_httpget_t *g, ps2curl_write_fn write_fn, void *userdata, long *status, long *total, long *got, char *errbuf, size_t errsize);
void PS2HttpGet_Close(ps2_httpget_t *g);

#endif
