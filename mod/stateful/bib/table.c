#include "nat64/mod/stateful/bib/table.h"
#include <net/ipv6.h>
#include "nat64/common/constants.h"
#include "nat64/common/str_utils.h"
#include "nat64/mod/common/rbtree.h"


/** Cache for struct bib_entrys, for efficient allocation. */
static struct kmem_cache *entry_cache;

int bibentry_init(void)
{
	entry_cache = kmem_cache_create("jool_bib_entries",
			sizeof(struct bib_entry), 0, 0, NULL);
	if (!entry_cache) {
		log_err("Could not allocate the BIB entry cache.");
		return -ENOMEM;
	}

	return 0;
}

void bibentry_destroy(void)
{
	kmem_cache_destroy(entry_cache);
}

/**
 * Allocates and initializes a BIB entry.
 * The entry is generated in dynamic memory; remember to bibentry_put() or pass
 * it along.
 */
struct bib_entry *bibentry_create(const struct ipv4_transport_addr *addr4,
		const struct ipv6_transport_addr *addr6,
		const bool is_static, const l4_protocol proto)
{
	struct bib_entry tmp = {
			.ipv4 = *addr4,
			.ipv6 = *addr6,
			.l4_proto = proto,
			.is_static = is_static,
	};

	struct bib_entry *result = kmem_cache_alloc(entry_cache, GFP_ATOMIC);
	if (!result)
		return NULL;

	memcpy(result, &tmp, sizeof(tmp));
	kref_init(&result->refcounter);
	result->table = NULL;
	RB_CLEAR_NODE(&result->tree6_hook);
	RB_CLEAR_NODE(&result->tree4_hook);
	result->host4_addr = NULL;

	return result;
}

struct bib_entry *bibentry_create_usr(struct bib_entry_usr *usr)
{
	return bibentry_create(&usr->addr4, &usr->addr6, usr->is_static,
			usr->l4_proto);
}

void bibentry_get(struct bib_entry *bib)
{
	kref_get(&bib->refcounter);
}

static void release(struct kref *ref)
{
	struct bib_entry *bib;
	bib = container_of(ref, typeof(*bib), refcounter);

	if (bib->table)
		bibtable_rm(bib->table, bib);

	kmem_cache_free(entry_cache, bib);
}

/**
 * bibentry_put - Decreases @bib's refcounter and kills it if no more references
 * remain.
 *
 * @must_die: If you know @bib is supposed to die during this put, send true.
 * Will drop a stack trace in the kernel logs if it doesn't die.
 * true = "entry MUST die." false = "entry might or might not die."
 */
void bibentry_put(struct bib_entry *bib, bool must_die)
{
	bool dead = kref_put(&bib->refcounter, release);
	WARN(must_die && !dead, "BIB entry did not die!");
}

bool bibentry_equals(const struct bib_entry *b1, const struct bib_entry *b2)
{
	return taddr4_equals(&b1->ipv4, &b2->ipv4)
			&& taddr6_equals(&b1->ipv6, &b2->ipv6)
			&& (b1->l4_proto == b2->l4_proto);
}

void bibentry_log(const struct bib_entry *bib, const char *action)
{
	struct timeval tval;
	struct tm t;

	do_gettimeofday(&tval);
	time_to_tm(tval.tv_sec, 0, &t);
	log_info("%ld/%d/%d %d:%d:%d (GMT) - %s %pI6c#%u to %pI4#%u (%s)",
			1900 + t.tm_year, t.tm_mon + 1, t.tm_mday,
			t.tm_hour, t.tm_min, t.tm_sec, action,
			&bib->ipv6.l3, bib->ipv6.l4,
			&bib->ipv4.l3, bib->ipv4.l4,
			l4proto_to_string(bib->l4_proto));
}

void bibtable_init(struct bib_table *table)
{
	table->tree6 = RB_ROOT;
	table->tree4 = RB_ROOT;
	table->count = 0;
	spin_lock_init(&table->lock);
	table->log_changes = DEFAULT_BIB_LOGGING;
}

static void destroy_aux(struct rb_node *node)
{
	struct bib_entry *bib;
	bib = rb_entry(node, typeof(*bib), tree6_hook);
	kmem_cache_free(entry_cache, bib);
}

void bibtable_destroy(struct bib_table *table)
{
	/*
	 * The values need to be released only in one of the trees
	 * because both trees point to the same values.
	 */
	rbtree_clear(&table->tree6, destroy_aux);
}

void bibtable_config_clone(struct bib_table *table, struct bib_config *config)
{
	spin_lock_bh(&table->lock);
	config->log_changes = table->log_changes;
	spin_unlock_bh(&table->lock);
}

void bibtable_config_set(struct bib_table *table, struct bib_config *config)
{
	spin_lock_bh(&table->lock);
	table->log_changes = config->log_changes;
	spin_unlock_bh(&table->lock);
}

/**
 * Returns > 0 if bib->ipv6.l3 > addr.
 * Returns < 0 if bib->ipv6.l3 < addr.
 * Returns 0 if bib->ipv6.l3 == addr.
 */
static int compare_addr6(const struct bib_entry *bib,
		const struct in6_addr *addr)
{
	return ipv6_addr_cmp(&bib->ipv6.l3, addr);
}

/**
 * Returns > 0 if bib->ipv6 > addr.
 * Returns < 0 if bib->ipv6 < addr.
 * Returns 0 if bib->ipv6 == addr.
 */
static int compare_full6(const struct bib_entry *bib,
		const struct ipv6_transport_addr *addr)
{
	int gap;

	gap = compare_addr6(bib, &addr->l3);
	if (gap)
		return gap;

	gap = bib->ipv6.l4 - addr->l4;
	return gap;
}

/**
 * Returns > 0 if bib->ipv4.l3 > addr.
 * Returns < 0 if bib->ipv4.l3 < addr.
 * Returns zero if bib->ipv4.l3 == addr.
 */
static int compare_addr4(const struct bib_entry *bib,
		const struct in_addr *addr)
{
	return ipv4_addr_cmp(&bib->ipv4.l3, addr);
}

/**
 * Returns > 0 if bib->ipv4 > addr.
 * Returns < 0 if bib->ipv4 < addr.
 * Returns 0 if bib->ipv4 == addr.
 */
static int compare_full4(const struct bib_entry *bib,
		const struct ipv4_transport_addr *addr)
{
	int gap;

	gap = compare_addr4(bib, &addr->l3);
	if (gap)
		return gap;

	gap = bib->ipv4.l4 - addr->l4;
	return gap;
}

static struct bib_entry *find_by_addr6(const struct bib_table *table,
		const struct ipv6_transport_addr *addr)
{
	return rbtree_find(addr, &table->tree6, compare_full6, struct bib_entry,
			tree6_hook);
}

static struct bib_entry *find_by_addr4(const struct bib_table *table,
		const struct ipv4_transport_addr *addr)
{
	return rbtree_find(addr, &table->tree4, compare_full4, struct bib_entry,
			tree4_hook);
}

int bibtable_get6(struct bib_table *table,
		const struct ipv6_transport_addr *addr,
		struct bib_entry **result)
{
	spin_lock_bh(&table->lock);
	*result = find_by_addr6(table, addr);
	if (*result)
		bibentry_get(*result);
	spin_unlock_bh(&table->lock);

	return (*result) ? 0 : -ESRCH;
}

int bibtable_get4(struct bib_table *table,
		const struct ipv4_transport_addr *addr,
		struct bib_entry **result)
{
	spin_lock_bh(&table->lock);
	*result = find_by_addr4(table, addr);
	if (*result)
		bibentry_get(*result);
	spin_unlock_bh(&table->lock);

	return (*result) ? 0 : -ESRCH;
}

bool bibtable_contains4(struct bib_table *table,
		const struct ipv4_transport_addr *addr)
{
	bool result;

	spin_lock_bh(&table->lock);
	result = find_by_addr4(table, addr) ? true : false;
	spin_unlock_bh(&table->lock);

	return result;
}

static struct bib_entry *add6(struct bib_table *table, struct bib_entry *bib)
{
	return rbtree_add(bib, &bib->ipv6, &table->tree6, compare_full6,
			struct bib_entry, tree6_hook);
}

static struct bib_entry *add4(struct bib_table *table, struct bib_entry *bib)
{
	return rbtree_add(bib, &bib->ipv4, &table->tree4, compare_full4,
			struct bib_entry, tree4_hook);
}

int bibtable_add(struct bib_table *table, struct bib_entry *bib,
		struct bib_entry **old)
{
	struct bib_entry *collision;

	spin_lock_bh(&table->lock);

	collision = add6(table, bib);
	if (collision) {
		log_debug("IPv6 index failed.");
		goto exists;
	}

	collision = add4(table, bib);
	if (collision) {
		rb_erase(&bib->tree6_hook, &table->tree6);
		log_debug("IPv4 index failed.");
		goto exists;
	}

	/*
	 * Note: Because of the way bibentry_put() works, bib->table MUST only
	 * be assigned when success is imminent. Please consider that if you
	 * need to edit this function.
	 */
	bib->table = table;
	table->count++;

	spin_unlock_bh(&table->lock);
	if (table->log_changes)
		bibentry_log(bib, "Mapped");
	return 0;

exists:
	if (old) {
		bibentry_get(collision);
		*old = collision;
	}
	spin_unlock_bh(&table->lock);
	return -EEXIST;
}

/**
 * Spinlock must be held.
 */
static void rm(struct bib_table *table, struct bib_entry *bib)
{
	if (!WARN(RB_EMPTY_NODE(&bib->tree6_hook), "Faulty IPv6 index"))
		rb_erase(&bib->tree6_hook, &table->tree6);
	if (!WARN(RB_EMPTY_NODE(&bib->tree4_hook), "Faulty IPv4 index"))
		rb_erase(&bib->tree4_hook, &table->tree4);
	table->count--;

	if (table->log_changes)
		bibentry_log(bib, "Forgot");
}

void bibtable_rm(struct bib_table *table, struct bib_entry *bib)
{
	spin_lock_bh(&table->lock);
	rm(table, bib);
	spin_unlock_bh(&table->lock);
}

static struct rb_node *find_starting_point(struct bib_table *table,
		const struct ipv4_transport_addr *offset, bool include_offset)
{
	struct bib_entry *bib;
	struct rb_node **node;
	struct rb_node *parent;

	/* If there's no offset, start from the beginning. */
	if (!offset)
		return rb_first(&table->tree4);

	/* If offset is found, start from offset or offset's next. */
	rbtree_find_node(offset, &table->tree4, compare_full4, struct bib_entry,
			tree4_hook, parent, node);
	if (*node)
		return include_offset ? (*node) : rb_next(*node);

	if (!parent)
		return NULL;

	/*
	 * If offset is not found, start from offset's next anyway.
	 * (If offset was meant to exist, it probably timed out and died while
	 * the caller wasn't holding the spinlock; it's nothing to worry about.)
	 */
	bib = rb_entry(parent, struct bib_entry, tree4_hook);
	return (compare_full4(bib, offset) < 0) ? rb_next(parent) : parent;
}

/**
 * The iteration is "safe"; it doesn't die if func() removes and/or deletes the
 * entry.
 */
static int __foreach(struct bib_table *table,
		int (*func)(struct bib_entry *, void *), void *arg,
		const struct ipv4_transport_addr *offset, bool include_offset)
{
	struct rb_node *node, *next;
	int error = 0;
	spin_lock_bh(&table->lock);

	node = find_starting_point(table, offset, include_offset);
	for (; node && !error; node = next) {
		next = rb_next(node);
		error = func(rb_entry(node, struct bib_entry, tree4_hook), arg);
	}

	spin_unlock_bh(&table->lock);
	return error;
}

int bibtable_foreach(struct bib_table *table,
		int (*func)(struct bib_entry *, void *), void *arg,
		const struct ipv4_transport_addr *offset)
{
	return __foreach(table, func, arg, offset, false);
}

int bibtable_count(struct bib_table *table, __u64 *result)
{
	spin_lock_bh(&table->lock);
	*result = table->count;
	spin_unlock_bh(&table->lock);
	return 0;
}

struct iteration_args {
	struct bib_table *table;
	const struct ipv4_prefix *prefix;
	const struct port_range *ports;
	unsigned int deleted;
};

static void release_locked(struct kref *ref)
{
	struct bib_entry *bib;
	bib = container_of(ref, typeof(*bib), refcounter);

	if (bib->table)
		rm(bib->table, bib);

	kmem_cache_free(entry_cache, bib);
}

static int __flush(struct bib_entry *bib, void *void_args)
{
	struct iteration_args *args = void_args;

	/*
	 * All we need to do is remove the fake user.
	 * Otherwise we might free entries being actively pointed by sessions.
	 */
	if (bib->is_static)
		args->deleted += kref_put(&bib->refcounter, release_locked);

	return 0;
}

void bibtable_flush(struct bib_table *table)
{
	struct iteration_args args = {
			.table = table,
			.deleted = 0,
	};

	__foreach(table, __flush, &args, NULL, 0);
	log_debug("Deleted %u BIB entries.", args.deleted);
}

static int __delete_taddr4s(struct bib_entry *bib, void *void_args)
{
	struct iteration_args *args = void_args;

	if (!prefix4_contains(args->prefix, &bib->ipv4.l3))
		return 1; /* positive = break iteration early, not an error. */
	if (!port_range_contains(args->ports, bib->ipv4.l4))
		return 0;

	return __flush(bib, void_args);
}

void bibtable_delete_taddr4s(struct bib_table *table,
		const struct ipv4_prefix *prefix, struct port_range *ports)
{
	struct iteration_args args = {
			.table = table,
			.prefix = prefix,
			.ports = ports,
			.deleted = 0,
	};
	struct ipv4_transport_addr offset = {
			.l3 = prefix->address,
			.l4 = ports->min,
	};

	__foreach(table, __delete_taddr4s, &args, &offset, true);
	log_debug("Deleted %u BIB entries.", args.deleted);
}
