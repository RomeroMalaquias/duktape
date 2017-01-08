/*
 *  Heap stringtable handling, string interning.
 */

#include "duk_internal.h"

#define DUK__PREVENT_MS_SIDE_EFFECTS(heap) do { \
		(heap)->mark_and_sweep_base_flags |= \
		        DUK_MS_FLAG_NO_STRINGTABLE_RESIZE |  /* avoid recursive string table call */ \
		        DUK_MS_FLAG_NO_FINALIZERS |          /* avoid pressure to add/remove strings, invalidation of call data argument, etc. */ \
		        DUK_MS_FLAG_NO_OBJECT_COMPACTION;    /* avoid array abandoning which interns strings */ \
	} while (0)

/*
 *  Create a hstring and insert into the heap.  The created object
 *  is directly garbage collectable with reference count zero.
 *
 *  The caller must place the interned string into the stringtable
 *  immediately (without chance of a longjmp); otherwise the string
 *  is lost.
 */

DUK_LOCAL
duk_hstring *duk__alloc_init_hstring(duk_heap *heap,
                                     const duk_uint8_t *str,
                                     duk_uint32_t blen,
                                     duk_uint32_t strhash,
                                     const duk_uint8_t *extdata) {
	duk_hstring *res = NULL;
	duk_uint8_t *data;
	duk_size_t alloc_size;
#if !defined(DUK_USE_HSTRING_ARRIDX)
	duk_uarridx_t dummy;
#endif
	duk_uint32_t clen;

#if defined(DUK_USE_STRLEN16)
	/* If blen <= 0xffffUL, clen is also guaranteed to be <= 0xffffUL. */
	if (blen > 0xffffUL) {
		DUK_D(DUK_DPRINT("16-bit string blen/clen active and blen over 16 bits, reject intern"));
		return NULL;
	}
#endif

	/* FIXME: extdata check only if extdata enabled? */

	/* FIXME: no point in NULLing link pointers... */

	if (extdata) {
		alloc_size = (duk_size_t) sizeof(duk_hstring_external);
		res = (duk_hstring *) DUK_ALLOC(heap, alloc_size);
		if (!res) {
			goto alloc_error;
		}
		DUK_MEMZERO(res, sizeof(duk_hstring_external));
#if defined(DUK_USE_EXPLICIT_NULL_INIT)
		DUK_HEAPHDR_STRING_INIT_NULLS(&res->hdr);
#endif
		DUK_HEAPHDR_SET_TYPE_AND_FLAGS(&res->hdr, DUK_HTYPE_STRING, DUK_HSTRING_FLAG_EXTDATA);

		((duk_hstring_external *) res)->extdata = extdata;
	} else {
		/* NUL terminate for convenient C access */
		DUK_ASSERT(sizeof(duk_hstring) + blen + 1 > blen);  /* No wrap, limits ensure. */
		alloc_size = (duk_size_t) (sizeof(duk_hstring) + blen + 1);
		res = (duk_hstring *) DUK_ALLOC(heap, alloc_size);
		if (!res) {
			goto alloc_error;
		}
		DUK_MEMZERO(res, sizeof(duk_hstring));
#if defined(DUK_USE_EXPLICIT_NULL_INIT)
		DUK_HEAPHDR_STRING_INIT_NULLS(&res->hdr);
#endif
		DUK_HEAPHDR_SET_TYPE_AND_FLAGS(&res->hdr, DUK_HTYPE_STRING, 0);

		data = (duk_uint8_t *) (res + 1);
		DUK_MEMCPY(data, str, blen);
		data[blen] = (duk_uint8_t) 0;
	}

	DUK_HSTRING_SET_HASH(res, strhash);
	DUK_HSTRING_SET_BYTELEN(res, blen);

	DUK_ASSERT(!DUK_HSTRING_HAS_ARRIDX(res));
#if defined(DUK_USE_HSTRING_ARRIDX)
	res->arridx = duk_js_to_arrayindex_string(str, blen);
	if (res->arridx != DUK_HSTRING_NO_ARRAY_INDEX) {
#else
	dummy = duk_js_to_arrayindex_string(str, blen);
	if (dummy != DUK_HSTRING_NO_ARRAY_INDEX) {
#endif
		/* Array index strings cannot be symbol strings,
		 * and they're always pure ASCII so blen = clen.
		 */
		DUK_HSTRING_SET_ARRIDX(res);
		DUK_HSTRING_SET_ASCII(res);
		DUK_ASSERT(duk_unicode_unvalidated_utf8_length(str, (duk_size_t) blen) == blen);
		clen = blen;
	} else {
		clen = (duk_uint32_t) duk_unicode_unvalidated_utf8_length(str, (duk_size_t) blen);
		DUK_ASSERT(clen <= blen);

		/* Using an explicit 'ASCII' flag has larger footprint (one call site
		 * only) but is quite useful for the case when there's no explicit
		 * 'clen' in duk_hstring.
		 */
		DUK_ASSERT(!DUK_HSTRING_HAS_ASCII(res));
		if (clen == blen) {
			DUK_HSTRING_SET_ASCII(res);
		}
	}
#if defined(DUK_USE_HSTRING_CLEN)
	DUK_HSTRING_SET_CHARLEN(res, clen);
#endif

	DUK_DDD(DUK_DDDPRINT("interned string, hash=0x%08lx, blen=%ld, clen=%ld, has_arridx=%ld, has_extdata=%ld",
	                     (unsigned long) DUK_HSTRING_GET_HASH(res),
	                     (long) DUK_HSTRING_GET_BYTELEN(res),
	                     (long) DUK_HSTRING_GET_CHARLEN(res),
	                     (long) (DUK_HSTRING_HAS_ARRIDX(res) ? 1 : 0),
	                     (long) (DUK_HSTRING_HAS_EXTDATA(res) ? 1 : 0)));

	return res;

 alloc_error:
	DUK_FREE(heap, res);
	return NULL;
}

/*
 *  Raw intern; string already checked not to be present.
 */

DUK_LOCAL duk_hstring *duk__do_intern(duk_heap *heap, const duk_uint8_t *str, duk_uint32_t blen, duk_uint32_t strhash) {
	duk_hstring *res;
	const duk_uint8_t *extdata;
	duk_hstring **tab;
	duk_hstring **slot;
	duk_hstring *other;
	duk_small_uint_t prev_mark_and_sweep_base_flags;

	DUK_D(DUK_DPRINT("DOINTERN: heap=%p, str=%p, blen=%lu, strhash=%lx",
	                 (void *) heap, (const void *) str, (unsigned long) blen, (unsigned long) strhash));

	/* Prevent any side effects on the string table and the caller provided
	 * str/blen arguments while interning is in progress.  For example, if
	 * the caller provided str/blen from a dynamic buffer, a finalizer might
	 * resize that dynamic buffer, invalidating the call arguments.
	 */
	DUK_ASSERT((heap->mark_and_sweep_base_flags & DUK_MS_FLAG_NO_STRINGTABLE_RESIZE) == 0);
	prev_mark_and_sweep_base_flags = heap->mark_and_sweep_base_flags;
	DUK__PREVENT_MS_SIDE_EFFECTS(heap);

#if defined(DUK_USE_HSTRING_EXTDATA) && defined(DUK_USE_EXTSTR_INTERN_CHECK)
	extdata = (const duk_uint8_t *) DUK_USE_EXTSTR_INTERN_CHECK(heap->heap_udata, (void *) DUK_LOSE_CONST(str), (duk_size_t) blen);
#else
	extdata = (const duk_uint8_t *) NULL;
#endif

	/* Allocate and initialize string, not yet linked. */

	res = duk__alloc_init_hstring(heap, str, blen, strhash, extdata);
	if (!res) {
		goto failed;
	}
	DUK_ASSERT(res->hdr.h_prev == NULL);
	DUK_ASSERT(res->hdr.h_next == NULL);

	/* Insert into stringtable. */

	tab = heap->strtable;
	slot = tab + (strhash & heap->st_mask);
	other = *slot;
	if (other == NULL) {
		DUK_ASSERT(res->hdr.h_prev == NULL);
		DUK_ASSERT(res->hdr.h_next == NULL);
	} else {
		res->hdr.h_next = other;
		DUK_ASSERT(res->hdr.h_prev == NULL);
		DUK_ASSERT(other->hdr.h_prev == NULL);
		other->hdr.h_prev = res;
	}
	*slot = res;

	/* String table grow/shrink check.  Because of chaining (and no
	 * accumulation issues as with hash probe chains and DELETED
	 * markers) there's never a mandatory need to resize right now.
	 * Check for the resize only periodically, based on st_count
	 * bit pattern.  Because string table removal doesn't do a shrink
	 * check, we do that also here.
	 */
	heap->st_count++;
	if ((heap->st_count & 0xffU) == 0) {  /* FIXME: best value; maybe lower for lowmemory */
		duk_uint32_t entries_per_chain;

		entries_per_chain = heap->st_count / heap->st_size;  /* FIXME: shift */
		if (entries_per_chain >= 2) {
			/* FIXME: concrete criterion */
		} else if (entries_per_chain == 0) {
			if (heap->st_size <= 1024) {
				/* FIXME: minimum size */
			}
		}
	}

	/* Note: hstring is in heap but has refcount zero and is not
	 * strongly reachable.  Caller should increase refcount and make
	 * the hstring reachable before any operations which require
	 * allocation (and possible gc).
	 */

 done:
	heap->mark_and_sweep_base_flags = prev_mark_and_sweep_base_flags;
	return res;

 failed:
	res = NULL;
	goto done;
}

/*
 *  Intern a string from str/blen.
 */

/* FIXME: ROM lookup */

DUK_INTERNAL duk_hstring *duk_heap_string_intern(duk_heap *heap, const duk_uint8_t *str, duk_uint32_t blen) {
	duk_hstring *res;
	duk_uint32_t strhash;
	duk_uint32_t mask;
	duk_hstring **tab;
	duk_hstring *h;

	DUK_D(DUK_DPRINT("INTERN: heap=%p, str=%p, blen=%lu", (void *) heap, (const void *) str, (unsigned long) blen));

	/* Preliminaries. */

	DUK_ASSERT(blen <= DUK_HSTRING_MAX_BYTELEN);  /* Caller is responsible for ensuring this. */
	strhash = duk_heap_hashstring(heap, str, (duk_size_t) blen);

	/* String table lookup. */

	tab = heap->strtable;
	mask = heap->st_mask;
	DUK_ASSERT(heap->st_size == mask + 1);

	h = tab[strhash & mask];
	while (h != NULL) {
		if (DUK_UNLIKELY(DUK_HSTRING_GET_HASH(h) == strhash)) {
			if (DUK_HSTRING_GET_BYTELEN(h) == blen &&
		            DUK_MEMCMP((const void *) str, (const void *) DUK_HSTRING_GET_DATA(h), (size_t) blen) == 0) {
				/* Found existing entry. */

				/* FIXME: raise to top? */
				return h;
			}
		}
		h = h->hdr.h_next;
	}

	/* ROM table lookup.  Because this lookup is slower, do it only after
	 * RAM lookup.  This works because no ROM string is ever interned into
	 * the RAM stringtable.
	 */

	/* Not found in string table; insert. */

	res = duk__do_intern(heap, str, blen, strhash);
	return res;  /* may be NULL */
}

/*
 *  Intern a string from u32.
 */

/* FIXME: special handling? we know arridx status etc */
DUK_INTERNAL duk_hstring *duk_heap_string_intern_u32(duk_heap *heap, duk_uint32_t val) {
	char buf[DUK_STRTAB_U32_MAX_STRLEN + 1];
	DUK_SNPRINTF(buf, sizeof(buf), "%lu", (unsigned long) val);
	buf[sizeof(buf) - 1] = (char) 0;
	DUK_ASSERT(DUK_STRLEN(buf) <= DUK_UINT32_MAX);  /* formatted result limited */
	return duk_heap_string_intern(heap, (const duk_uint8_t *) buf, (duk_uint32_t) DUK_STRLEN(buf));
}

/*
 *  FIXME: convenience variants.  Because the main use case is for the checked
 *  variants, make them the main functionality and provide a safe variant
 *  separately (it is only needed during heap init).  The problem with that
 *  is that longjmp state and error creation must already be possible to throw.
 */

DUK_INTERNAL duk_hstring *duk_heap_string_intern_checked(duk_hthread *thr, const duk_uint8_t *str, duk_uint32_t blen) {
	duk_hstring *res = duk_heap_string_intern(thr->heap, str, blen);
	if (!res) {
		DUK_ERROR_ALLOC_FAILED(thr);
	}
	return res;
}

DUK_INTERNAL duk_hstring *duk_heap_string_intern_u32_checked(duk_hthread *thr, duk_uint32_t val) {
	duk_hstring *res = duk_heap_string_intern_u32(thr->heap, val);
	if (!res) {
		DUK_ERROR_ALLOC_FAILED(thr);
	}
	return res;
}

/* Find and remove string from stringtable; caller must free the string
 * itself.  The string link pointers are left as garbage as the string
 * is immediately freed.
 * FIXME: change contract?
 */
DUK_INTERNAL void duk_heap_string_remove(duk_heap *heap, duk_hstring *h) {
	DUK_D(DUK_DPRINT("REMOVE: heap=%p, h=%p, blen=%lu, strhash=%lx",
	                 (void *) heap, (void *) h,
	                 (unsigned long) (h != NULL ? DUK_HSTRING_GET_BYTELEN(h) : 0),
	                 (unsigned long) (h != NULL ? DUK_HSTRING_GET_HASH(h) : 0)));

	DUK_ASSERT(h != NULL);

	if (h->hdr.h_prev == NULL) {
		/* Head of list; must lookup list head slot too. */
		duk_hstring **slot;

		slot = heap->strtable + (DUK_HSTRING_GET_HASH(h) & heap->st_mask);
		DUK_ASSERT(*slot != NULL);
		if (h->hdr.h_next == NULL) {
			*slot = NULL;
		} else {
			*slot = h->hdr.h_next;
			h->hdr.h_next->hdr.h_prev = NULL;
		}
	} else {
		h->hdr.h_prev->hdr.h_next = h->hdr.h_next;
		if (h->hdr.h_next != NULL) {
			h->hdr.h_next->hdr.h_prev = h->hdr.h_prev;
		}
	}

	/* There's no resize check on a string free.  The next string
	 * intern will do one.
	 */
}

/*
 *  Force string table resize.
 *
 *  FIXME: not really needed?
 */

#if defined(DUK_USE_MS_STRINGTABLE_RESIZE)
DUK_INTERNAL void duk_heap_force_strtab_resize(duk_heap *heap) {
	duk_small_uint_t prev_mark_and_sweep_base_flags;

	DUK_ASSERT((heap->mark_and_sweep_base_flags & DUK_MS_FLAG_NO_STRINGTABLE_RESIZE) == 0);
	prev_mark_and_sweep_base_flags = heap->mark_and_sweep_base_flags;
	DUK__PREVENT_MS_SIDE_EFFECTS(heap);

	/* FIXME */

	heap->mark_and_sweep_base_flags = prev_mark_and_sweep_base_flags;
}
#endif

/*
 *  Free strings in the stringtable and any allocations needed by the
 *  stringtable itself (nothing now).
 */

DUK_INTERNAL void duk_heap_free_strtab(duk_heap *heap) {
	duk_uint32_t i;
	duk_hstring *h;

	if (heap->strtable == NULL) {
		/* Can happen if heap init fails. */
		return;
	}

	for (i = 0; i < heap->st_size; i++) {
		h = heap->strtable[i];
		while (h) {
			duk_hstring *h_next;
			h_next = h->hdr.h_next;

			/* Strings may have inner refs (extdata) in some cases. */
			duk_free_hstring(heap, h);

			h = h_next;
		}
	}
}

/*
 *  Debug dump stringtable
 */

#if defined(DUK_USE_DEBUG)
DUK_INTERNAL void duk_heap_dump_strtab(duk_heap *heap) {
}
#endif  /* DUK_USE_DEBUG */
