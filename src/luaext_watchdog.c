/*
 * luaext — the deadline watchdog.
 *
 * PURITY RULE, enforced structurally, by tools/check-watchdog-purity.sh and by
 * a CI job: this file includes neither php.h nor lua.h, and must not gain
 * either. The watchdog runs on a thread PHP did not create and that has no TSRM
 * context, so a LUAEXT_G() from here would silently read another thread's
 * globals rather than failing. Keeping those headers out of scope makes the
 * mistake unwritable. It is also why the pool below is plain malloc() rather
 * than pemalloc(): the honest reason is that pemalloc IS malloc for persistent
 * allocations, and reaching for it would mean including the header this file
 * exists to exclude.
 *
 * THE ONE INVARIANT WORTH STATING UP FRONT: the deadline heap is a HINT, never
 * an authority. An entry says "this slot might be over budget by now"; whether
 * it actually is gets decided by re-reading the clocks under the slot's own lock
 * at the moment the entry surfaces. That is what lets the owning thread publish
 * a deadline without holding its slot lock across the watchdog lock, and it is
 * what makes a stale entry harmless rather than a source of early trips.
 *
 * Lock order is watchdog.lock -> slot.lock, never reversed. The pool lock is a
 * leaf: nothing else is ever taken while it is held. luaext_watchdog_release()
 * deliberately takes NEITHER the watchdog lock nor the pool lock in the same
 * breath as a slot lock, because it runs on every sandbox teardown and making
 * that path contend for a process-wide lock would be a real bottleneck under a
 * worker SAPI. Stale heap entries are reaped lazily instead, by the generation
 * check that every pop performs.
 */

#include "luaext_watchdog.h"

#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Tuning
 * ---------------------------------------------------------------------- */

/* Slots per pool block. The store is never freed before shutdown (see the
 * header), so this only trades a little address space against the number of
 * malloc calls a busy worker makes. */
#define LUAEXT_WATCH_BLOCK_SLOTS 64

/*
 * How many count-hook ticks pass between in-VM clock self-checks.
 *
 * The hook itself fires every luaext.hook_count instructions, and reading a
 * thread CPU clock is a syscall on most platforms; doing it on every tick would
 * dominate the cost of running Lua at all. Striding on top of the hook keeps
 * the self-check at roughly one clock read per hook_count * this instructions,
 * which at the defaults is well under a millisecond of real time.
 */
#define LUAEXT_WATCH_SELF_CHECK_STRIDE 64u

/*
 * A CPU limit finer than this many clock ticks cannot be measured meaningfully,
 * whatever the platform claims. Below it the limit is DEGRADED and gets a
 * wall-clock companion deadline, so a script still stops -- it just stops on a
 * quantity the host did not ask for, which is why features() and the error class
 * both have to say CPU rather than pretend.
 */
#define LUAEXT_WATCH_MIN_TICKS UINT64_C(20)

/* Default floor on wake-ups, overridden from luaext.watchdog_resolution_us. */
#define LUAEXT_WATCH_DEFAULT_FLOOR_NS UINT64_C(500000)

#define LUAEXT_WATCH_BOTH ((uint8_t)(LUAEXT_WATCH_CPU | LUAEXT_WATCH_WALL))

/* -------------------------------------------------------------------------
 * Slots
 * ---------------------------------------------------------------------- */

struct luaext_watch_slot {
	luaext_mutex lock;

	/*
	 * Bumped on acquire AND on release, so a heap entry captured under one
	 * tenancy can never be mistaken for a live one under the next. Combined with
	 * "the backing store outlives every reader", this turns a lifetime problem
	 * into a validity problem -- and a validity problem is one a counter solves.
	 */
	uint64_t generation;

	/*
	 * The interrupt flags of the sandbox that owns this slot, and NOTHING else
	 * about it. The watchdog is physically incapable of reaching a zend_object
	 * from here because no PHP type is in scope in this translation unit.
	 */
	luaext_irq *irq;

	luaext_cpu_clock clock;
	bool clock_ok;

	/*
	 * Set when opening the CPU segment could not read the clock, and the reason
	 * it needs its own field rather than being handled where it happens.
	 *
	 * Every consumer of CPU time is gated on LUAEXT_WATCH_CPU being in `open`:
	 * luaext_watch_sample() only accumulates live time for an open segment,
	 * luaext_watch_deadline() only contributes a CPU deadline for one, and --
	 * the trap -- `cpu_lost`, the signal that says the clock has died and the
	 * script must stop, is itself computed inside that same open-guarded branch.
	 * So a segment that fails to OPEN cannot report that it failed: it reads
	 * exactly like a sandbox using no CPU, forever, while features() goes on
	 * claiming LimitSupport::Enforced.
	 *
	 * Recording it here lets luaext_watch_evaluate() treat it identically to
	 * cpu_lost. A CPU limit that cannot be measured must stop the script; it must
	 * never be quietly dropped, which is the exact failure this extension exists
	 * to eliminate.
	 */
	bool cpu_open_failed;

	uint64_t cpu_limit;
	uint64_t wall_limit;

	/*
	 * The degraded companion. A wall-clock budget that reports itself as a CPU
	 * breach, armed when the CPU limit is too fine for this platform's clock.
	 * The host asked for a CPU limit and was told the platform is coarse, so
	 * reporting WallClockLimitError here would answer a question nobody asked.
	 */
	uint64_t cpu_wall_limit;

	/* Closed segments, already billed. */
	uint64_t cpu_used;
	uint64_t wall_used;

	/* Where the currently open segment started. Meaningful only for the bits
	 * set in `open`. */
	uint64_t cpu_base;
	uint64_t wall_base;

	bool armed;
	uint8_t open; /* segments currently measuring */

	/*
	 * Distinguishes a live heap entry from a superseded one. Every publication
	 * takes a fresh epoch under this lock; a pop whose epoch is stale is a
	 * duplicate and is dropped without re-queueing, which is what keeps the heap
	 * to one live entry per slot however often the owner re-publishes.
	 */
	uint64_t epoch;

	/* -----------------------------------------------------------------
	 * Owner-thread-only. The watchdog never reads these, so they need no
	 * lock and are deliberately not atomic: making them atomic would imply
	 * a sharing that does not exist and would cost the hot path.
	 * ----------------------------------------------------------------- */
	uint8_t paused;
	bool has_limits;
	uint32_t hook_ticks;

	struct luaext_watch_slot *free_next;
};

typedef struct luaext_watch_block {
	struct luaext_watch_block *next;
	luaext_watch_slot slots[LUAEXT_WATCH_BLOCK_SLOTS];
} luaext_watch_block;

/* -------------------------------------------------------------------------
 * Process-wide state
 * ---------------------------------------------------------------------- */

typedef struct {
	luaext_watch_slot *slot;
	uint64_t generation;
	uint64_t epoch;
	uint64_t deadline; /* monotonic nanoseconds */
} luaext_watch_entry;

static struct {
	luaext_mutex lock; /* pool lock; a leaf, never held with any other */
	luaext_watch_block *blocks;
	luaext_watch_slot *free_list;
	bool ready;
} luaext_watch_pool;

static struct {
	luaext_mutex lock;
	luaext_cond cond;
	luaext_thread thread;
	luaext_once once;

	luaext_watch_entry *heap;
	size_t heap_count;
	size_t heap_capacity;

	uint64_t floor_ns;

	bool ready;	  /* the lock and condvar exist */
	bool running; /* the thread exists and has not been asked to stop */
	bool failed;  /* creation was attempted and the platform refused */
	bool stop;
} luaext_watch;

/* -------------------------------------------------------------------------
 * The slot pool
 * ---------------------------------------------------------------------- */

static luaext_watch_slot *luaext_watch_pool_take(void)
{
	luaext_watch_slot *slot = NULL;

	if (!luaext_watch_pool.ready) {
		return NULL;
	}

	luaext_mutex_lock(&luaext_watch_pool.lock);

	if (luaext_watch_pool.free_list == NULL) {
		luaext_watch_block *block = (luaext_watch_block *)malloc(sizeof(*block));

		if (block != NULL) {
			size_t index;

			memset(block, 0, sizeof(*block));

			for (index = 0; index < LUAEXT_WATCH_BLOCK_SLOTS; index++) {
				luaext_watch_slot *fresh = &block->slots[index];

				if (!luaext_mutex_init(&fresh->lock)) {
					/* Every slot in a block shares its fate: a partially usable
					 * block would need a per-slot "is this one real" test on
					 * every path that touches one. */
					while (index > 0) {
						luaext_mutex_destroy(&block->slots[--index].lock);
					}

					free(block);
					block = NULL;
					break;
				}

				fresh->free_next = luaext_watch_pool.free_list;
				luaext_watch_pool.free_list = fresh;
			}

			if (block != NULL) {
				block->next = luaext_watch_pool.blocks;
				luaext_watch_pool.blocks = block;
			} else {
				/* Unwind the partial free list this block contributed. */
				luaext_watch_pool.free_list = NULL;
			}
		}
	}

	if (luaext_watch_pool.free_list != NULL) {
		slot = luaext_watch_pool.free_list;
		luaext_watch_pool.free_list = slot->free_next;
		slot->free_next = NULL;
	}

	luaext_mutex_unlock(&luaext_watch_pool.lock);

	return slot;
}

static void luaext_watch_pool_give(luaext_watch_slot *slot)
{
	luaext_mutex_lock(&luaext_watch_pool.lock);
	slot->free_next = luaext_watch_pool.free_list;
	luaext_watch_pool.free_list = slot;
	luaext_mutex_unlock(&luaext_watch_pool.lock);
}

/* -------------------------------------------------------------------------
 * The deadline heap. Caller holds watchdog.lock.
 * ---------------------------------------------------------------------- */

static bool luaext_watch_heap_reserve(void)
{
	size_t capacity;
	luaext_watch_entry *grown;

	if (luaext_watch.heap_count < luaext_watch.heap_capacity) {
		return true;
	}

	capacity = luaext_watch.heap_capacity == 0 ? 32 : luaext_watch.heap_capacity * 2;
	grown = (luaext_watch_entry *)realloc(luaext_watch.heap, capacity * sizeof(*grown));

	if (grown == NULL) {
		return false;
	}

	luaext_watch.heap = grown;
	luaext_watch.heap_capacity = capacity;

	return true;
}

static void luaext_watch_heap_push(const luaext_watch_entry *entry)
{
	size_t index;

	if (!luaext_watch_heap_reserve()) {
		/*
		 * Out of memory publishing a deadline. The wall limit loses its
		 * out-of-VM coverage for this call; the CPU limit does not, because the
		 * in-VM self-check does not depend on this heap at all.
		 */
		return;
	}

	index = luaext_watch.heap_count++;
	luaext_watch.heap[index] = *entry;

	while (index > 0) {
		size_t parent = (index - 1) / 2;

		if (luaext_watch.heap[parent].deadline <= luaext_watch.heap[index].deadline) {
			break;
		}

		{
			luaext_watch_entry swap = luaext_watch.heap[parent];
			luaext_watch.heap[parent] = luaext_watch.heap[index];
			luaext_watch.heap[index] = swap;
		}

		index = parent;
	}
}

static void luaext_watch_heap_pop(luaext_watch_entry *out)
{
	size_t index = 0;

	*out = luaext_watch.heap[0];
	luaext_watch.heap_count--;

	if (luaext_watch.heap_count == 0) {
		return;
	}

	luaext_watch.heap[0] = luaext_watch.heap[luaext_watch.heap_count];

	for (;;) {
		size_t left = index * 2 + 1;
		size_t right = left + 1;
		size_t smallest = index;

		if (left < luaext_watch.heap_count &&
			luaext_watch.heap[left].deadline < luaext_watch.heap[smallest].deadline) {
			smallest = left;
		}

		if (right < luaext_watch.heap_count &&
			luaext_watch.heap[right].deadline < luaext_watch.heap[smallest].deadline) {
			smallest = right;
		}

		if (smallest == index) {
			return;
		}

		{
			luaext_watch_entry swap = luaext_watch.heap[smallest];
			luaext_watch.heap[smallest] = luaext_watch.heap[index];
			luaext_watch.heap[index] = swap;
		}

		index = smallest;
	}
}

/* -------------------------------------------------------------------------
 * Accounting. Caller holds slot->lock.
 * ---------------------------------------------------------------------- */

/*
 * What this slot has spent, right now.
 *
 * The CPU clock is read INSIDE the lock rather than snapshotted and read later:
 * luaext_watchdog_release() closes the handle under this same lock, so a
 * snapshot taken outside it is a handle that may already have been closed.
 *
 * A CPU clock that cannot be read is reported as spent, not as zero. The only
 * realistic cause is that the owning thread has exited, and a sandbox whose
 * thread is gone must not look like one using no CPU at all.
 */
static void luaext_watch_sample(luaext_watch_slot *slot, uint64_t *cpu, uint64_t *wall,
								bool *cpu_lost)
{
	*cpu = slot->cpu_used;
	*wall = slot->wall_used;
	*cpu_lost = false;

	if ((slot->open & LUAEXT_WATCH_CPU) != 0 && slot->clock_ok) {
		uint64_t now = 0;

		if (!luaext_clock_read(&slot->clock, &now)) {
			*cpu_lost = true;
		} else if (now > slot->cpu_base) {
			*cpu += now - slot->cpu_base;
		}
	}

	if ((slot->open & LUAEXT_WATCH_WALL) != 0) {
		uint64_t now = luaext_clock_monotonic_ns();

		if (now > slot->wall_base) {
			*wall += now - slot->wall_base;
		}
	}
}

static void luaext_watch_raise(luaext_watch_slot *slot, uint8_t reason)
{
	luaext_irq *irq = slot->irq;

	if (irq == NULL) {
		return;
	}

	/*
	 * Reason first and relaxed, then the flag with release. LUAEXT_CHECK loads
	 * the flag relaxed on the hot path and executes an acquire fence before
	 * acting, so a reader that observes the flag also observes this reason.
	 * Storing them the other way round lets a weakly-ordered CPU report the
	 * wrong one, which would surface as a WallClockLimitError for a CPU breach.
	 */
	atomic_store_explicit(&irq->reason, reason, memory_order_relaxed);
	atomic_store_explicit(&irq->interrupted, (unsigned char)1, memory_order_release);
}

/*
 * Decide whether this slot is over budget and, if so, raise its interrupt.
 *
 * Returns true when it tripped. The flag is deliberately sticky: nothing here
 * ever clears it, because it is the only thing that still stops a script when
 * Lua itself swallows the resulting error -- which is exactly what GCTM does to
 * an error raised inside a finaliser.
 */
static bool luaext_watch_evaluate(luaext_watch_slot *slot)
{
	uint64_t cpu;
	uint64_t wall;
	bool cpu_lost;

	if (!slot->armed || slot->irq == NULL) {
		return false;
	}

	luaext_watch_sample(slot, &cpu, &wall, &cpu_lost);

	/*
	 * Two ways the clock can betray us, treated identically: it died while a
	 * segment was open (cpu_lost), or it refused to give us a base to open one
	 * against in the first place (cpu_open_failed). Either way the CPU budget is
	 * unmeasurable from here on, and unmeasurable must mean stopped.
	 */
	if (cpu_lost || slot->cpu_open_failed) {
		luaext_watch_raise(slot, LUAEXT_WATCH_REASON_CPU);
		return true;
	}

	if (slot->cpu_limit != 0 && cpu >= slot->cpu_limit) {
		luaext_watch_raise(slot, LUAEXT_WATCH_REASON_CPU);
		return true;
	}

	/* Before the wall limit: when both are spent, the host that asked for a CPU
	 * limit on a coarse platform should hear about the CPU limit. */
	if (slot->cpu_wall_limit != 0 && wall >= slot->cpu_wall_limit) {
		luaext_watch_raise(slot, LUAEXT_WATCH_REASON_CPU);
		return true;
	}

	if (slot->wall_limit != 0 && wall >= slot->wall_limit) {
		luaext_watch_raise(slot, LUAEXT_WATCH_REASON_WALL);
		return true;
	}

	return false;
}

/*
 * The earliest monotonic instant at which this slot could possibly trip.
 *
 * Thread CPU time advances no faster than wall time, so whatever CPU budget
 * remains is a valid lower bound on how long the watchdog may sleep. That is
 * what turns a CPU-bound one-second limit into about one wakeup instead of two
 * thousand polls.
 */
static bool luaext_watch_deadline(luaext_watch_slot *slot, uint64_t *deadline)
{
	uint64_t cpu;
	uint64_t wall;
	uint64_t remaining = UINT64_MAX;
	bool cpu_lost;

	if (!slot->armed || slot->irq == NULL || slot->open == 0) {
		return false;
	}

	luaext_watch_sample(slot, &cpu, &wall, &cpu_lost);

	if (cpu_lost) {
		*deadline = luaext_clock_monotonic_ns();
		return true;
	}

	if ((slot->open & LUAEXT_WATCH_CPU) != 0 && slot->cpu_limit != 0) {
		remaining = slot->cpu_limit > cpu ? slot->cpu_limit - cpu : 0;
	}

	if ((slot->open & LUAEXT_WATCH_WALL) != 0) {
		if (slot->cpu_wall_limit != 0) {
			uint64_t left = slot->cpu_wall_limit > wall ? slot->cpu_wall_limit - wall : 0;

			remaining = left < remaining ? left : remaining;
		}

		if (slot->wall_limit != 0) {
			uint64_t left = slot->wall_limit > wall ? slot->wall_limit - wall : 0;

			remaining = left < remaining ? left : remaining;
		}
	}

	if (remaining == UINT64_MAX) {
		return false;
	}

	if (remaining < luaext_watch.floor_ns) {
		remaining = luaext_watch.floor_ns;
	}

	*deadline = luaext_clock_monotonic_ns() + remaining;

	return true;
}

/* Take a fresh epoch so any entry already in the heap becomes a duplicate.
 * Caller holds slot->lock. */
static void luaext_watch_stamp(luaext_watch_slot *slot, luaext_watch_entry *entry,
							   uint64_t deadline)
{
	entry->slot = slot;
	entry->generation = slot->generation;
	entry->epoch = ++slot->epoch;
	entry->deadline = deadline;
}

/* Publish. Caller holds NOTHING: taking watchdog.lock while holding slot->lock
 * would be the one ordering this file forbids. */
static void luaext_watch_publish(const luaext_watch_entry *entry)
{
	if (!luaext_watch.ready) {
		return;
	}

	luaext_mutex_lock(&luaext_watch.lock);

	if (luaext_watch.running) {
		luaext_watch_heap_push(entry);
		luaext_cond_signal(&luaext_watch.cond);
	}

	luaext_mutex_unlock(&luaext_watch.lock);
}

/* -------------------------------------------------------------------------
 * The thread
 * ---------------------------------------------------------------------- */

/* Caller holds watchdog.lock, and takes slot->lock inside it -- the permitted
 * direction. */
static void luaext_watch_service(const luaext_watch_entry *entry)
{
	luaext_watch_slot *slot = entry->slot;
	luaext_watch_entry next;
	bool republish = false;

	luaext_mutex_lock(&slot->lock);

	/*
	 * Two lazy reaps in one test. A generation mismatch means the slot was
	 * released (and possibly re-let) since this entry was published, so the
	 * entry describes a sandbox that no longer exists. An epoch mismatch means
	 * the owner republished a better deadline and this entry is a duplicate.
	 * Either way it is dropped without being re-queued, which is what keeps a
	 * detach that never touches the watchdog lock from leaking heap entries.
	 */
	if (slot->generation == entry->generation && slot->epoch == entry->epoch) {
		if (!luaext_watch_evaluate(slot)) {
			uint64_t deadline;

			if (luaext_watch_deadline(slot, &deadline)) {
				luaext_watch_stamp(slot, &next, deadline);
				republish = true;
			}
		}
	}

	luaext_mutex_unlock(&slot->lock);

	if (republish) {
		luaext_watch_heap_push(&next);
	}
}

static void *luaext_watch_main(void *argument)
{
	(void)argument;

	luaext_mutex_lock(&luaext_watch.lock);

	while (!luaext_watch.stop) {
		uint64_t now;

		if (luaext_watch.heap_count == 0) {
			luaext_cond_wait(&luaext_watch.cond, &luaext_watch.lock);
			continue;
		}

		now = luaext_clock_monotonic_ns();

		if (luaext_watch.heap[0].deadline > now) {
			luaext_cond_wait_for(&luaext_watch.cond, &luaext_watch.lock,
								 luaext_watch.heap[0].deadline - now);
			continue;
		}

		{
			luaext_watch_entry entry;

			luaext_watch_heap_pop(&entry);
			luaext_watch_service(&entry);
		}
	}

	luaext_mutex_unlock(&luaext_watch.lock);

	return NULL;
}

static void luaext_watch_start_once(void)
{
	luaext_mutex_lock(&luaext_watch.lock);

	if (!luaext_watch.running && !luaext_watch.stop) {
		luaext_watch.running = luaext_thread_start(&luaext_watch.thread, luaext_watch_main, NULL);
		luaext_watch.failed = !luaext_watch.running;
	}

	luaext_mutex_unlock(&luaext_watch.lock);
}

/*
 * Lazily start the thread on the first armed limit.
 *
 * A process that never sets a timing limit never creates it. Under a worker SAPI
 * many PHP threads can reach this together, which is what the once-flag is for
 * -- and it also supplies the happens-before edge that lets every caller read
 * `running` without further synchronisation.
 */
static void luaext_watch_ensure_thread(void)
{
	if (!luaext_watch.ready) {
		return;
	}

	luaext_once_run(&luaext_watch.once, luaext_watch_start_once);
}

/* -------------------------------------------------------------------------
 * Module lifecycle
 * ---------------------------------------------------------------------- */

void luaext_watchdog_startup(void)
{
	memset(&luaext_watch_pool, 0, sizeof(luaext_watch_pool));

	luaext_watch.heap = NULL;
	luaext_watch.heap_count = 0;
	luaext_watch.heap_capacity = 0;
	luaext_watch.floor_ns = LUAEXT_WATCH_DEFAULT_FLOOR_NS;
	luaext_watch.running = false;
	luaext_watch.stop = false;
	luaext_watch.ready = false;

	{
		luaext_once fresh = LUAEXT_ONCE_INIT;

		luaext_watch.once = fresh;
	}

	if (!luaext_mutex_init(&luaext_watch_pool.lock)) {
		return;
	}

	luaext_watch_pool.ready = true;

	if (!luaext_mutex_init(&luaext_watch.lock)) {
		return;
	}

	if (!luaext_cond_init(&luaext_watch.cond)) {
		luaext_mutex_destroy(&luaext_watch.lock);
		return;
	}

	luaext_watch.ready = true;
}

void luaext_watchdog_shutdown(void)
{
	luaext_watch_block *block;

	if (luaext_watch.ready) {
		luaext_mutex_lock(&luaext_watch.lock);
		luaext_watch.stop = true;
		luaext_cond_signal(&luaext_watch.cond);
		luaext_mutex_unlock(&luaext_watch.lock);

		/*
		 * JOIN before releasing anything the thread could be reading. This is
		 * the whole ordering: after the join no thread can be inside a slot, so
		 * the backing store is safe to hand back. Reversed, it is a
		 * use-after-free that only shows up under load.
		 */
		luaext_thread_join(&luaext_watch.thread);

		luaext_watch.running = false;

		luaext_cond_destroy(&luaext_watch.cond);
		luaext_mutex_destroy(&luaext_watch.lock);
		luaext_watch.ready = false;
	}

	free(luaext_watch.heap);
	luaext_watch.heap = NULL;
	luaext_watch.heap_count = 0;
	luaext_watch.heap_capacity = 0;

	block = luaext_watch_pool.blocks;

	while (block != NULL) {
		luaext_watch_block *next = block->next;
		size_t index;

		for (index = 0; index < LUAEXT_WATCH_BLOCK_SLOTS; index++) {
			luaext_clock_release(&block->slots[index].clock);
			luaext_mutex_destroy(&block->slots[index].lock);
		}

		free(block);
		block = next;
	}

	if (luaext_watch_pool.ready) {
		luaext_mutex_destroy(&luaext_watch_pool.lock);
	}

	memset(&luaext_watch_pool, 0, sizeof(luaext_watch_pool));

	/*
	 * Re-arm the once-flag. A pthread_once_t cannot be reset while anything
	 * could be racing it; nothing can, here, because the thread has been joined
	 * and the module is going away. Skipping this would mean that an SAPI which
	 * runs MINIT twice in one process gets a watchdog the first time and
	 * silently none the second -- an INI-free way to void the wall limit.
	 */
	{
		luaext_once fresh = LUAEXT_ONCE_INIT;

		luaext_watch.once = fresh;
	}

	luaext_watch.stop = false;
}

void luaext_watchdog_set_resolution_ns(uint64_t ns)
{
	luaext_watch.floor_ns = ns == 0 ? LUAEXT_WATCH_DEFAULT_FLOOR_NS : ns;
}

bool luaext_watchdog_thread_running(void)
{
	bool running;

	if (!luaext_watch.ready) {
		return false;
	}

	luaext_mutex_lock(&luaext_watch.lock);
	running = luaext_watch.running;
	luaext_mutex_unlock(&luaext_watch.lock);

	return running;
}

bool luaext_watchdog_thread_failed(void)
{
	bool failed;

	if (!luaext_watch.ready) {
		return true;
	}

	luaext_mutex_lock(&luaext_watch.lock);
	failed = luaext_watch.failed;
	luaext_mutex_unlock(&luaext_watch.lock);

	return failed;
}

/* -------------------------------------------------------------------------
 * Slots
 * ---------------------------------------------------------------------- */

luaext_watch_slot *luaext_watchdog_acquire(luaext_irq *irq)
{
	luaext_watch_slot *slot = luaext_watch_pool_take();

	if (slot == NULL) {
		return NULL;
	}

	luaext_mutex_lock(&slot->lock);

	slot->generation++;
	slot->irq = irq;
	slot->cpu_limit = 0;
	slot->wall_limit = 0;
	slot->cpu_wall_limit = 0;
	slot->cpu_used = 0;
	slot->wall_used = 0;
	slot->cpu_base = 0;
	slot->wall_base = 0;
	slot->armed = false;
	slot->open = 0;
	slot->paused = 0;
	slot->has_limits = false;
	slot->hook_ticks = 0;

	/* Slots are recycled, so a clock failure recorded by the previous tenant must
	 * not be inherited by this one. */
	slot->cpu_open_failed = false;

	/* On the OWNING thread, which is why this is done at construction rather
	 * than at the first arm: the thread demonstrably exists here. */
	slot->clock_ok = luaext_clock_capture_self(&slot->clock);

	luaext_mutex_unlock(&slot->lock);

	return slot;
}

void luaext_watchdog_release(luaext_watch_slot *slot)
{
	if (slot == NULL) {
		return;
	}

	luaext_mutex_lock(&slot->lock);

	/*
	 * The irq is forgotten, not written to. It belongs to the sandbox rather
	 * than to this slot, it outlives the slot, and what its flag should say
	 * during teardown is a decision the layer that knows about teardown makes --
	 * see luaext_timers_detach().
	 */
	slot->irq = NULL;

	if (slot->clock_ok) {
		luaext_clock_release(&slot->clock);
		slot->clock_ok = false;
	}

	slot->armed = false;
	slot->open = 0;
	slot->paused = 0;
	slot->has_limits = false;

	/*
	 * Invalidates every outstanding heap entry. Note what is NOT done here: the
	 * watchdog lock is never taken, and nothing is removed from the heap. Those
	 * entries are reaped when they surface, which is what keeps teardown off a
	 * process-wide lock.
	 */
	slot->generation++;

	luaext_mutex_unlock(&slot->lock);

	luaext_watch_pool_give(slot);
}

/* -------------------------------------------------------------------------
 * Limits
 * ---------------------------------------------------------------------- */

bool luaext_watchdog_cpu_is_degraded(uint64_t limit_ns, uint64_t resolution_ns)
{
	if (limit_ns == 0) {
		return false;
	}

	if (resolution_ns == 0) {
		return true;
	}

	return limit_ns / resolution_ns < LUAEXT_WATCH_MIN_TICKS;
}

/* Republish under the ordering rule: stamp under slot->lock, push outside it. */
static void luaext_watchdog_republish(luaext_watch_slot *slot)
{
	luaext_watch_entry entry;
	uint64_t deadline;
	bool queue;

	luaext_mutex_lock(&slot->lock);

	/*
	 * Evaluated before republishing, exactly as arm() and resume() do, and for a
	 * reason that is a live denial of service rather than a tidiness argument.
	 *
	 * luaext_watch_stamp() takes a fresh epoch, which makes whatever entry the
	 * watchdog is currently sleeping on a duplicate to be discarded. A host
	 * callback that calls setLimits() in a loop therefore replaces the pending
	 * entry faster than the thread can act on it, and -- because the deadline
	 * published here is the MINIMUM across the CPU and wall limits -- it takes
	 * the wall backstop down with it. Both limits then go unenforced for as long
	 * as the script keeps calling, which is precisely the attack
	 * tests/02-limits/cpu-limit-not-refunded.phpt exists to disprove.
	 *
	 * Deciding here, on the owner's own thread, removes the race outright: a
	 * budget that is already spent trips synchronously and the script stops at
	 * the next back-edge check, with no watchdog round trip to lose.
	 */
	queue = !luaext_watch_evaluate(slot) && luaext_watch_deadline(slot, &deadline);

	if (queue) {
		luaext_watch_stamp(slot, &entry, deadline);
	}

	luaext_mutex_unlock(&slot->lock);

	if (queue) {
		luaext_watch_publish(&entry);
	}
}

void luaext_watchdog_set_cpu_limit(luaext_watch_slot *slot, uint64_t ns, uint64_t resolution_ns)
{
	if (slot == NULL) {
		return;
	}

	luaext_mutex_lock(&slot->lock);

	/*
	 * The new budget is limit-minus-used, because nothing here resets cpu_used.
	 * A deliberate divergence from the extension this replaces, whose setter
	 * reset the counter and so let a host callback call it in a loop and run
	 * forever.
	 */
	slot->cpu_limit = ns;
	slot->cpu_wall_limit = luaext_watchdog_cpu_is_degraded(ns, resolution_ns) ? ns : 0;
	slot->has_limits = slot->cpu_limit != 0 || slot->wall_limit != 0;

	luaext_mutex_unlock(&slot->lock);

	if (slot->has_limits) {
		luaext_watch_ensure_thread();
		luaext_watchdog_republish(slot);
	}
}

void luaext_watchdog_set_wall_limit(luaext_watch_slot *slot, uint64_t ns)
{
	if (slot == NULL) {
		return;
	}

	luaext_mutex_lock(&slot->lock);
	slot->wall_limit = ns;
	slot->has_limits = slot->cpu_limit != 0 || slot->wall_limit != 0;
	luaext_mutex_unlock(&slot->lock);

	if (slot->has_limits) {
		luaext_watch_ensure_thread();
		luaext_watchdog_republish(slot);
	}
}

/* -------------------------------------------------------------------------
 * Arming
 * ---------------------------------------------------------------------- */

/* Open the segments named by `mask` that are not paused. Caller holds the
 * lock. */
static void luaext_watch_open(luaext_watch_slot *slot, uint8_t mask)
{
	mask = (uint8_t)(mask & ~slot->paused);
	mask = (uint8_t)(mask & ~slot->open);

	if ((mask & LUAEXT_WATCH_CPU) != 0 && slot->clock_ok) {
		uint64_t now = 0;

		if (luaext_clock_read(&slot->clock, &now)) {
			slot->cpu_base = now;
			slot->open |= LUAEXT_WATCH_CPU;
		} else {
			/*
			 * Fail closed. Leaving the segment shut and saying nothing would
			 * silently un-enforce the CPU limit for the rest of the call -- see
			 * cpu_open_failed. luaext_watch_sample() already treats a failed read
			 * this way; this is the same rule applied on the way in.
			 */
			slot->cpu_open_failed = true;
		}
	}

	if ((mask & LUAEXT_WATCH_WALL) != 0) {
		slot->wall_base = luaext_clock_monotonic_ns();
		slot->open |= LUAEXT_WATCH_WALL;
	}
}

/* Close the segments named by `mask`, billing what they measured. Caller holds
 * the lock. */
static void luaext_watch_close(luaext_watch_slot *slot, uint8_t mask)
{
	uint64_t cpu;
	uint64_t wall;
	bool cpu_lost;

	mask = (uint8_t)(mask & slot->open);

	if (mask == 0) {
		return;
	}

	luaext_watch_sample(slot, &cpu, &wall, &cpu_lost);

	if ((mask & LUAEXT_WATCH_CPU) != 0 && !cpu_lost) {
		slot->cpu_used = cpu;
	}

	if ((mask & LUAEXT_WATCH_WALL) != 0) {
		slot->wall_used = wall;
	}

	slot->open = (uint8_t)(slot->open & ~mask);
}

void luaext_watchdog_arm(luaext_watch_slot *slot)
{
	luaext_watch_entry entry;
	uint64_t deadline;
	bool queue;

	/*
	 * The no-limit fast path, and the reason it reads an owner-only mirror
	 * rather than the slot's own state: every eval() and call() goes through
	 * here, and a process-wide -- or even a per-slot -- lock acquisition on the
	 * common case where the host set no timing limit would be a measurable
	 * regression for no benefit.
	 */
	if (slot == NULL || !slot->has_limits || slot->armed) {
		return;
	}

	luaext_watch_ensure_thread();

	luaext_mutex_lock(&slot->lock);

	slot->armed = true;
	slot->open = 0;
	luaext_watch_open(slot, LUAEXT_WATCH_BOTH);

	/*
	 * Evaluated on the way in, not just on the way round. A budget that is
	 * already spent -- because an earlier call used it, or because the host set
	 * a limit below what has been used -- must stop this call before it executes
	 * anything, and waiting for the first wakeup or the first hook tick would
	 * let a short chunk run to completion inside a limit it had already
	 * exhausted. The flag is sticky, so the boundary that returns to PHP reports
	 * it even if the chunk never ticks the hook at all.
	 */
	queue = !luaext_watch_evaluate(slot) && luaext_watch_deadline(slot, &deadline);

	if (queue) {
		luaext_watch_stamp(slot, &entry, deadline);
	}

	luaext_mutex_unlock(&slot->lock);

	if (queue) {
		luaext_watch_publish(&entry);
	}
}

void luaext_watchdog_disarm(luaext_watch_slot *slot)
{
	/*
	 * Keyed on `armed`, not on has_limits: a limit lifted mid-call would
	 * otherwise leave the segments open forever and the next arm() would find
	 * stale bases. `armed` is written only by the owning thread, so reading it
	 * here keeps the no-limit path lock-free just the same -- nothing that was
	 * never armed reaches the lock.
	 */
	if (slot == NULL || !slot->armed) {
		return;
	}

	luaext_mutex_lock(&slot->lock);

	luaext_watch_close(slot, LUAEXT_WATCH_BOTH);
	slot->armed = false;

	/*
	 * A pause does not outlive the call it was taken in. The reference
	 * implementation calls that auto-unpausing; either way it errs towards
	 * billing, which is the only safe direction for a limit.
	 */
	slot->paused = 0;

	/*
	 * Cleared with the arming, not carried across calls. A clock that failed to
	 * open once must not condemn every later call on the same sandbox -- the next
	 * arm() reads the clock again and gets to decide for itself. Leaving it set
	 * would turn one transient failure into a permanently unusable sandbox.
	 */
	slot->cpu_open_failed = false;

	/*
	 * The interrupt flag is NOT cleared here. It is sticky until the outermost
	 * call has fully unwound, and clearing it belongs to the layer that owns the
	 * luaext_irq -- which also has to clear it for a sandbox that never armed
	 * anything, because Sandbox::interrupt() can raise it with no limit set at
	 * all. See luaext_timers_leave_lua().
	 */

	luaext_mutex_unlock(&slot->lock);
}

/* -------------------------------------------------------------------------
 * Pausing
 * ---------------------------------------------------------------------- */

void luaext_watchdog_pause(luaext_watch_slot *slot, uint8_t mask)
{
	if (slot == NULL || mask == 0) {
		return;
	}

	slot->paused = (uint8_t)(slot->paused | mask);

	if (!slot->has_limits) {
		return;
	}

	luaext_mutex_lock(&slot->lock);
	luaext_watch_close(slot, mask);
	luaext_mutex_unlock(&slot->lock);

	/*
	 * No republication. Closing a segment can only push a deadline LATER, and an
	 * entry that fires early costs one re-evaluation which finds nothing and
	 * reschedules. Paying a watchdog-lock acquisition on every callback boundary
	 * to save that would be the wrong trade.
	 */
}

bool luaext_watchdog_resume(luaext_watch_slot *slot, uint8_t mask)
{
	luaext_watch_entry entry;
	uint64_t deadline;
	bool queue = false;
	bool spent = false;

	if (slot == NULL || mask == 0) {
		return false;
	}

	slot->paused = (uint8_t)(slot->paused & ~mask);

	if (!slot->has_limits) {
		return false;
	}

	luaext_mutex_lock(&slot->lock);

	if (slot->armed) {
		luaext_watch_open(slot, mask);

		/*
		 * Evaluated at the moment of resumption rather than left to the next
		 * wakeup. A budget that is already spent must trip NOW: waiting would
		 * hand the script a whole extra budget, which is exactly the hole the
		 * old "expired while paused" reconstruction existed to paper over.
		 * Nothing expires while paused here, because while paused nothing is
		 * measured -- but time billed BEFORE the pause still counts.
		 */
		spent = luaext_watch_evaluate(slot);

		if (!spent) {
			queue = luaext_watch_deadline(slot, &deadline);

			if (queue) {
				luaext_watch_stamp(slot, &entry, deadline);
			}
		}
	}

	luaext_mutex_unlock(&slot->lock);

	if (queue) {
		luaext_watch_publish(&entry);
	}

	return spent;
}

uint8_t luaext_watchdog_pause_mask(const luaext_watch_slot *slot)
{
	/* Owner-thread-only field; see the slot declaration for why that makes this
	 * safe without the lock. */
	return slot == NULL ? (uint8_t)0 : slot->paused;
}

/* -------------------------------------------------------------------------
 * Usage
 * ---------------------------------------------------------------------- */

/*
 * The const is a promise to the CALLER about the slot's observable state, not a
 * claim that no lock is taken: the clock has to be read under it, because
 * release() closes the handle there.
 */
static void luaext_watchdog_usage(const luaext_watch_slot *slot, uint64_t *cpu, uint64_t *wall)
{
	luaext_watch_slot *live = (luaext_watch_slot *)slot;
	bool cpu_lost;

	*cpu = 0;
	*wall = 0;

	if (slot == NULL) {
		return;
	}

	luaext_mutex_lock(&live->lock);
	luaext_watch_sample(live, cpu, wall, &cpu_lost);
	luaext_mutex_unlock(&live->lock);
}

uint64_t luaext_watchdog_cpu_ns(const luaext_watch_slot *slot)
{
	uint64_t cpu;
	uint64_t wall;

	luaext_watchdog_usage(slot, &cpu, &wall);

	return cpu;
}

uint64_t luaext_watchdog_wall_ns(const luaext_watch_slot *slot)
{
	uint64_t cpu;
	uint64_t wall;

	luaext_watchdog_usage(slot, &cpu, &wall);

	return wall;
}

/* -------------------------------------------------------------------------
 * The in-VM self-check
 * ---------------------------------------------------------------------- */

bool luaext_watchdog_self_check(luaext_watch_slot *slot)
{
	bool over;

	if (slot == NULL || !slot->has_limits || !slot->armed) {
		return false;
	}

	/*
	 * The stride lives here rather than in the caller because the counter has to
	 * live somewhere per-sandbox, and this slot is the only per-sandbox storage
	 * the pure layer owns. Reading a thread CPU clock is a syscall on most
	 * platforms; doing it on every count-hook tick would cost more than running
	 * the script.
	 */
	if (++slot->hook_ticks < LUAEXT_WATCH_SELF_CHECK_STRIDE) {
		return false;
	}

	slot->hook_ticks = 0;

	luaext_mutex_lock(&slot->lock);
	over = luaext_watch_evaluate(slot);
	luaext_mutex_unlock(&slot->lock);

	return over;
}

/*
 * The self-check without the stride, for the one place per call that must not
 * sample lazily: the return boundary.
 *
 * Delivery through the watchdog thread is a race the thread can lose. A script
 * that crosses its deadline shortly before returning can be back in PHP before
 * the thread's next wakeup, and the boundary then disarms the slot -- so the
 * wakeup finds nothing to service and a measured breach reports success. A
 * contended macOS runner lost exactly that race, with the artifact to show for
 * it: 0.151s of CPU billed against a 0.10s limit, no error. Sampling once at
 * the boundary makes the verdict deterministic, whatever the thread's latency.
 */
bool luaext_watchdog_final_check(luaext_watch_slot *slot)
{
	bool over;

	if (slot == NULL || !slot->has_limits || !slot->armed) {
		return false;
	}

	luaext_mutex_lock(&slot->lock);
	over = luaext_watch_evaluate(slot);
	luaext_mutex_unlock(&slot->lock);

	return over;
}
