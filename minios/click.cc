/*
 *          ClickOS
 *
 *   file: click.cc
 *
 *          NEC Europe Ltd. PROPRIETARY INFORMATION
 *
 * This software is supplied under the terms of a license agreement
 * or nondisclosure agreement with NEC Europe Ltd. and may not be
 * copied or disclosed except in accordance with the terms of that
 * agreement. The software and its source code contain valuable trade
 * secrets and confidential information which have to be maintained in
 * confidence.
 * Any unauthorized publication, transfer to third parties or duplication
 * of the object or source code - either totally or in part – is
 * prohibited.
 *
 *      Copyright (c) 2014 NEC Europe Ltd. All Rights Reserved.
 *
 * Authors: Joao Martins <joao.martins@neclab.eu>
 * 	    Filipe Manco <filipe.manco@neclab.eu>
 *
 * NEC Europe Ltd. DISCLAIMS ALL WARRANTIES, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE AND THE WARRANTY AGAINST LATENT
 * DEFECTS, WITH RESPECT TO THE PROGRAM AND THE ACCOMPANYING
 * DOCUMENTATION.
 *
 * No Liability For Consequential Damages IN NO EVENT SHALL NEC Europe
 * Ltd., NEC Corporation OR ANY OF ITS SUBSIDIARIES BE LIABLE FOR ANY
 * DAMAGES WHATSOEVER (INCLUDING, WITHOUT LIMITATION, DAMAGES FOR LOSS
 * OF BUSINESS PROFITS, BUSINESS INTERRUPTION, LOSS OF INFORMATION, OR
 * OTHER PECUNIARY LOSS AND INDIRECT, CONSEQUENTIAL, INCIDENTAL,
 * ECONOMIC OR PUNITIVE DAMAGES) ARISING OUT OF THE USE OF OR INABILITY
 * TO USE THIS PROGRAM, EVEN IF NEC Europe Ltd. HAS BEEN ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGES.
 *
 *     THIS HEADER MAY NOT BE EXTRACTED OR MODIFIED IN ANY WAY.
 */

#include <click/config.h>
#include <click/lexer.hh>
#include <click/router.hh>
#include <click/master.hh>
#include <click/error.hh>
#include <click/timer.hh>
#include <click/string.hh>
#include <click/straccum.hh>
#include <click/driver.hh>

extern "C"{
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <mini-os/xenbus.h>
#include <mini-os/shutdown.h>
#include <mini-os/sched.h>
}

void *__dso_handle = NULL;

#define NLOG(fmt, ...)
#define LOG(fmt, ...) \
	printf("[%s:%d] " fmt "\n", \
		__FUNCTION__, __LINE__, ##__VA_ARGS__)

/**
 *  a simple event emitter to
 * control xenstore events
 */
#define MAX_EVENTS	256
#define MAX_EVENTS_KEY	1024

typedef u_int (*event_fn_t) (char *key, void *data);
struct event {
	u_int wildcard;
	event_fn_t fn;
} listeners[MAX_EVENTS];

inline
uint32_t h(const char * s)
{
	uint32_t hash = 0;

	for(; *s; ++s) {
		hash += *s;
		hash += (hash << 10);
		hash ^= (hash >> 6);
	}
	hash += (hash << 3);
	hash ^= (hash >> 11);
	hash += (hash << 15);
	return hash % MAX_EVENTS;
}

u_int stripw(const char *key)
{
	char *si = strchr(key, '*');
	return si ? si - key : strlen(key);
}

void on(const char *_key, event_fn_t func)
{
	char key[MAX_EVENTS_KEY];
	u_int k, w;

	memset(key, 0, MAX_EVENTS_KEY);

	w = stripw(_key);
	strncpy(key, (char*) _key, w);
	k = h(key);

	NLOG("on %d for %s", k, key);

	listeners[k].fn = func;

	if (strlen(_key) != w) {
		listeners[k].wildcard = w;
		LOG("* %d for %s", w, _key);
	}
}

int fire(char *key, void *data)
{
	int k, w = 0, rc = -EINVAL;
	char buf[MAX_EVENTS_KEY];
	void *arg0 = data;

	for (k=0; k<MAX_EVENTS; ++k) {
		struct event evt = listeners[k];
		if (!evt.wildcard)
			continue;

		memset(buf, 0, MAX_EVENTS_KEY);
		strncpy(buf, key, evt.wildcard);
		k = h(buf);
		if (listeners[k].fn) {
			w = listeners[k].wildcard;
			break;
		}
	}

	k = h(buf);
	NLOG("fire %d for %s", k, key);
	if (listeners[k].fn)
		rc = listeners[k].fn(key + w, arg0);
	return rc;
}

/**
 * xenstore helpers
 */
#define PATH_ROOT	"clickos"
#define PATH_MAX_LEN	1024

struct xenstore_dev {
	domid_t dom;
	char *nodename;
	xenbus_event_queue events;
} *xsdev = NULL;

uint32_t domid = -1;
u_int _shutdown = 0;
u_int _reason = 0;

#define path_strip(p) \
	String(p, stripw(p)).c_str()

#define xb_read(path, val) \
	xenbus_read(XBT_NIL, path, val);

#define xb_write(path, val) \
	xenbus_write(XBT_NIL, path, val);

#define xb_watch(path, name, cb) \
	xenbus_watch_path_token(XBT_NIL, path_strip(path), name, &xsdev->events); \
	on(path, cb);

#define xb_unwatch(path, name) \
	xenbus_unwatch_path_token(XBT_NIL, path_strip(path), name);

#define xb_wait() \
	xenbus_wait_for_watch_return(&xsdev->events);

#define xb_release() \
	xenbus_release_wait_for_watch(&xsdev->events);

String
read_config()
{
	char path[PATH_MAX_LEN];
	String cfg;
	char *token;

	for (int i = 0;; i++) {
		snprintf(path, PATH_MAX_LEN, "clickos/0/click_os_config/%d", i);
		xb_read(path, &token);
		if (token == NULL)
			break;
		cfg += token;
	}

	return cfg;
}

/*
 * click glue
 */
static Master master(1);
static String router_config;
static u_int f_stop = 1;
static Router *r;
static struct thread* thread;

void
router_thread(void *thread_data)
{
	Vector<String> v;
	ErrorHandler *errh;
	String config(router_config);

	click_static_initialize();
	errh = ErrorHandler::default_handler();
	r = click_read_router(config, true, errh, false, &master);
	f_stop = 0;

	click_lexer()->element_type_names(v);
	StringAccum sa;

	for (int i = 0; i < v.size(); i++)
		sa << v[i] << "\n";

	if (r->initialize(errh) < 0) {
		LOG("Router init failed!");
		f_stop = 1;
		return;
	}


	r->use();
	r->activate(errh);

	LOG("Starting driver...\n\n");
	r->master()->thread(0)->driver();

	LOG("Stopping driver...\n\n");
	r->unuse();
	f_stop = 1;

	LOG("Master/driver stopped, closing router_thread");
}

void
router_suspend()
{
	master.suspend();
}

void
router_resume()
{
	master.resume();
}

void
router_stop()
{
	if (f_stop)
		return;

	do {
		r->please_stop_driver();
		schedule();
	} while (!(f_stop));
}

/*
 * event emitter callbacks
 */
u_int
on_status(char *key, void *data)
{
	String status = (char *) data;

	LOG("status change to %s", data);
	if (status != "Running")
		return -EINVAL;

	router_config = read_config();
	thread = create_thread((char*)"click", router_thread, NULL);
	return 0;
}

static void
read_cname(char *path, Element **e, const Handler **h)
{
	int sep = strchr(path, '/') - path;
	String ename(path, sep), hname(path + sep + 1);

	NLOG("sep=%d element=%s handler=%s", sep,
			ename.c_str(), hname.c_str());

	*e = r->find(ename);
	*h = Router::handler(*e, hname);
	NLOG("ename %s=%p hname %s=%p", ename.c_str(), e, hname.c_str(), h);
}

u_int
on_elem_readh(char *key, void *data)
{
	Element *e;
	const Handler* h;
	String val, h_path, lock_path;

    if (strncmp(key, "/read/", 6))
        return 0;

	if (f_stop)
		return 0;

	read_cname(key+6, &e, &h);

	if (!h || !h->readable())
		return EINVAL;

	val = h->call_read(e, "", ErrorHandler::default_handler());
	h_path = "clickos/0/elements/" + e->name() + "/" + h->name();
	lock_path = h_path + "/lock";

	xb_write(h_path.c_str(), val.c_str());
	xb_write(lock_path.c_str(), "0");

	NLOG("element handler read %s", val.c_str());
	return 0;
}

u_int
on_elem_writeh(char *key, void *data)
{
	Element *e;
	const Handler* h;
	int rc;

	if (f_stop || strlen((char *) data) == 0)
		return 0;

	read_cname(key+1, &e, &h);

	if (!h || !h->writable())
		return EINVAL;

	rc = h->call_write(String(data), e, ErrorHandler::default_handler());
	NLOG("element handler write %s value %s", data, "");
	return 0;
}

/* runs the event loop */
int main(int argc, char **argv)
{
	size_t len = sizeof(struct xenstore_dev);
	xsdev = (struct xenstore_dev*) malloc(len);
	memset(xsdev, 0, len);
	memset(listeners, 0, MAX_EVENTS * sizeof(struct event));

	xb_watch("clickos/0/status", "status-watch", on_status);
	xb_watch("clickos/0/elements*", "elements-watch", on_elem_writeh);
	xb_watch("clickos/0/control*", "control-watch", on_elem_readh);

	while (!_shutdown) {
		String value;
		char *err, *val, **path;

		path = xb_wait();
		if (!path || _shutdown)
			continue;

		err = xb_read(*path, &val);
		if (err)
			continue;

		fire(*path, val);
	}

	LOG("Shutting down...");
	xb_unwatch("clickos/0/status", "status-watch");
	xb_unwatch("clickos/0/elements*", "elements-watch");
	xb_unwatch("clickos/0/control*", "control-watch");

	fini_xenbus();
	free(xsdev);

	return _reason;
}

/* app shutdown hook from minios */
extern "C" {
int app_shutdown(unsigned reason)
{
	switch (reason) {
	case SHUTDOWN_poweroff:
		LOG("Requested shutdown reason=poweroff");
		_shutdown = 1;
		_reason = reason;
		router_stop();
		xb_release();
		break;
	case SHUTDOWN_reboot:
		LOG("Requested shutdown reason=reboot");
		_shutdown = 1;
		_reason = reason;
		router_stop();
		xb_release();
		break;
	case SHUTDOWN_suspend:
		LOG("Requested shutdown reason=suspend");
		router_suspend();
		kernel_suspend();
		router_resume();
		break;
	default:
		LOG("Requested shutdown with invalid reason (%d)", reason);
		break;
	}

	return 0;
}
}
