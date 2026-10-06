// Host test (Winsock) of the PS2 HTTP client (src/ps2/ps2_curl.c) against tools/ps2/mock_masterserver.py: run by ps2_http_hosttest.py.
// usage: ps2_http_hosttest BASEURL   -> prints "T <name> <PASS|FAIL> ..." lines
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/ps2/ps2_curl.h"

static char out[65536];
static size_t outn;
static int fails;

static size_t OnRead(char *s, size_t sz, size_t n, void *ud)
{
	(void)ud;
	if (outn + sz * n >= sizeof out)
		return 0;
	memcpy(out + outn, s, sz * n);
	outn += sz * n;
	out[outn] = '\0';
	return n;
}

static void Check(const char *name, int ok, const char *extra)
{
	printf("T %-28s %s %s\n", name, ok ? "PASS" : "FAIL", extra ? extra : "");
	if (!ok)
		fails++;
}

static CURLcode Do(const char *url, const char *post, long timeout, long *status, char *err)
{
	CURL *c = curl_easy_init();
	CURLcode rc;

	outn = 0;
	out[0] = 0;
	err[0] = 0;
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, err);
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, OnRead);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "ps2-http-hosttest/1");
	if (post)
		curl_easy_setopt(c, CURLOPT_POSTFIELDS, post);
	rc = curl_easy_perform(c);
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, status);
	curl_easy_cleanup(c);
	return rc;
}

int main(int argc, char **argv)
{
	const char *base = argc > 1 ? argv[1] : "http://127.0.0.1:8080/MS/0";
	char url[512], err[256], tok[64];
	long st;
	CURLcode rc;
	char *esc, *un;
	int n;

	curl_global_init(CURL_GLOBAL_ALL);

	if (argc > 2 && !strcmp(argv[2], "redirect"))
	{
		// the mock answers GET / with 302 -> <base>/servers: the client has to follow it (and a POST through 302 becomes a GET)
		char root[256];
		const char *slash = strchr(base + 8, '/');

		snprintf(root, sizeof root, "%.*s/", (int)(slash - base), base);
		rc = Do(root, NULL, 5, &st, err);
		Check("302 followed", rc == CURLE_OK && st == 200 && strstr(out, "1\n"), err);
		printf("RESULT %s\n", fails ? "FAIL" : "PASS");
		return fails ? 1 : 0;
	}

	snprintf(url, sizeof url, "%s/rooms", base);
	rc = Do(url, NULL, 5, &st, err);
	Check("get rooms", rc == CURLE_OK && st == 200 && strstr(out, "1\nStandard\n") && strstr(out, "\n\n\n"), err);

	esc = curl_easy_escape(NULL, "My SRB2 server/\xC3\xA9", 0);
	Check("escape", esc && !strcmp(esc, "My%20SRB2%20server%2F%C3%A9"), esc);
	un = curl_easy_unescape(NULL, esc, 0, &n);
	Check("unescape", un && n == 17 && !strcmp(un, "My SRB2 server/\xC3\xA9"), NULL);
	{
		char post[256];

		snprintf(post, sizeof post, "port=%d&title=%s&version=%s", 5029, esc, "2.2.15");
		snprintf(url, sizeof url, "%s/rooms/1/register", base);
		rc = Do(url, post, 5, &st, err);
		Check("post register", rc == CURLE_OK && st == 200 && !strncmp(out, "tok", 3), err);
		snprintf(tok, sizeof tok, "%s", strtok(out, "\n"));
	}
	snprintf(url, sizeof url, "%s/servers", base);
	rc = Do(url, NULL, 5, &st, err);
	Check("get servers lists host", rc == CURLE_OK && st == 200 && strstr(out, " 5029 My%20SRB2%20server%2F%C3%A9 2.2.15"), out);
	snprintf(url, sizeof url, "%s/servers/%s/update", base, tok);
	rc = Do(url, "title=Renamed", 5, &st, err);
	Check("post update", rc == CURLE_OK && st == 200, err);
	snprintf(url, sizeof url, "%s/rooms/1/servers", base);
	rc = Do(url, NULL, 5, &st, err);
	Check("room servers shows rename", rc == CURLE_OK && strstr(out, " 5029 Renamed 2.2.15"), out);
	snprintf(url, sizeof url, "%s/servers/%s/unlist", base, tok);
	{
		CURL *c = curl_easy_init();

		outn = 0;
		curl_easy_setopt(c, CURLOPT_URL, url);
		curl_easy_setopt(c, CURLOPT_POST, 1L);
		curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, 0L);
		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, OnRead);
		curl_easy_setopt(c, CURLOPT_TIMEOUT, 5L);
		rc = curl_easy_perform(c);
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &st);
		curl_easy_cleanup(c);
		Check("post unlist (empty body)", rc == CURLE_OK && st == 200, NULL);
	}
	snprintf(url, sizeof url, "%s/servers", base);
	rc = Do(url, NULL, 5, &st, err);
	Check("unlisted host is gone", rc == CURLE_OK && !strstr(out, " 5029 "), out);
	snprintf(url, sizeof url, "%s/nothing", base);
	rc = Do(url, NULL, 5, &st, err);
	Check("404 is an HTTP result", rc == CURLE_OK && st == 404, NULL);
	rc = Do("https://127.0.0.1:1/x", NULL, 5, &st, err);
	Check("https refused cleanly", rc == CURLE_UNSUPPORTED_PROTOCOL && err[0], err);
	rc = Do("http://127.0.0.1:1/x", NULL, 5, &st, err);
	Check("connection refused", rc == CURLE_COULDNT_CONNECT && err[0], err);
	rc = Do("http://10.255.255.1:81/x", NULL, 2, &st, err);
	Check("connect timeout (2 s)", rc == CURLE_OPERATION_TIMEDOUT, err);
	rc = Do("ftp://x/y", NULL, 2, &st, err);
	Check("unsupported scheme", rc == CURLE_UNSUPPORTED_PROTOCOL, err);
	rc = Do("http://", NULL, 2, &st, err);
	Check("malformed url", rc == CURLE_URL_MALFORMAT, err);
	curl_free(esc);
	curl_free(un);
	printf("RESULT %s\n", fails ? "FAIL" : "PASS");
	return fails ? 1 : 0;
}
