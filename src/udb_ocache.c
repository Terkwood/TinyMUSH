/* udb_ocache.c - LRU caching */

#include "copyright.h"
#include "config.h"
#include "system.h"

#include "typedefs.h"  /* required by mudconf */
#include "game.h"      /* required by mudconf */
#include "alloc.h"     /* required by mudconf */
#include "flags.h"     /* required by mudconf */
#include "htab.h"      /* required by mudconf */
#include "ltdl.h"      /* required by mudconf */
#include "udb.h"       /* required by mudconf */
#include "udb_defs.h"  /* required by mudconf */
#include "mushconf.h"  /* required by code */
#include "db.h"        /* required by externs */
#include "interface.h" /* required by code */
#include "externs.h"   /* required by code */
#include "udb.h"       /* required by code */
#include "udb_defs.h"  /* required by code */

#define NAMECMP(a, b, c, d, e) ((d == e) && !memcmp(a, b, c))

#define INSHEAD(q, e)          \
    if (q->head == (UDB_CACHE *)0) \
    {                          \
        q->tail = e;           \
        e->nxt = (UDB_CACHE *)0;   \
    }                          \
    else                       \
    {                          \
        e->nxt = q->head;      \
    }                          \
    q->head = e;

#define INSTAIL(q, e)          \
    if (q->head == (UDB_CACHE *)0) \
    {                          \
        q->head = e;           \
    }                          \
    else                       \
    {                          \
        q->tail->nxt = e;      \
    }                          \
    q->tail = e;               \
    e->nxt = (UDB_CACHE *)0;

#define F_DEQUEUE(q, e)                       \
    if (e->nxtfree == (UDB_CACHE *)0)             \
    {                                         \
        if (e->prvfree != (UDB_CACHE *)0)         \
        {                                     \
            e->prvfree->nxtfree = (UDB_CACHE *)0; \
        }                                     \
        q->tail = e->prvfree;                 \
    }                                         \
    if (e->prvfree == (UDB_CACHE *)0)             \
    {                                         \
        q->head = e->nxtfree;                 \
        if (e->nxtfree)                       \
            e->nxtfree->prvfree = (UDB_CACHE *)0; \
    }                                         \
    else                                      \
    {                                         \
        e->prvfree->nxtfree = e->nxtfree;     \
        if (e->nxtfree)                       \
            e->nxtfree->prvfree = e->prvfree; \
    }

#define F_INSHEAD(q, e)          \
    if (q->head == (UDB_CACHE *)0)   \
    {                            \
        q->tail = e;             \
        e->nxtfree = (UDB_CACHE *)0; \
    }                            \
    else                         \
    {                            \
        e->nxtfree = q->head;    \
        e->nxtfree->prvfree = e; \
    }                            \
    e->prvfree = (UDB_CACHE *)0;     \
    q->head = e;

#define F_INSTAIL(q, e)        \
    if (q->head == (UDB_CACHE *)0) \
    {                          \
        q->head = e;           \
    }                          \
    else                       \
    {                          \
        q->tail->nxtfree = e;  \
    }                          \
    e->prvfree = q->tail;      \
    q->tail = e;               \
    e->nxtfree = (UDB_CACHE *)0;

UDB_CACHE *get_free_entry(int);

/* initial settings for cache sizes */

int cwidth = CACHE_WIDTH;

/* sys_c points to all cache lists */

UDB_CHAIN *sys_c;

/* freelist points to an alternate linked list kept in LRU order */

UDB_CHAIN *freelist;
int cache_initted = 0;
int cache_frozen = 0;

/* cache stats */

time_t cs_ltime;
int cs_writes = 0;   /* total writes */
int cs_reads = 0;    /* total reads */
int cs_dbreads = 0;  /* total read-throughs */
int cs_dbwrites = 0; /* total write-throughs */
int cs_dels = 0;     /* total deletes */
int cs_checks = 0;   /* total checks */
int cs_rhits = 0;    /* total reads filled from cache */
int cs_ahits = 0;    /* total reads filled active cache */
int cs_whits = 0;    /* total writes to dirty cache */
int cs_fails = 0;    /* attempts to grab nonexistent */
int cs_syncs = 0;    /* total cache syncs */
int cs_size = 0;     /* total cache size */

int cachehash(void *keydata, int keylen, unsigned int type)
{
    unsigned int hash = 0;
    char *sp;

    if (keydata == NULL)
    {
        return 0;
    }

    for (sp = (char *)keydata; (sp - (char *)keydata) < keylen; sp++)
    {
        hash = (hash << 5) + hash + *sp;
    }

    return ((hash + type) % cwidth);
}

void cache_repl(UDB_CACHE *cp, void *new, int len, unsigned int type, unsigned int flags)
{
    cs_size -= cp->datalen;

    if (cp->data != NULL)
    {
        XFREE(cp->data);
    }

    cp->data = new;
    cp->datalen = len;
    cp->type = type;
    cp->flags = flags;
    cs_size += cp->datalen;
}

int cache_init(int width)
{
    int x;
    UDB_CHAIN *sp;

    if (cache_initted || sys_c != (UDB_CHAIN *)0)
    {
        return (0);
    }

    /*
     * If width is specified as non-zero, change it to that,
     * * otherwise use default.
     */

    if (width)
    {
        cwidth = width;
    }

    sp = sys_c = (UDB_CHAIN *)XMALLOC((unsigned)cwidth * sizeof(UDB_CHAIN), "sys_c");

    if (sys_c == (UDB_CHAIN *)0)
    {
        warning("cache_init: cannot allocate cache: ", (char *)-1, "\n", (char *)0);
        return (-1);
    }

    freelist = (UDB_CHAIN *)XMALLOC(sizeof(UDB_CHAIN), "freelist");

    /*
     * Allocate the initial cache entries
     */

    for (x = 0; x < cwidth; x++, sp++)
    {
        sp->head = (UDB_CACHE *)0;
        sp->tail = (UDB_CACHE *)0;
    }

    /*
     * Init the LRU freelist
     */
    freelist->head = (UDB_CACHE *)0;
    freelist->tail = (UDB_CACHE *)0;

    /*
     * Initialize the object pipelines
     */

    for (x = 0; x < NUM_OBJPIPES; x++)
    {
        mudstate.objpipes[x] = NULL;
    }

    /*
     * Initialize the object access counter
     */
    mudstate.objc = 0;
    /*
     * mark caching system live
     */
    cache_initted++;
    cs_ltime = time(NULL);
    return (0);
}

void cache_reset(void)
{
    int x;
    UDB_CACHE *cp, *nxt;
    UDB_CHAIN *sp;
    UDB_DATA key, data;
    /*
     * Clear the cache after startup and reset stats
     */
    db_lock();

    for (x = 0; x < cwidth; x++, sp++)
    {
        sp = &sys_c[x];

        /*
	 * traverse the chain
	 */
        for (cp = sp->head; cp != NULL; cp = nxt)
        {
            nxt = cp->nxt;

            if (cp->flags & CACHE_DIRTY)
            {
                if (cp->data == NULL)
                {
                    switch (cp->type)
                    {
                    case DBTYPE_ATTRIBUTE:
                        pipe_del_attrib(((UDB_ANAME *)cp->keydata)->attrnum, ((UDB_ANAME *)cp->keydata)->object);
                        break;

                    default:
                        key.dptr = cp->keydata;
                        key.dsize = cp->keylen;
                        db_del(key, cp->type);
                    }

                    cs_dels++;
                }
                else
                {
                    switch (cp->type)
                    {
                    case DBTYPE_ATTRIBUTE:
                        pipe_set_attrib(((UDB_ANAME *)cp->keydata)->attrnum, ((UDB_ANAME *)cp->keydata)->object, (char *)cp->data);
                        break;

                    default:
                        key.dptr = cp->keydata;
                        key.dsize = cp->keylen;
                        data.dptr = cp->data;
                        data.dsize = cp->datalen;
                        db_put(key, data, cp->type);
                    }

                    cs_dbwrites++;
                }
            }

            cache_repl(cp, NULL, 0, DBTYPE_EMPTY, 0);
            XFREE(cp->keydata);
            XFREE(cp);
        }

        sp->head = (UDB_CACHE *)0;
        sp->tail = (UDB_CACHE *)0;
    }

    freelist->head = (UDB_CACHE *)0;
    freelist->tail = (UDB_CACHE *)0;
    db_unlock();
    /*
     * Clear the counters after startup, or they'll be skewed
     */
    cs_writes = 0;   /* total writes */
    cs_reads = 0;    /* total reads */
    cs_dbreads = 0;  /* total read-throughs */
    cs_dbwrites = 0; /* total write-throughs */
    cs_dels = 0;     /* total deletes */
    cs_checks = 0;   /* total checks */
    cs_rhits = 0;    /* total reads filled from cache */
    cs_ahits = 0;    /* total reads filled active cache */
    cs_whits = 0;    /* total writes to dirty cache */
    cs_fails = 0;    /* attempts to grab nonexistent */
    cs_syncs = 0;    /* total cache syncs */
    cs_size = 0;     /* size of cache in bytes */
}

/* list dbrefs of objects in the cache. */

void list_cached_objs(dbref player)
{
    UDB_CHAIN *sp;
    UDB_CACHE *cp;
    int x;
    int aco, maco, asize, msize, oco, moco;
    int *count_array, *size_array;
    char *s;
    aco = maco = asize = msize = oco = moco = 0;
    count_array = (int *)XCALLOC(mudstate.db_top, sizeof(int), "count_array");
    size_array = (int *)XCALLOC(mudstate.db_top, sizeof(int), "size_array");

    for (x = 0, sp = sys_c; x < cwidth; x++, sp++)
    {
        for (cp = sp->head; cp != NULL; cp = cp->nxt)
        {
            if (cp->data && (cp->type == DBTYPE_ATTRIBUTE) && !(cp->flags & CACHE_DIRTY))
            {
                aco++;
                asize += cp->datalen;
                count_array[((UDB_ANAME *)cp->keydata)->object] += 1;
                size_array[((UDB_ANAME *)cp->keydata)->object] += cp->datalen;
            }
        }
    }

    raw_notify(player, NULL, "Active Cache:");
    raw_notify(player, NULL, "Name                            Dbref    Attrs      Size");
    raw_notify(player, NULL, "========================================================");

    for (x = 0; x < mudstate.db_top; x++)
    {
        if (count_array[x] > 0)
        {
            s = strip_ansi(Name(x));
            raw_notify(player, "%-30.30s  #%-6d  %5d  %8d", s, x, count_array[x], size_array[x]);
            XFREE(s);
            oco++;
            count_array[x] = 0;
            size_array[x] = 0;
        }
    }

    raw_notify(player, NULL, "\nModified Active Cache:");
    raw_notify(player, NULL, "Name                            Dbref    Attrs      Size");
    raw_notify(player, NULL, "========================================================");

    for (x = 0, sp = sys_c; x < cwidth; x++, sp++)
    {
        for (cp = sp->head; cp != NULL; cp = cp->nxt)
        {
            if (cp->data && (cp->type == DBTYPE_ATTRIBUTE) && (cp->flags & CACHE_DIRTY))
            {
                maco++;
                msize += cp->datalen;
                count_array[((UDB_ANAME *)cp->keydata)->object] += 1;
                size_array[((UDB_ANAME *)cp->keydata)->object] += cp->datalen;
            }
        }
    }

    for (x = 0; x < mudstate.db_top; x++)
    {
        if (count_array[x] > 0)
        {
            s = strip_ansi(Name(x));
            raw_notify(player, "%-30.30s  #%-6d  %5d  %8d", s, x, count_array[x], size_array[x]);
            XFREE(s);
            moco++;
        }
    }

    raw_notify(player, "\nTotals: active %d (%d attrs), modified active %d (%d attrs), total attrs %d", oco, aco, moco, maco, aco + maco);
    raw_notify(player, "Size: active %d bytes, modified active %d bytes", asize, msize);
    XFREE(count_array);
    XFREE(size_array);
}

void list_cached_attrs(dbref player)
{
    UDB_CHAIN *sp;
    UDB_CACHE *cp;
    int x;
    int aco, maco, asize, msize;
    ATTR *atr;
    aco = maco = asize = msize = 0;
    raw_notify(player, NULL, "Active Cache:");
    raw_notify(player, NULL, "Name                    Attribute                       Dbref   Size");
    raw_notify(player, NULL, "====================================================================");

    for (x = 0, sp = sys_c; x < cwidth; x++, sp++)
    {
        for (cp = sp->head; cp != NULL; cp = cp->nxt)
        {
            if (cp->data && (cp->type == DBTYPE_ATTRIBUTE) && !(cp->flags & CACHE_DIRTY))
            {
                aco++;
                asize += cp->datalen;
                atr = atr_num(((UDB_ANAME *)cp->keydata)->attrnum);
                raw_notify(player, "%-23.23s %-31.31s #%-6d %6d", PureName(((UDB_ANAME *)cp->keydata)->object), (atr ? atr->name : "(Unknown)"), ((UDB_ANAME *)cp->keydata)->object, cp->datalen);
            }
        }
    }

    raw_notify(player, NULL, "\nModified Active Cache:");
    raw_notify(player, NULL, "Name                    Attribute                       Dbref   Size");
    raw_notify(player, NULL, "====================================================================");

    for (x = 0, sp = sys_c; x < cwidth; x++, sp++)
    {
        for (cp = sp->head; cp != NULL; cp = cp->nxt)
        {
            if (cp->data && (cp->type == DBTYPE_ATTRIBUTE) && (cp->flags & CACHE_DIRTY))
            {
                maco++;
                msize += cp->datalen;
                atr = atr_num(((UDB_ANAME *)cp->keydata)->attrnum);
                raw_notify(player, "%-23.23s %-31.31s #%-6d %6d", PureName(((UDB_ANAME *)cp->keydata)->object), (atr ? atr->name : "(Unknown)"), ((UDB_ANAME *)cp->keydata)->object, cp->datalen);
            }
        }
    }

    raw_notify(player, "\nTotals: active %d, modified active %d, total attributes %d", aco, maco, aco + maco);
    raw_notify(player, "Size: active %d bytes, modified active %d bytes", asize, msize);
}

/* Search the cache for an entry of a specific type, if found, copy the data
 * and length into pointers provided by the caller, if not, fetch from DB.
 * You do not need to free data returned by this call. */

UDB_DATA cache_get(UDB_DATA key, unsigned int type)
{
    UDB_CACHE *cp;
    UDB_CHAIN *sp;
    int hv = 0;
    UDB_DATA data;
#ifdef MEMORY_BASED
    char *cdata;
#endif

    if (!key.dptr || !cache_initted)
    {
        data.dptr = NULL;
        data.dsize = 0;
        return data;
    }

    /*
     * If we're dumping, ignore stats - activity during a dump skews the
     * * working set. We make sure in get_free_entry that any activity
     * * resulting from a dump does not push out entries that are already
     * * in the cache
     */
#ifndef MEMORY_BASED

    if (!mudstate.standalone && !mudstate.dumping)
    {
        cs_reads++;
    }
#endif
#ifdef MEMORY_BASED

    if (type == DBTYPE_ATTRIBUTE)
    {
        goto skipcacheget;
    }
#endif
    hv = cachehash(key.dptr, key.dsize, type);
    sp = &sys_c[hv];

    for (cp = sp->head; cp != NULL; cp = cp->nxt)
    {
        if (NAMECMP(key.dptr, cp->keydata, key.dsize, type, cp->type))
        {
            if (!mudstate.standalone && !mudstate.dumping)
            {
                cs_rhits++;
                cs_ahits++;
            }

            F_DEQUEUE(freelist, cp);
            F_INSTAIL(freelist, cp);
            data.dptr = cp->data;
            data.dsize = cp->datalen;
            return data;
        }
    }

#ifdef MEMORY_BASED
skipcacheget:
#endif

    /*
     * DARN IT - at this point we have a certified, type-A cache miss
     */

    /*
     * Grab the data from wherever
     */

    switch (type)
    {
    case DBTYPE_ATTRIBUTE:
#ifdef MEMORY_BASED
        cdata = obj_get_attrib(((UDB_ANAME *)key.dptr)->attrnum, &(db[((UDB_ANAME *)key.dptr)->object].attrtext));

        if (cdata)
        {
            data.dptr = cdata;
            data.dsize = strlen(cdata) + 1;
            return data;
        }
#endif
        data.dptr = (void *)pipe_get_attrib(((UDB_ANAME *)key.dptr)->attrnum, ((UDB_ANAME *)key.dptr)->object);

        if (data.dptr == NULL)
        {
            data.dsize = 0;
        }
        else
        {
            data.dsize = strlen(data.dptr) + 1;
        }

#ifdef MEMORY_BASED

        if (!mudstate.standalone && !mudstate.dumping)
        {
            cs_dbreads++;
        }

        if (data.dptr)
        {
            data.dsize = strlen(data.dptr) + 1;
            cdata = XMALLOC(data.dsize, "cdata");
            XMEMCPY((void *)cdata, (void *)data.dptr, data.dsize);
            obj_set_attrib(((UDB_ANAME *)key.dptr)->attrnum, &(db[((UDB_ANAME *)key.dptr)->object].attrtext), cdata);
            data.dptr = cdata;
            return data;
        }
        else
        {
            data.dptr = NULL;
            data.dsize = 0;
            return data;
        }

#endif
        break;

    default:
        data = db_get(key, type);
    }

    if (!mudstate.standalone && !mudstate.dumping)
    {
        cs_dbreads++;
    }

    if (data.dptr == NULL)
    {
        return data;
    }

    if ((cp = get_free_entry(data.dsize)) == NULL)
    {
        return data;
    }

    cp->keydata = (void *)XMALLOC(key.dsize, "cp->keydata");
    XMEMCPY(cp->keydata, key.dptr, key.dsize);
    cp->keylen = key.dsize;
    cp->data = data.dptr;
    cp->datalen = data.dsize;
    cp->type = type;
    cp->flags = 0;
    /*
     * If we're dumping, we'll put everything we fetch that is not
     * already in cache at the end of the chain and set its last
     * referenced time to zero. This will ensure that we won't blow away
     * what's already in cache, since get_free_entry will just reuse
     * these entries.
     */
    cs_size += cp->datalen;

    if (mudstate.dumping)
    {
        /*
	 * Link at head of chain
	 */
        INSHEAD(sp, cp);
        /*
	 * Link at head of LRU freelist
	 */
        F_INSHEAD(freelist, cp);
    }
    else
    {
        /*
	 * Link at tail of chain
	 */
        INSTAIL(sp, cp);
        /*
	 * Link at tail of LRU freelist
	 */
        F_INSTAIL(freelist, cp);
    }

    return data;
}

/*
 * put an attribute back into the cache. this is complicated by the
 * fact that when a function calls this with an object, the object
 * is *already* in the cache, since calling functions operate on
 * pointers to the cached objects, and may or may not be actively
 * modifying them. in other words, by the time the object is handed
 * to cache_put, it has probably already been modified, and the cached
 * version probably already reflects those modifications!
 *
 * so - we do a couple of things: we make sure that the cached object is
 * actually there, and move it to the modified chain. if we can't find it -
 * either we have a (major) programming error, or the
 * *name* of the object has been changed, or the object is a totally
 * new creation someone made and is inserting into the world.
 *
 * in the case of totally new creations, we simply accept the pointer
 * to the object and add it into our environment. freeing it becomes
 * the responsibility of the cache code. DO NOT HAND A POINTER TO
 * CACHE_PUT AND THEN FREE IT YOURSELF!!!!
 *
 */

int cache_put(UDB_DATA key, UDB_DATA data, unsigned int type)
{
    UDB_CACHE *cp;
    UDB_CHAIN *sp;
    int hv = 0;
#ifdef MEMORY_BASED
    char *cdata;
#endif

    if (!key.dptr || !data.dptr || !cache_initted)
    {
        return (1);
    }

    /*
     * Call module API hook
     */

    for (MODULE *cam__mp = mudstate.modules_list; cam__mp != NULL; cam__mp = cam__mp->next)
    {
        if (cam__mp->cache_put_notify)
        {
            (*(cam__mp->cache_put_notify))(key, type);
        }
    }

#ifndef MEMORY_BASED
    if (mudstate.standalone)
    {
#endif

        /*
	 * Bypass the cache when standalone or memory based for writes
	 */
        if (data.dptr == NULL)
        {
            switch (type)
            {
            case DBTYPE_ATTRIBUTE:
                pipe_del_attrib(((UDB_ANAME *)key.dptr)->attrnum, ((UDB_ANAME *)key.dptr)->object);
#ifdef MEMORY_BASED
                obj_del_attrib(((UDB_ANAME *)key.dptr)->attrnum, &(db[((UDB_ANAME *)key.dptr)->object].attrtext));
#endif
                break;

            default:
                db_lock();
                db_del(key, type);
                db_unlock();
            }
        }
        else
        {
            switch (type)
            {
            case DBTYPE_ATTRIBUTE:
                pipe_set_attrib(((UDB_ANAME *)key.dptr)->attrnum, ((UDB_ANAME *)key.dptr)->object, (char *)data.dptr);
#ifdef MEMORY_BASED
                cdata = XMALLOC(data.dsize, "cdata");
                XMEMCPY((void *)cdata, (void *)data.dptr, data.dsize);
                obj_set_attrib(((UDB_ANAME *)key.dptr)->attrnum, &(db[((UDB_ANAME *)key.dptr)->object].attrtext), cdata);
#endif
                /*
		 * Don't forget to free data.dptr when standalone
		 */
                XFREE(data.dptr);
                break;

            default:
                db_lock();
                db_put(key, data, type);
                db_unlock();
            }
        }

        return (0);
#ifndef MEMORY_BASED
    }
#endif
    cs_writes++;
    /*
     * generate hash
     */
    hv = cachehash(key.dptr, key.dsize, type);
    sp = &sys_c[hv];

    /*
     * step one, search chain, and if we find the obj, dirty it
     */

    for (cp = sp->head; cp != NULL; cp = cp->nxt)
    {
        if (NAMECMP(key.dptr, cp->keydata, key.dsize, type, cp->type))
        {
            if (!mudstate.dumping)
            {
                cs_whits++;
            }

            if (cp->data != data.dptr)
            {
                cache_repl(cp, data.dptr, data.dsize, type, CACHE_DIRTY);
            }

            F_DEQUEUE(freelist, cp);
            F_INSTAIL(freelist, cp);
            return (0);
        }
    }

    /*
     * Add a new entry to the cache
     */

    if ((cp = get_free_entry(data.dsize)) == NULL)
    {
        return (1);
    }

    cp->keydata = (void *)XMALLOC(key.dsize, "cp->keydata");
    XMEMCPY(cp->keydata, key.dptr, key.dsize);
    cp->keylen = key.dsize;
    cp->data = data.dptr;
    cp->datalen = data.dsize;
    cp->type = type;
    cp->flags = CACHE_DIRTY;
    cs_size += cp->datalen;
    /*
     * link at tail of chain
     */
    INSTAIL(sp, cp);
    /*
     * link at tail of LRU freelist
     */
    F_INSTAIL(freelist, cp);
    return (0);
}

UDB_CACHE *get_free_entry(int atrsize)
{
    UDB_DATA key, data;
    UDB_CHAIN *sp;
    UDB_CACHE *cp, *p, *prv;
    int hv;

    /*
     * Flush entries from the cache until there's enough room for
     * * this one. The max size can be dynamically changed-- if it is too
     * * small, the MUSH will flush entries until the cache fits within
     * * this size and if it is too large, we'll fill it up before we
     * * start flushing
     */

    while ((cs_size + atrsize) > (mudconf.cache_size ? mudconf.cache_size : CACHE_SIZE))
    {
        cp = freelist->head;

        if (cp)
        {
            F_DEQUEUE(freelist, cp);
        }

        if (cp && (cp->flags & CACHE_DIRTY))
        {
            /*
	     * Flush the modified attributes to disk
	     */
            if (cp->data == NULL)
            {
                switch (cp->type)
                {
                case DBTYPE_ATTRIBUTE:
                    pipe_del_attrib(((UDB_ANAME *)cp->keydata)->attrnum, ((UDB_ANAME *)cp->keydata)->object);
                    break;

                default:
                    key.dptr = cp->keydata;
                    key.dsize = cp->keylen;
                    db_lock();
                    db_del(key, cp->type);
                    db_unlock();
                }

                cs_dels++;
            }
            else
            {
                switch (cp->type)
                {
                case DBTYPE_ATTRIBUTE:
                    pipe_set_attrib(((UDB_ANAME *)cp->keydata)->attrnum, ((UDB_ANAME *)cp->keydata)->object, (char *)cp->data);
                    break;

                default:
                    key.dptr = cp->keydata;
                    key.dsize = cp->keylen;
                    data.dptr = cp->data;
                    data.dsize = cp->datalen;
                    db_lock();
                    db_put(key, data, cp->type);
                    db_unlock();
                }

                cs_dbwrites++;
            }
        }

        /*
	 * Take the attribute off of its chain and nuke the
	 * attribute's memory
	 */

        if (cp)
        {
            /*
	     * Find the cache entry inside the real cache
	     */
            hv = cachehash(cp->keydata, cp->keylen, cp->type);
            sp = &sys_c[hv];
            prv = NULL;

            for (p = sp->head; p != NULL; p = p->nxt)
            {
                if (cp == p)
                {
                    break;
                }

                prv = p;
            }

            /*
	     * Remove the cache entry
	     */
            if (cp->nxt == (UDB_CACHE *)0)
            {
                if (prv != (UDB_CACHE *)0)
                {
                    prv->nxt = (UDB_CACHE *)0;
                }
                sp->tail = prv;
            }
            if (prv == (UDB_CACHE *)0)
            {
                sp->head = cp->nxt;
            }
            else
            {
                prv->nxt = cp->nxt;
            }

            cache_repl(cp, NULL, 0, DBTYPE_EMPTY, 0);
            XFREE(cp->keydata);
            XFREE(cp);
        }

        cp = NULL;
    }

    /*
     * No valid cache entries to flush, allocate a new one
     */

    if ((cp = (UDB_CACHE *)XMALLOC(sizeof(UDB_CACHE), "cp")) == NULL)
    {
        fatal("cache get_free_entry: XMALLOC failed", (char *)-1, (char *)0);
    }

    cp->keydata = NULL;
    cp->keylen = 0;
    cp->data = NULL;
    cp->datalen = 0;
    cp->type = DBTYPE_EMPTY;
    cp->flags = 0;
    return (cp);
}

int cache_write(UDB_CACHE *cp)
{
    UDB_DATA key, data;

    /*
     * Write a single cache chain to disk
     */

    while (cp != NULL)
    {
        if (!(cp->flags & CACHE_DIRTY))
        {
            cp = cp->nxt;
            continue;
        }

        if (cp->data == NULL)
        {
            switch (cp->type)
            {
            case DBTYPE_ATTRIBUTE:
                pipe_del_attrib(((UDB_ANAME *)cp->keydata)->attrnum, ((UDB_ANAME *)cp->keydata)->object);
                break;

            default:
                key.dptr = cp->keydata;
                key.dsize = cp->keylen;
                db_del(key, cp->type);
            }

            cs_dels++;
        }
        else
        {
            switch (cp->type)
            {
            case DBTYPE_ATTRIBUTE:
                pipe_set_attrib(((UDB_ANAME *)cp->keydata)->attrnum, ((UDB_ANAME *)cp->keydata)->object, (char *)cp->data);
                break;

            default:
                key.dptr = cp->keydata;
                key.dsize = cp->keylen;
                data.dptr = cp->data;
                data.dsize = cp->datalen;
                db_put(key, data, cp->type);
            }

            cs_dbwrites++;
        }

        cp->flags = 0;
        cp = cp->nxt;
    }

    return (0);
}

int cache_sync(void)
{
    int x;
    UDB_CHAIN *sp;
    cs_syncs++;

    if (!cache_initted)
    {
        return (1);
    }

    if (cache_frozen)
    {
        return (0);
    }

    if (mudstate.standalone || mudstate.restarting)
    {
        /*
	 * If we're restarting or standalone, having DBM wait for
	 * * each write is a performance no-no; run asynchronously
	 */
        dddb_setsync(0);
    }

    /*
     * Lock the database
     */
    db_lock();

    for (x = 0, sp = sys_c; x < cwidth; x++, sp++)
    {
        if (cache_write(sp->head))
        {
            return (1);
        }
    }

    /*
     * Also sync the read and write object structures if they're dirty
     */
    attrib_sync();
    /*
     * Unlock the database
     */
    db_unlock();

    if (mudstate.standalone || mudstate.restarting)
    {
        dddb_setsync(1);
    }

    return (0);
}

void cache_del(UDB_DATA key, unsigned int type)
{
    UDB_CACHE *cp;
    UDB_CHAIN *sp;
    int hv = 0;

    if (!key.dptr || !cache_initted)
    {
        return;
    }

    /*
     * Call module API hook
     */

    for (MODULE *cam__mp = mudstate.modules_list; cam__mp != NULL; cam__mp = cam__mp->next)
    {
        if (cam__mp->cache_del_notify)
        {
            (*(cam__mp->cache_del_notify))(key, type);
        }
    }

#ifdef MEMORY_BASED
    if (type == DBTYPE_ATTRIBUTE)
    {
        pipe_del_attrib(((UDB_ANAME *)key.dptr)->attrnum, ((UDB_ANAME *)key.dptr)->object);
        obj_del_attrib(((UDB_ANAME *)key.dptr)->attrnum, &(db[((UDB_ANAME *)key.dptr)->object].attrtext));
        return;
    }
#endif
    cs_dels++;
    hv = cachehash(key.dptr, key.dsize, type);
    sp = &sys_c[hv];

    /*
     * mark dead in cache
     */

    for (cp = sp->head; cp != NULL; cp = cp->nxt)
    {
        if (NAMECMP(key.dptr, cp->keydata, key.dsize, type, cp->type))
        {
            F_DEQUEUE(freelist, cp);
            F_INSHEAD(freelist, cp);
            cache_repl(cp, NULL, 0, type, CACHE_DIRTY);
            return;
        }
    }

    if ((cp = get_free_entry(0)) == NULL)
    {
        return;
    }

    cp->keydata = (void *)XMALLOC(key.dsize, "cp->keydata");
    XMEMCPY(cp->keydata, key.dptr, key.dsize);
    cp->keylen = key.dsize;
    cp->type = type;
    cp->flags = CACHE_DIRTY;
    INSHEAD(sp, cp);
    F_INSHEAD(freelist, cp);
    return;
}
