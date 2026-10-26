// OPT14 GIF: host test of the event queue loop of D_ProcessEvents (src/d_main.c): a responder that drains the queue itself (the network bring-up screen of ps2_netui.c does)
// made the old loop - which steps eventtail on AFTER the body - replay the whole ring; the PS2 loop takes the event out first. Build: gcc -O1 -Wall -o /tmp/evq tools/ps2/evqueue_hosttest.c && /tmp/evq
#include <stdio.h>
#include <string.h>

#define MAXEVENTS 128
enum { ev_keydown, ev_keyup, ev_text, ev_console, ev_mouse, ev_joystick };
typedef struct { int type, key, x, y, repeated; } event_t;

static event_t events[MAXEVENTS];
static int eventhead, eventtail;

static void post(int type, int key)
{
	event_t e = {type, key, 0, 0, 0};
	events[eventhead] = e;
	eventhead = (eventhead + 1) & (MAXEVENTS - 1);
}

static void drain(void) // ps2_netui.c Poll(): empties the queue, from the event the caller still holds
{
	for (; eventtail != eventhead; eventtail = (eventtail + 1) & (MAXEVENTS - 1))
		;
}

static int seen, phantom_key0, nested_at;

static void responder(const event_t *ev, int n)
{
	seen++;
	if (ev->type == ev_keydown && ev->key == 0)
		phantom_key0++;
	if (n == nested_at)
		drain(); // the menu handler of that event runs a blocking request with a "please wait" screen
}

static void process_old(void)
{
	int n = 0;
	for (; eventtail != eventhead; eventtail = (eventtail + 1) & (MAXEVENTS - 1))
		responder(&events[eventtail], n++);
}

static void process_new(void)
{
	int n = 0;
	while (eventtail != eventhead)
	{
		event_t evcopy = events[eventtail];
		eventtail = (eventtail + 1) & (MAXEVENTS - 1);
		responder(&evcopy, n++);
	}
}

static int run(void (*process)(void), const char *name, int nposted)
{
	int i;
	memset(events, 0, sizeof events);
	eventhead = eventtail = 0;
	seen = phantom_key0 = 0;
	nested_at = 1; // the second event starts the nested drain
	for (i = 0; i < nposted; i++)
		post(ev_keydown, 'a' + i % 20);
	process();
	printf("%-4s posted %3d: responder saw %3d events, %3d of them keydown/key 0, tail %d head %d\n", name, nposted, seen, phantom_key0, eventtail, eventhead);
	return seen;
}

int main(void)
{
	int bad = 0;

	run(process_old, "old", 8);
	run(process_old, "old", 40);
	bad += run(process_new, "new", 8) != 2; // the event that ran the drain and the first one: the rest went to the nested loop
	bad += run(process_new, "new", 40) != 2;
	bad += phantom_key0 != 0;
	// no nesting: both loops give every event once, in order
	nested_at = -1;
	{
		int i, a, b;
		memset(events, 0, sizeof events);
		eventhead = eventtail = 0;
		for (i = 0; i < 100; i++)
			post(ev_keydown, i + 1);
		seen = 0; process_old(); a = seen;
		for (i = 0; i < 100; i++)
			post(ev_keydown, i + 1);
		seen = 0; process_new(); b = seen;
		printf("no nesting: old %d new %d events (100 each expected)\n", a, b);
		bad += !(a == 100 && b == 100);
	}
	printf(bad ? "FAIL\n" : "OK\n");
	return bad != 0;
}
