#include "nat64/mod/stateful/session/table.h"

#include <net/ipv6.h>
#include "nat64/common/constants.h"
#include "nat64/mod/common/rbtree.h"
#include "nat64/mod/stateful/session/pkt_queue.h"

void send_probe_packet(struct session_entry *session);

/**
 * Called once in a while to kick off the scheduled expired sessions massacre.
 *
 * In that sense, it's a public function, so it requires spinlocks to NOT be held.
 */
static void cleaner_timer(unsigned long param)
{
	struct expire_timer *expirer = (struct expire_timer *) param;
	unsigned long timeout;
	struct session_entry *session, *tmp;
	struct list_head expires, probes;

	log_debug("===============================================");
	log_debug("Handling expired sessions...");

	timeout = expirer->get_timeout();
	INIT_LIST_HEAD(&expires);
	INIT_LIST_HEAD(&probes);

	spin_lock_bh(&expirer->table->lock);
	list_for_each_entry_safe(session, tmp, &expirer->sessions, list_hook) {
		/*
		 * "list" is sorted by expiration date,
		 * so stop on the first unexpired session.
		 */
		if (time_before(jiffies, session->update_time + timeout))
			break;

		expirer->callback(session, &expires, &probes);
	}
	spin_unlock_bh(&expirer->table->lock);

	list_for_each_entry_safe(session, tmp, &probes, list_hook) {
		send_probe_packet(session);
		session_return(session);
	}
	list_for_each_entry_safe(session, tmp, &expires, list_hook) {
		session_return(session);
	}

	sessiontable_update_timers(expirer->table);
}

static void init_expirer(struct expire_timer *expirer,
		timeout_fn timeout_fn, expire_fn callback_fn,
		struct session_table *table)
{
	init_timer(&expirer->timer);
	expirer->timer.function = cleaner_timer;
	expirer->timer.expires = 0;
	expirer->timer.data = (unsigned long) expirer;
	INIT_LIST_HEAD(&expirer->sessions);
	expirer->get_timeout = timeout_fn;
	expirer->callback = callback_fn;
	expirer->table = table;
}

void sessiontable_init(struct session_table *table,
		timeout_fn est_timeout, expire_fn est_callback,
		timeout_fn trans_timeout, expire_fn trans_callback)
{
	table->tree6 = RB_ROOT;
	table->tree4 = RB_ROOT;
	table->count = 0;
	init_expirer(&table->est_timer, est_timeout, est_callback, table);
	init_expirer(&table->trans_timer, trans_timeout, trans_callback, table);
	spin_lock_init(&table->lock);
}

/**
 * Auxiliar for sessiondb_destroy(). Wraps the destruction of a session,
 * exposing an API the rbtree module wants.
 *
 * Doesn't care about spinlocks (destructor code doesn't share threads).
 */
static void __destroy_aux(struct rb_node *node)
{
	session_return(rb_entry(node, struct session_entry, tree6_hook));
}

void sessiontable_destroy(struct session_table *table)
{
	del_timer_sync(&table->est_timer.timer);
	del_timer_sync(&table->trans_timer.timer);
	/*
	 * The values need to be released only in one of the trees
	 * because both trees point to the same values.
	 */
	rbtree_clear(&table->tree6, __destroy_aux);
}

static int compare_addr6(const struct ipv6_transport_addr *a1,
		const struct ipv6_transport_addr *a2)
{
	int gap;

	gap = ipv6_addr_cmp(&a1->l3, &a2->l3);
	if (gap)
		return gap;

	gap = a1->l4 - a2->l4;
	return gap;
}

static int compare_session6(const struct session_entry *s1,
		const struct session_entry *s2)
{
	int gap;

	gap = compare_addr6(&s1->local6, &s2->local6);
	if (gap)
		return gap;

	gap = compare_addr6(&s1->remote6, &s2->remote6);
	return gap;
}

/**
 * Returns > 0 if session.*6 > tuple6.*.addr6.
 * Returns < 0 integer if session.*6 < tuple6.*.addr6.
 * Returns 0 if session.*6 == tuple6.*.addr6.
 *
 * Doesn't care about spinlocks.
 */
static int compare_full6(const struct session_entry *session,
		const struct tuple *tuple6)
{
	int gap;

	gap = compare_addr6(&session->local6, &tuple6->dst.addr6);
	if (gap)
		return gap;

	gap = compare_addr6(&session->remote6, &tuple6->src.addr6);
	return gap;
}

static int compare_addr4(const struct ipv4_transport_addr *a1,
		const struct ipv4_transport_addr *a2)
{
	int gap;

	gap = ipv4_addr_cmp(&a1->l3, &a2->l3);
	if (gap)
		return gap;

	gap = a1->l4 - a2->l4;
	return gap;
}

static int compare_session4(const struct session_entry *s1,
		const struct session_entry *s2)
{
	int gap;

	gap = compare_addr4(&s1->local4, &s2->local4);
	if (gap)
		return gap;

	gap = compare_addr4(&s1->remote4, &s2->remote4);
	return gap;
}

/**
 * Returns > 0 if session.*4 > tuple4.*.addr4.
 * Returns < 0 if session.*4 < tuple4.*.addr4.
 * Returns 0 if session.*4 == tuple4.*.addr4.
 *
 * It excludes remote layer-4 IDs from the comparison.
 * See sessiondb_allow() to find out why.
 *
 * Doesn't care about spinlocks.
 */
static int compare_addrs4(const struct session_entry *session,
		const struct tuple *tuple4)
{
	int gap;

	gap = compare_addr4(&session->local4, &tuple4->dst.addr4);
	if (gap)
		return gap;

	gap = ipv4_addr_cmp(&session->remote4.l3, &tuple4->src.addr4.l3);
	return gap;
}

/**
 * Returns > 0 if session.*4 > tuple4.*.addr4.
 * Returns < 0 if session.*4 < tuple4.*.addr4.
 * Returns 0 if session.*4 == tuple4.*.addr4.
 *
 * Doesn't care about spinlocks.
 */
static int compare_full4(const struct session_entry *session,
		const struct tuple *tuple4)
{
	int gap;

	gap = compare_addr4(&session->local4, &tuple4->dst.addr4);
	if (gap)
		return gap;

	gap = compare_addr4(&session->remote4, &tuple4->src.addr4);
	return gap;
}

/**
 * Removes all of this database's references towards "session", and drops its
 * refcount accordingly.
 *
 * The only thing it doesn't do is decrement count of "session"'s table! I do
 * that outside because I always want to add up and report that number.
 *
 * @return number of sessions removed from the database. This is always 1,
 *		because I have no way to know if the removal failed
 *		(and it shouldn't be possible anyway).
 *
 * "table"'s spinlock must already be held.
 *
 * TODO add the removal list as an argument?
 * TODO return value seems pointless now too.
 */
static int remove(struct session_table *table, struct session_entry *session)
{
	if (!RB_EMPTY_NODE(&session->tree6_hook))
		rb_erase(&session->tree6_hook, &table->tree6);
	if (!RB_EMPTY_NODE(&session->tree4_hook))
		rb_erase(&session->tree4_hook, &table->tree4);
	list_del(&session->list_hook);
	session->expirer = NULL;

	/* TODO */
//	session_return(session);
	session_log(session, "Forgot session");
	return 1;
}

/**
 * Wrapper for mod_timer().
 *
 * Not holding a spinlock is desirable for performance reasons (mod_timer()
 * syncs itself).
 */
static void schedule_timer(struct expire_timer *expirer, unsigned long next_time)
{
	unsigned long min_next = jiffies + MIN_TIMER_SLEEP;

	if (time_before(next_time, min_next))
		next_time = min_next;

	mod_timer(&expirer->timer, next_time);
	log_debug("Timer will awake in %u msecs.",
			jiffies_to_msecs(expirer->timer.expires - jiffies));
}

int sessiontable_get_timeout(struct session_entry *session,
		unsigned long *result)
{
	if (!session->expirer) {
		log_debug("The session entry doesn't have an expirer");
		return -EINVAL;
	}

	*result = session->expirer->get_timeout();
	return 0;
}

/**
 * Helper of the set_*_timer functions. Safely updates "session"->dying_time
 * using "ttl" and moves it from its original location to the end of "list".
 */
static struct expire_timer *set_timer(struct session_entry *session,
		struct expire_timer *expirer)
{
	struct expire_timer *result;

	session->update_time = jiffies;
	list_del(&session->list_hook);
	list_add_tail(&session->list_hook, &expirer->sessions);
	session->expirer = expirer;

	/*
	 * The new session is always going to expire last.
	 * So if the timer is already set, there should be no reason to edit it.
	 */
	result = timer_pending(&expirer->timer) ? NULL : expirer;

	return result;
}

static void commit_timer(struct expire_timer *expirer)
{
	if (expirer)
		schedule_timer(expirer, jiffies + expirer->get_timeout());
}

static struct session_entry *get_by_ipv6(struct session_table *table,
		struct tuple *tuple)
{
	return rbtree_find(tuple, &table->tree6, compare_full6,
			struct session_entry, tree6_hook);
}

static struct session_entry *get_by_ipv4(struct session_table *table,
		struct tuple *tuple)
{
	return rbtree_find(tuple, &table->tree4, compare_full4,
			struct session_entry, tree4_hook);
}

/* TODO remember to not store SYN sessions. */
int sessiontable_get(struct session_table *table, struct tuple *tuple,
		struct session_entry **result)
{
	struct session_entry *session;

	spin_lock_bh(&table->lock);

	switch (tuple->l3_proto) {
	case L3PROTO_IPV6:
		session = get_by_ipv6(table, tuple);
		break;
	case L3PROTO_IPV4:
		session = get_by_ipv4(table, tuple);
		break;
	default:
		WARN(true, "Unsupported network protocol: %u", tuple->l3_proto);
		spin_unlock_bh(&table->lock);
		return -EINVAL;
	}

	if (session)
		session_get(session);

	spin_unlock_bh(&table->lock);

	if (!session)
		return -ESRCH;

	*result = session;
	return 0;
}

bool sessiontable_allow(struct session_table *table, struct tuple *tuple4)
{
	struct session_entry *session;
	bool result;

	spin_lock_bh(&table->lock);
	session = rbtree_find(tuple4, &table->tree4, compare_addrs4, struct session_entry, tree4_hook);
	result = session ? true : false;
	spin_unlock_bh(&table->lock);

	return result;
}

static int add6(struct session_table *table, struct session_entry *session)
{
	return rbtree_add(session, session, &table->tree6, compare_session6,
			struct session_entry, tree6_hook);
}

static int add4(struct session_table *table, struct session_entry *session)
{
	return rbtree_add(session, session, &table->tree4, compare_session4,
			struct session_entry, tree4_hook);
}

int sessiontable_add(struct session_table *table, struct session_entry *session,
		bool is_established)
{
	struct expire_timer *expirer;
	int error;

	error = pktqueue_remove(session);
	if (error)
		return error;

	expirer = is_established ? &table->est_timer : &table->trans_timer;

	spin_lock_bh(&table->lock);

	error = add6(table, session);
	if (error) {
		spin_unlock_bh(&table->lock);
		return error;
	}

	error = add4(table, session);
	if (error) {
		rb_erase(&session->tree6_hook, &table->tree6);
		spin_unlock_bh(&table->lock);
		return error;
	}


	expirer = set_timer(session, expirer);
	session_get(session); /* Database's references. */
	table->count++;

	spin_unlock_bh(&table->lock);

	session_log(session, "Added session");
	commit_timer(expirer);

	return 0;
}

/**
 * See the function of the same name from the BIB DB module for comments on this.
 *
 * Requires "table"'s spinlock to already be held.
 */
static struct rb_node *find_next_chunk(struct session_table *table,
		const struct ipv4_transport_addr *offset_remote,
		const struct ipv4_transport_addr *offset_local)
{
	struct rb_node **node, *parent;
	struct session_entry *session;
	struct tuple tmp;

	if (!offset_remote || !offset_local)
		return rb_first(&table->tree4);

	tmp.src.addr4 = *offset_remote;
	tmp.dst.addr4 = *offset_local;
	/* the protos are not needed. */
	rbtree_find_node(&tmp, &table->tree4, compare_full4,
			struct session_entry, tree4_hook, parent, node);
	if (*node)
		return rb_next(*node);

	session = rb_entry(parent, struct session_entry, tree4_hook);
	return (compare_full4(session, &tmp) < 0) ? parent : rb_next(parent);
}

static void reschedule(struct expire_timer *expirer)
{
	bool reschedule = false;
	struct session_entry *session;
	unsigned long death_time;

	spin_lock_bh(&expirer->table->lock);
	if (!list_empty(&expirer->sessions)) {
		reschedule = true;
		session = list_entry(expirer->sessions.next, struct session_entry, list_hook);
		death_time = session->update_time + expirer->get_timeout();
	}
	spin_unlock_bh(&expirer->table->lock);

	if (reschedule)
		schedule_timer(expirer, death_time);
}

void sessiontable_update_timers(struct session_table *table)
{
	reschedule(&table->est_timer);
	reschedule(&table->trans_timer);
}

int sessiontable_foreach(struct session_table *table,
		int (*func)(struct session_entry *, void *), void *arg,
		const struct ipv4_transport_addr *offset_remote,
		const struct ipv4_transport_addr *offset_local)
{
	struct rb_node *node, *next;
	struct session_entry *session;
	int error = 0;
	spin_lock_bh(&table->lock);

	node = find_next_chunk(table, offset_remote, offset_local);
	for (; node && !error; node = next) {
		next = rb_next(node);
		session = rb_entry(node, struct session_entry, tree4_hook);
		error = func(session, arg);
	}

	spin_unlock_bh(&table->lock);
	return error;
}

int sessiontable_count(struct session_table *table, __u64 *result)
{
	spin_lock_bh(&table->lock);
	*result = table->count;
	spin_unlock_bh(&table->lock);
	return 0;
}

static void delete(struct list_head *sessions)
{
	struct session_entry *session;
	unsigned long s = 0;

	while (!list_empty(sessions)) {
		session = list_entry(sessions->next, typeof(*session),
				list_hook);
		list_del(&session->list_hook);
		session_return(session);
		s++;
	}

	log_debug("Deleted %lu entries.", s);
}

struct bib_remove_args {
	struct session_table *table;
	const struct ipv4_transport_addr *addr4;
	struct list_head removed;
};

static int __remove_by_bib(struct session_entry *session, void *args_void)
{
	struct bib_remove_args *args = args_void;

	if (!ipv4_transport_addr_equals(args->addr4, &session->local4))
		return 1; /* positive = break iteration early, no error. */

	remove(args->table, session);
	list_add(&session->list_hook, &args->removed);
	return 0;
}

int sessiontable_delete_by_bib(struct session_table *table,
		struct bib_entry *bib)
{
	struct bib_remove_args args = {
			.table = table,
			.addr4 = &bib->ipv4,
			.removed = LIST_HEAD_INIT(args.removed),
	};
	struct ipv4_transport_addr remote = {
			.l3.s_addr = 0,
			.l4 = 0,
	};
	int error;

	error = sessiontable_foreach(table, __remove_by_bib, &args,
			&remote, &bib->ipv4);
	delete(&args.removed);
	return error;
}

struct prefix4_remove_args {
	struct session_table *table;
	const struct ipv4_prefix *prefix;
	struct list_head removed;
};

static int __remove_by_prefix4(struct session_entry *session, void *args_void)
{
	struct prefix4_remove_args *args = args_void;

	if (!prefix4_contains(args->prefix, &session->local4.l3))
		return 1; /* positive = break iteration early, no error. */

	remove(args->table, session);
	list_add(&session->list_hook, &args->removed);
	return 0;
}

/**
 * Deletes the sessions from the "table" table whose local IPv4 address is "addr".
 * This function is awfully similar to sessiondb_delete_by_bib(). See that for more comments.
 */
int sessiontable_delete_by_prefix4(struct session_table *table,
		struct ipv4_prefix *prefix)
{
	struct prefix4_remove_args args = {
			.table = table,
			.prefix = prefix,
			.removed = LIST_HEAD_INIT(args.removed),
	};
	struct ipv4_transport_addr remote = {
			.l3.s_addr = 0,
			.l4 = 0,
	};
	struct ipv4_transport_addr local = {
			.l3 = prefix->address,
			.l4 = 0,
	};
	int error;

	error = sessiontable_foreach(table, __remove_by_prefix4, &args,
			&remote, &local);
	delete(&args.removed);
	return error;
}

struct flush_args {
	struct session_table *table;
	struct list_head removed;
};

static int __flush(struct session_entry *session, void *args_void)
{
	struct prefix4_remove_args *args = args_void;
	remove(args->table, session);
	list_add(&session->list_hook, &args->removed);
	return 0;
}

int sessiontable_flush(struct session_table *table)
{
	struct flush_args args = {
			.table = table,
			.removed = LIST_HEAD_INIT(args.removed),
	};
	int error;

	error = sessiontable_foreach(table, __flush, &args, NULL, NULL);
	delete(&args.removed);
	return error;
}
