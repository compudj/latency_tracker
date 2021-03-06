/*
 * latency_tracker.c
 *
 * Latency tracker
 *
 * Copyright (C) 2014 Julien Desfossez <jdesfossez@efficios.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; only
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <linux/module.h>
#include <linux/preempt_mask.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/version.h>
#include <linux/jhash.h>
#include <linux/module.h>
#include "latency_tracker.h"
#include "wrapper/jiffies.h"
#include "wrapper/vmalloc.h"
#include "wrapper/tracepoint.h"
#include "wrapper/ht.h"
#include "tracker_private.h"
#define CREATE_TRACE_POINTS
#include <trace/events/latency_tracker.h>

#define DEFAULT_MAX_ALLOC_EVENTS 100

EXPORT_TRACEPOINT_SYMBOL_GPL(sched_latency);
EXPORT_TRACEPOINT_SYMBOL_GPL(net_latency);
EXPORT_TRACEPOINT_SYMBOL_GPL(block_latency);

static void latency_tracker_enable_gc(struct latency_tracker *tracker);
static void latency_tracker_gc_cb(unsigned long ptr);

/*
 * Function to get the timestamp.
 * FIXME: import the goodness from the LTTng trace clock
 */
static inline u64 trace_clock_monotonic_wrapper(void)
{
	ktime_t ktime;

	/*
	 * Refuse to trace from NMIs with this wrapper, because an NMI could
	 * nest over the xtime write seqlock and deadlock.
	 */
	if (in_nmi())
		return (u64) -EIO;

	ktime = ktime_get();
	return ktime_to_ns(ktime);
}

/*
 * Must be called with the tracker->lock held.
 */
static
struct latency_tracker_event *latency_tracker_get_event(
		struct latency_tracker *tracker)
{
	struct latency_tracker_event *e;

	if (list_empty(&tracker->events_free_list)) {
		goto error;
	}
	e = list_last_entry(&tracker->events_free_list,
			struct latency_tracker_event, list);
	list_del(&e->list);
	goto end;

error:
	e = NULL;
end:
	return e;
}

/*
 * Must be called with the tracker->lock held.
 */
static
void latency_tracker_put_event(struct latency_tracker *tracker,
		struct latency_tracker_event *e)
{
	memset(e, 0, sizeof(struct latency_tracker_event));
	list_add(&e->list, &tracker->events_free_list);
}

/*
 * Must be called with the tracker->lock held.
 */
static
void latency_tracker_event_destroy(struct latency_tracker *tracker,
		struct latency_tracker_event *s)
{
	unsigned long flags;

	spin_lock_irqsave(&tracker->lock, flags);
	wrapper_ht_del(tracker, s);
	if (s->timeout > 0)
		del_timer(&s->timer);
	latency_tracker_put_event(tracker, s);
	spin_unlock_irqrestore(&tracker->lock, flags);
}

static
void latency_tracker_destroy_free_list(struct latency_tracker *tracker)
{
	struct latency_tracker_event *e, *n;

	list_for_each_entry_safe(e, n, &tracker->events_free_list, list) {
		list_del(&e->list);
		kfree(e);
	}
}

static
void latency_tracker_gc_cb(unsigned long ptr)
{
	struct latency_tracker *tracker = (struct latency_tracker *) ptr;
	unsigned long flags;
	u64 now;

	now = trace_clock_monotonic_wrapper();
	spin_lock_irqsave(&tracker->lock, flags);
	wrapper_ht_gc(tracker, now);
	latency_tracker_enable_gc(tracker);
	spin_unlock_irqrestore(&tracker->lock, flags);
}

/* Must be called with the lock held. */
static
void latency_tracker_enable_gc(struct latency_tracker *tracker)
{
	del_timer(&tracker->timer);
	if (tracker->gc_period == 0 || tracker->gc_thresh == 0) {
		return;
	}

	init_timer(&tracker->timer);
	tracker->timer.function = latency_tracker_gc_cb;
	tracker->timer.expires = jiffies +
		wrapper_nsecs_to_jiffies(tracker->gc_period);
	tracker->timer.data = (unsigned long) tracker;
	add_timer(&tracker->timer);
}

void latency_tracker_set_gc_thresh(struct latency_tracker *tracker,
		uint64_t gc_thresh)
{
	unsigned long flags;

	spin_lock_irqsave(&tracker->lock, flags);
	tracker->gc_thresh = gc_thresh;
	latency_tracker_enable_gc(tracker);
	spin_unlock_irqrestore(&tracker->lock, flags);
}

void latency_tracker_set_gc_period(struct latency_tracker *tracker,
		uint64_t gc_period)
{
	unsigned long flags;

	spin_lock_irqsave(&tracker->lock, flags);
	tracker->gc_period = gc_period;
	latency_tracker_enable_gc(tracker);
	spin_unlock_irqrestore(&tracker->lock, flags);
}

struct latency_tracker *latency_tracker_create(
		int (*match_fct) (const void *key1, const void *key2,
			size_t length),
		u32 (*hash_fct) (const void *key, u32 length, u32 initval),
		int max_events, uint64_t gc_period, uint64_t gc_thresh,
		void *priv)

{
	struct latency_tracker *tracker;
	struct latency_tracker_event *e;
	int ret, i;

	tracker = kzalloc(sizeof(struct latency_tracker), GFP_KERNEL);
	if (!tracker) {
		printk("latency_tracker: Alloc tracker failed\n");
		goto error;
	}
	if (!hash_fct) {
		tracker->hash_fct = jhash;
	}
	if (!match_fct) {
		tracker->match_fct = memcmp;
	}
	if (!max_events)
		max_events = DEFAULT_MAX_ALLOC_EVENTS;
	tracker->gc_period = gc_period;
	tracker->gc_thresh = gc_thresh;
	tracker->priv = priv;

	latency_tracker_enable_gc(tracker);

	spin_lock_init(&tracker->lock);

	wrapper_ht_init(tracker);

	INIT_LIST_HEAD(&tracker->events_free_list);
	for (i = 0; i < max_events; i++) {
		e = kzalloc(sizeof(struct latency_tracker_event), GFP_KERNEL);
		if (!e)
			goto error_free_events;
		list_add(&e->list, &tracker->events_free_list);
	}
	wrapper_vmalloc_sync_all();
	ret = try_module_get(THIS_MODULE);
	if (!ret)
		goto error_free_events;

	goto end;

error_free_events:
	latency_tracker_destroy_free_list(tracker);
	kfree(tracker);
error:
	tracker = NULL;
end:
	return tracker;
}
EXPORT_SYMBOL_GPL(latency_tracker_create);

void latency_tracker_destroy(struct latency_tracker *tracker)
{
	unsigned long flags;
	int nb = 0;

	del_timer(&tracker->timer);
	nb = wrapper_ht_clear(tracker);
	printk("latency_tracker: %d events were still pending at destruction\n", nb);
	spin_lock_irqsave(&tracker->lock, flags);
	latency_tracker_destroy_free_list(tracker);
	spin_unlock_irqrestore(&tracker->lock, flags);
	kfree(tracker);
	module_put(THIS_MODULE);
}
EXPORT_SYMBOL_GPL(latency_tracker_destroy);

static
void latency_tracker_timeout_cb(unsigned long ptr)
{
	struct latency_tracker_event *data = (struct latency_tracker_event *) ptr;

	del_timer(&data->timer);
	data->cb_flag = LATENCY_TRACKER_CB_TIMEOUT;
	data->timeout = 0;

	/* Run the user-provided callback. */
	data->cb(ptr);
}

enum latency_tracker_event_in_ret latency_tracker_event_in(
		struct latency_tracker *tracker,
		void *key, size_t key_len, uint64_t thresh,
		void (*cb)(unsigned long ptr),
		uint64_t timeout, unsigned int unique, void *priv)
{
	struct latency_tracker_event *s;
	unsigned long flags;
	int ret;

	if (!tracker) {
		ret = LATENCY_TRACKER_ERR;
		goto end;
	}
	if (key_len > LATENCY_TRACKER_MAX_KEY_SIZE) {
		ret = LATENCY_TRACKER_ERR;
		goto end;
	}

	spin_lock_irqsave(&tracker->lock, flags);
	/*
	 * If we specify the unique property, get rid of other duplicate keys
	 * without calling the callback.
	 */
	if (unique)
		wrapper_ht_unique_check(tracker, s, key, key_len);

	s = latency_tracker_get_event(tracker);
	if (!s) {
		ret = LATENCY_TRACKER_FULL;
		goto error_unlock;
	}

	s->hkey = tracker->hash_fct(key, key_len, 0);
	memcpy(s->key, key, key_len);

	s->start_ts = trace_clock_monotonic_wrapper();
	s->thresh = thresh;
	s->timeout = timeout;
	s->cb = cb;
	s->priv = priv;

	if (timeout > 0) {
		init_timer(&s->timer);
		s->timer.function = latency_tracker_timeout_cb;
		s->timer.expires = jiffies +
			wrapper_nsecs_to_jiffies(timeout);
		s->timer.data = (unsigned long) s;
		add_timer(&s->timer);
	}

	wrapper_ht_add(tracker, s);
	ret = LATENCY_TRACKER_OK;

error_unlock:
	spin_unlock_irqrestore(&tracker->lock, flags);
end:
	return ret;
}
EXPORT_SYMBOL_GPL(latency_tracker_event_in);

int latency_tracker_event_out(struct latency_tracker *tracker,
		void *key, unsigned int key_len, unsigned int id)
{
	int ret;
	int found = 0;
	u64 now;

	if (!tracker) {
		goto error;
	}

	now = trace_clock_monotonic_wrapper();
	found = wrapper_ht_check_event(tracker, key, key_len, id, now);

	if (!found)
		goto error;

	ret = 0;
	goto end;

error:
	ret = -1;
end:
	return ret;
}
EXPORT_SYMBOL_GPL(latency_tracker_event_out);

void *latency_tracker_get_priv(struct latency_tracker *tracker)
{
	return tracker->priv;
}
EXPORT_SYMBOL_GPL(latency_tracker_get_priv);

void example_cb(unsigned long ptr)
{
	struct latency_tracker_event *data = (struct latency_tracker_event *) ptr;
	printk("cb called for key %s with %p, cb_flag = %d\n", (char *) data->key,
			data->priv, data->cb_flag);
}

static
int test_tracker(void)
{
	char *k1 = "blablabla1";
	char *k2 = "bliblibli1";
	int ret;
	struct latency_tracker *tracker;

	tracker = latency_tracker_create(NULL, NULL, 3, 0, 0, NULL);
	if (!tracker)
		goto error;

	printk("insert k1\n");
	ret = latency_tracker_event_in(tracker, k1, strlen(k1) + 1, 6,
			example_cb, 0, 0, NULL);
	if (ret)
		printk("failed\n");

	printk("insert k2\n");
	ret = latency_tracker_event_in(tracker, k2, strlen(k2) + 1, 400,
			example_cb, 0, 0, NULL);
	if (ret)
		printk("failed\n");

	printk("lookup k1\n");
	latency_tracker_event_out(tracker, k1, strlen(k1) + 1, 0);
	printk("lookup k2\n");
	latency_tracker_event_out(tracker, k2, strlen(k2) + 1, 0);
	printk("lookup k1\n");
	latency_tracker_event_out(tracker, k1, strlen(k1) + 1, 0);

	printk("done\n");
	latency_tracker_destroy(tracker);

	ret = 0;
	goto end;

error:
	ret = -1;
end:
	return ret;

}

static
int __init latency_tracker_init(void)
{
	int ret;

	ret = test_tracker();

	ret = lttng_tracepoint_init();
	if (ret)
		return ret;

	return ret;
}

static
void __exit latency_tracker_exit(void)
{
	lttng_tracepoint_exit();
}

module_init(latency_tracker_init);
module_exit(latency_tracker_exit);
MODULE_AUTHOR("Julien Desfossez <jdesfossez@efficios.com>");
MODULE_VERSION("1.0");
MODULE_LICENSE("GPL");
