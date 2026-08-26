/*
 * luaext — releasing PHP references outside Lua's collector.
 *
 * See luaext_defer.h for why this exists at all. This file is only the
 * bookkeeping.
 */

#include "luaext_defer.h"

#include <Zend/zend_API.h>

/* Slots the queue starts with, and the factor it grows by. Small on purpose:
 * the common sandbox defers nothing, and one that does usually defers a handful
 * of closures at teardown rather than a stream. */
#define LUAEXT_DEFER_INITIAL 8

static bool luaext_defer_reserve(luaext_deferred *queue)
{
	size_t capacity;
	luaext_defer_item *grown;

	if (queue->count < queue->capacity) {
		return true;
	}

	capacity = queue->capacity == 0 ? LUAEXT_DEFER_INITIAL : queue->capacity * 2;

	/* perealloc rather than erealloc: __gc runs from lua_close() during the
	 * request-shutdown sweep, when request memory is already gone. */
	grown = (luaext_defer_item *)perealloc(queue->items, capacity * sizeof(*grown), 1);

	if (grown == NULL) {
		return false;
	}

	queue->items = grown;
	queue->capacity = capacity;

	return true;
}

bool luaext_defer_fcc(luaext_sandbox *sandbox, zend_fcall_info_cache *fcc)
{
	luaext_defer_item *item;

	if (sandbox == NULL || !luaext_defer_reserve(&sandbox->deferred)) {
		return false;
	}

	item = &sandbox->deferred.items[sandbox->deferred.count++];
	item->kind = LUAEXT_DEFER_FCC;
	item->as.fcc = *fcc;

	/* The caller no longer owns it, and must not release it too. */
	memset(fcc, 0, sizeof(*fcc));

	return true;
}

bool luaext_defer_zval(luaext_sandbox *sandbox, zval *value)
{
	luaext_defer_item *item;

	if (sandbox == NULL || !luaext_defer_reserve(&sandbox->deferred)) {
		return false;
	}

	item = &sandbox->deferred.items[sandbox->deferred.count++];
	item->kind = LUAEXT_DEFER_ZVAL;
	ZVAL_COPY_VALUE(&item->as.value, value);

	ZVAL_UNDEF(value);

	return true;
}

void luaext_defer_drain(luaext_sandbox *sandbox)
{
	luaext_defer_item *pending;
	size_t count;
	size_t index;

	if (sandbox == NULL || sandbox->deferred.count == 0) {
		return;
	}

	/*
	 * Detach before releasing anything.
	 *
	 * A destructor released below may call back into this sandbox, and a
	 * re-entrant path can reach this function again. Handing it an already-empty
	 * queue is what stops it from releasing the same items a second time -- the
	 * array is still ours, but nothing in it is reachable from the sandbox any
	 * more. New items pushed by that destructor land in a fresh allocation and
	 * are drained by whichever call is still unwinding.
	 */
	pending = sandbox->deferred.items;
	count = sandbox->deferred.count;

	sandbox->deferred.items = NULL;
	sandbox->deferred.count = 0;
	sandbox->deferred.capacity = 0;

	for (index = 0; index < count; index++) {
		luaext_defer_item *item = &pending[index];

		switch (item->kind) {
		case LUAEXT_DEFER_FCC:
			if (ZEND_FCC_INITIALIZED(item->as.fcc)) {
				zend_fcc_dtor(&item->as.fcc);
			}
			break;

		case LUAEXT_DEFER_ZVAL:
			zval_ptr_dtor(&item->as.value);
			break;
		}
	}

	pefree(pending, 1);
}

void luaext_defer_shutdown(luaext_sandbox *sandbox)
{
	if (sandbox == NULL) {
		return;
	}

	/*
	 * Bounded rather than while(count): a destructor released here can push more
	 * work, and one that does so unconditionally would spin forever. Anything
	 * still queued after the last pass is leaked deliberately -- at shutdown a
	 * bounded leak is preferable to a hang, and a destructor that defers on eight
	 * consecutive passes is misbehaving in a way this layer cannot fix.
	 */
	{
		int pass;

		for (pass = 0; pass < 8 && sandbox->deferred.count > 0; pass++) {
			luaext_defer_drain(sandbox);
		}
	}

	if (sandbox->deferred.items != NULL) {
		pefree(sandbox->deferred.items, 1);
		sandbox->deferred.items = NULL;
	}

	sandbox->deferred.count = 0;
	sandbox->deferred.capacity = 0;
}
