#include "corobus.h"

#include "libcoro.h"
#include "rlist.h"

#include <deque>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/**
 * One coroutine waiting to be woken up in a list of other
 * suspended coros.
 */
struct wakeup_entry {
	struct rlist base;
	struct coro *coro;
};

/** A queue of suspended coros waiting to be woken up. */
struct wakeup_queue {
	struct rlist coros;
};

#if 1 /* Uncomment this if want to use */

/** Suspend the current coroutine until it is woken up. */
static void
wakeup_queue_suspend_this(struct wakeup_queue *queue)
{
	struct wakeup_entry entry;
	entry.coro = coro_this();
	rlist_add_tail_entry(&queue->coros, &entry, base);
	coro_suspend();
	rlist_del_entry(&entry, base);
}

/** Wakeup the first coroutine in the queue. */
static void
wakeup_queue_wakeup_first(struct wakeup_queue *queue)
{
	if (rlist_empty(&queue->coros))
		return;
	struct wakeup_entry *entry = rlist_first_entry(&queue->coros,
		struct wakeup_entry, base);
	coro_wakeup(entry->coro);
}

#endif

struct coro_bus_channel {
	/** Channel max capacity. */
	size_t size_limit;
	/** Coroutines waiting until the channel is not full. */
	struct wakeup_queue send_queue;
	/** Coroutines waiting until the channel is not empty. */
	struct wakeup_queue recv_queue;
	/** Message queue. */
	std::deque<unsigned> data;
};

struct coro_bus {
	struct coro_bus_channel **channels;
	int channel_count;
};

static enum coro_bus_error_code global_error = CORO_BUS_ERR_NONE;

enum coro_bus_error_code
coro_bus_errno(void)
{
	return global_error;
}

void
coro_bus_errno_set(enum coro_bus_error_code err)
{
	global_error = err;
}

struct coro_bus *
coro_bus_new(void)
{
	struct coro_bus *bus = (struct coro_bus *)malloc(sizeof(*bus));
	assert(bus != NULL);

	bus->channels = NULL;
	bus->channel_count = 0;

	coro_bus_errno_set(CORO_BUS_ERR_NONE);
	return bus;
}


void
coro_bus_delete(struct coro_bus *bus)
{
	if (bus == NULL)
		return;

	for (int i = 0; i < bus->channel_count; ++i) {
		if (bus->channels[i] != NULL) {
			free(bus->channels[i]);
		}
	}
	free(bus->channels);
	free(bus);
}


int
coro_bus_channel_open(struct coro_bus *bus, size_t size_limit)
{
    assert(bus != NULL);

    for (int i = 0; i < bus->channel_count; ++i) {
        if (bus->channels[i] == NULL) {
            struct coro_bus_channel *chan = (struct coro_bus_channel *)malloc(sizeof(*chan));
            assert(chan != NULL);
            chan->size_limit = size_limit;
            rlist_create(&chan->send_queue.coros);
            rlist_create(&chan->recv_queue.coros);
            bus->channels[i] = chan;
            coro_bus_errno_set(CORO_BUS_ERR_NONE);
            return i;
        }
    }

    int old_count = bus->channel_count;
    int new_count = old_count + 1;
    struct coro_bus_channel **new_array = (struct coro_bus_channel **)malloc(sizeof(*new_array) * new_count);
    assert(new_array != NULL);
    for (int i = 0; i < old_count; ++i)
        new_array[i] = bus->channels[i];
    new_array[old_count] = NULL; 

    free(bus->channels);
    bus->channels = new_array;
    bus->channel_count = new_count;

    struct coro_bus_channel *chan = (struct coro_bus_channel *)malloc(sizeof(*chan));
    assert(chan != NULL);
    chan->size_limit = size_limit;
    rlist_create(&chan->send_queue.coros);
    rlist_create(&chan->recv_queue.coros);

    bus->channels[old_count] = chan;
    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return old_count;
}


void
coro_bus_channel_close(struct coro_bus *bus, int channel)
{
    if (bus == NULL || channel < 0 || channel >= bus->channel_count)
        return;

    struct coro_bus_channel *chan = bus->channels[channel];
    if (chan == NULL)
        return;

    while (!rlist_empty(&chan->send_queue.coros)) {
        struct wakeup_entry *entry = rlist_shift_entry(&chan->send_queue.coros, struct wakeup_entry, base);
        coro_wakeup(entry->coro);
    }

    while (!rlist_empty(&chan->recv_queue.coros)) {
        struct wakeup_entry *entry = rlist_shift_entry(&chan->recv_queue.coros, struct wakeup_entry, base);
        coro_wakeup(entry->coro);
    }

    free(chan);
    bus->channels[channel] = NULL;

    coro_yield();
}


int
coro_bus_send(struct coro_bus *bus, int channel, unsigned data)
{
	while (1) {
		if (bus == NULL || channel < 0 ||
		    channel >= bus->channel_count ||
		    bus->channels[channel] == NULL) {
			coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
			return -1;
		}

		struct coro_bus_channel *ch = bus->channels[channel];

		if (ch->data.size() < ch->size_limit) {
			ch->data.push_back(data);

			wakeup_queue_wakeup_first(&ch->recv_queue);

			coro_bus_errno_set(CORO_BUS_ERR_NONE);
			return 0;
		}

		wakeup_queue_suspend_this(&ch->send_queue);
	}
}


int
coro_bus_try_send(struct coro_bus *bus, int channel, unsigned data)
{
	if (bus == NULL || channel < 0 || channel >= bus->channel_count) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	struct coro_bus_channel *chan = bus->channels[channel];
	if (chan == NULL) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	if (chan->data.size() >= chan->size_limit) {
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	chan->data.push_back(data);

	if (!rlist_empty(&chan->recv_queue.coros)) {
		struct wakeup_entry *entry =
			rlist_first_entry(&chan->recv_queue.coros,
				struct wakeup_entry, base);
		coro_wakeup(entry->coro);
	}

	coro_bus_errno_set(CORO_BUS_ERR_NONE);
	return 0;
}


int
coro_bus_recv(struct coro_bus *bus, int channel, unsigned *data)
{
	while (1) {
		if (bus == NULL || channel < 0 ||
		    channel >= bus->channel_count ||
		    bus->channels[channel] == NULL) {
			coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
			return -1;
		}

		struct coro_bus_channel *ch = bus->channels[channel];

		if (!ch->data.empty()) {
			*data = ch->data.front();
			ch->data.pop_front();

			wakeup_queue_wakeup_first(&ch->send_queue);

			coro_bus_errno_set(CORO_BUS_ERR_NONE);
			return 0;
		}

		wakeup_queue_suspend_this(&ch->recv_queue);
	}
}



int
coro_bus_try_recv(struct coro_bus *bus, int channel, unsigned *data)
{
	if (bus == NULL || channel < 0 || channel >= bus->channel_count ||
	    bus->channels[channel] == NULL) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	struct coro_bus_channel *ch = bus->channels[channel];

	if (ch->data.empty()) {
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	*data = ch->data.front();
	ch->data.pop_front();

	wakeup_queue_wakeup_first(&ch->send_queue);

	coro_bus_errno_set(CORO_BUS_ERR_NONE);
	return 0;
}



#if NEED_BROADCAST

int
coro_bus_broadcast(struct coro_bus *bus, unsigned data)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)bus;
	(void)data;
	coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
	return -1;
}

int
coro_bus_try_broadcast(struct coro_bus *bus, unsigned data)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)bus;
	(void)data;
	coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
	return -1;
}

#endif

#if NEED_BATCH

int
coro_bus_send_v(struct coro_bus *bus, int channel, const unsigned *data, unsigned count)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)bus;
	(void)channel;
	(void)data;
	(void)count;
	coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
	return -1;
}

int
coro_bus_try_send_v(struct coro_bus *bus, int channel, const unsigned *data, unsigned count)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)bus;
	(void)channel;
	(void)data;
	(void)count;
	coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
	return -1;
}

int
coro_bus_recv_v(struct coro_bus *bus, int channel, unsigned *data, unsigned capacity)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)bus;
	(void)channel;
	(void)data;
	(void)capacity;
	coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
	return -1;
}

int
coro_bus_try_recv_v(struct coro_bus *bus, int channel, unsigned *data, unsigned capacity)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)bus;
	(void)channel;
	(void)data;
	(void)capacity;
	coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
	return -1;
}

#endif
