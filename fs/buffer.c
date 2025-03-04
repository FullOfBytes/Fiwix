/*
 * fiwix/fs/buffer.c
 *
 * Copyright 2018-2021, Jordi Sanfeliu. All rights reserved.
 * Distributed under the terms of the Fiwix License.
 */

/*
 * buffer.c implements a cache using the LRU (Least Recently Used) algorithm,
 * with a free list as a doubly circular linked list and a chained hash table
 * with doubly linked lists.
 *
 * hash table
 * +--------+  +--------------+  +--------------+  +--------------+
 * | index  |  |prev|data|next|  |prev|data|next|  |prev|data|next|
 * |   0   --> | /  |    | --->  <--- |    | --->  <--- |    |  / |
 * +--------+  +--------------+  +--------------+  +--------------+
 * +--------+  +--------------+  +--------------+  +--------------+
 * | index  |  |prev|data|next|  |prev|data|next|  |prev|data|next|
 * |   1   --> | /  |    | --->  <--- |    | --->  <--- |    |  / |
 * +--------+  +--------------+  +--------------+  +--------------+
 *              (buffer)          (buffer)          (buffer)
 *    ...
 */

#include <fiwix/asm.h>
#include <fiwix/kernel.h>
#include <fiwix/sleep.h>
#include <fiwix/sched.h>
#include <fiwix/buffer.h>
#include <fiwix/devices.h>
#include <fiwix/fs.h>
#include <fiwix/mm.h>
#include <fiwix/errno.h>
#include <fiwix/stdio.h>
#include <fiwix/string.h>
#include <fiwix/stat.h>

#define BUFFER_HASH(dev, block)	(((__dev_t)(dev) ^ (__blk_t)(block)) % (NR_BUF_HASH))
#define NR_BUFFERS	(buffer_table_size / sizeof(struct buffer))
#define NR_BUF_HASH	(buffer_hash_table_size / sizeof(unsigned int))

struct buffer *buffer_table;		/* buffer pool */
struct buffer *buffer_head;		/* buffer pool head */
struct buffer *buffer_dirty_head;
struct buffer **buffer_hash_table;

static struct resource sync_resource = { NULL, NULL };

static void insert_to_hash(struct buffer *buf)
{
	struct buffer **h;
	int i;

	i = BUFFER_HASH(buf->dev, buf->block);
	h = &buffer_hash_table[i];

	if(!*h) {
		*h = buf;
		(*h)->prev_hash = (*h)->next_hash = NULL;
	} else {
		buf->prev_hash = NULL;
		buf->next_hash = *h;
		(*h)->prev_hash = buf;
		*h = buf;
	}
}

static void remove_from_hash(struct buffer *buf)
{
	struct buffer **h;
	int i;

	i = BUFFER_HASH(buf->dev, buf->block);
	h = &buffer_hash_table[i];

	while(*h) {
		if(*h == buf) {
			if((*h)->next_hash) {
				(*h)->next_hash->prev_hash = (*h)->prev_hash;
			}
			if((*h)->prev_hash) {
				(*h)->prev_hash->next_hash = (*h)->next_hash;
			}
			if(h == &buffer_hash_table[i]) {
				*h = (*h)->next_hash;
			}
			break;
		}
		h = &(*h)->next_hash;
	}
}

static void insert_on_dirty_list(struct buffer *buf)
{
	if(buf->prev_dirty || buf->next_dirty) {
		return;
	}

	if(buffer_dirty_head) {
		buf->next_dirty = buffer_dirty_head;
		buffer_dirty_head->prev_dirty = buf;
	}
	buffer_dirty_head = buf;
	kstat.dirty += (PAGE_SIZE / 1024);
}

static void remove_from_dirty_list(struct buffer *buf)
{
	if(buf->next_dirty) {
		buf->next_dirty->prev_dirty = buf->prev_dirty;
	}
	if(buf->prev_dirty) {
		buf->prev_dirty->next_dirty = buf->next_dirty;
	}
	if(buf == buffer_dirty_head) {
		buffer_dirty_head = buf->next_dirty;
	}
	buf->prev_dirty = buf->next_dirty = NULL;
	buf->flags &= ~BUFFER_DIRTY;
	kstat.dirty -= (PAGE_SIZE / 1024);
}

static void insert_on_free_list(struct buffer *buf)
{
	if(!buffer_head) {
		buf->prev_free = buf->next_free = buf;
		buffer_head = buf;
	} else {
		buf->next_free = buffer_head;
		buf->prev_free = buffer_head->prev_free;
		buffer_head->prev_free->next_free = buf;
		buffer_head->prev_free = buf;

		/*
		 * If is marked as not valid then the buffer is
		 * placed at the beginning of the free list.
		 */
		if(!(buf->flags & BUFFER_VALID)) {
			buffer_head = buf;
		}
	}
}

static void remove_from_free_list(struct buffer *buf)
{
	if(!buffer_head) {
		return;
	}

	buf->prev_free->next_free = buf->next_free;
	buf->next_free->prev_free = buf->prev_free;
	if(buf == buffer_head) {
		buffer_head = buf->next_free;
	}

	if(buffer_head == buffer_head->next_free) {
		buffer_head = NULL;
	}
}

static void buffer_wait(struct buffer *buf)
{
	unsigned long int flags;

	for(;;) {
		SAVE_FLAGS(flags); CLI();
		if(buf->flags & BUFFER_LOCKED) {
			RESTORE_FLAGS(flags);
			sleep(&buffer_wait, PROC_UNINTERRUPTIBLE);
		} else {
			break;
		}
	}
	buf->flags |= BUFFER_LOCKED;
	RESTORE_FLAGS(flags);
}

static struct buffer * get_free_buffer(void)
{
	unsigned long int flags;
	struct buffer *buf;

	/* no more buffers on free list */
	if(!buffer_head) {
		return NULL;
	}

	for(;;) {
		SAVE_FLAGS(flags); CLI();
		buf = buffer_head;
		if(buf->flags & BUFFER_LOCKED) {
			RESTORE_FLAGS(flags);
			sleep(&buffer_wait, PROC_UNINTERRUPTIBLE);
		} else {
			break;
		}
	}

	remove_from_free_list(buf);
	buf->flags |= BUFFER_LOCKED;

	RESTORE_FLAGS(flags);
	return buf;
}

static void sync_one_buffer(struct buffer *buf)
{
	struct device *d;
	int errno;

	if(!(d = get_device(BLK_DEV, buf->dev))) {
		printk("WARNING: %s(): block device %d,%d not registered!\n", __FUNCTION__, MAJOR(buf->dev), MINOR(buf->dev));
		return;
	}

	if(d->fsop && d->fsop->write_block) {
		errno = d->fsop->write_block(buf->dev, buf->block, buf->data, buf->size);
		if(errno < 0) {
			if(errno == -EROFS) {
				printk("WARNING: %s(): write protection on device %d,%d.\n", __FUNCTION__, MAJOR(buf->dev), MINOR(buf->dev), buf->block);
			} else {
				printk("WARNING: %s(): I/O error on device %d,%d.\n", __FUNCTION__, MAJOR(buf->dev), MINOR(buf->dev), buf->block);
			}
			return;
		}
		remove_from_dirty_list(buf);
	} else {
		printk("WARNING: %s(): device %d,%d does not have the write_block() method!\n", __FUNCTION__, MAJOR(buf->dev), MINOR(buf->dev));
	}
}

static struct buffer * search_buffer_hash(__dev_t dev, __blk_t block, int size)
{
	struct buffer *buf;
	int i;

	i = BUFFER_HASH(dev, block);
	buf = buffer_hash_table[i];

	while(buf) {
		if(buf->dev == dev && buf->block == block && buf->size == size) {
			return buf;
		}
		buf = buf->next_hash;
	}

	return NULL;
}

static struct buffer * getblk(__dev_t dev, __blk_t block, int size)
{
	unsigned long int flags;
	struct buffer *buf;

	for(;;) {
		if((buf = search_buffer_hash(dev, block, size))) {
			SAVE_FLAGS(flags); CLI();
			if(buf->flags & BUFFER_LOCKED) {
				RESTORE_FLAGS(flags);
				sleep(&buffer_wait, PROC_UNINTERRUPTIBLE);
				continue;
			}
			buf->flags |= BUFFER_LOCKED;
			remove_from_free_list(buf);
			RESTORE_FLAGS(flags);
			return buf;
		}

		if(!(buf = get_free_buffer())) {
			printk("WARNING: %s(): no more buffers on free list!\n", __FUNCTION__);
			sleep(&get_free_buffer, PROC_UNINTERRUPTIBLE);
			continue;
		}

		if(buf->flags & BUFFER_DIRTY) {
			sync_one_buffer(buf);
		} else {
			if(!buf->data) {
				if(!(buf->data = (char *)kmalloc())) {
					brelse(buf);
					printk("%s(): returning NULL\n", __FUNCTION__);
					return NULL;
				}
				kstat.buffers += (PAGE_SIZE / 1024);
			}
		}

		SAVE_FLAGS(flags); CLI();
		remove_from_hash(buf);	/* remove it from old hash */
		buf->dev = dev;
		buf->block = block;
		buf->size = size;
		insert_to_hash(buf);
		buf->flags &= ~BUFFER_VALID;
		RESTORE_FLAGS(flags);
		return buf;
	}
}

struct buffer * bread(__dev_t dev, __blk_t block, int size)
{
	struct buffer *buf;
	struct device *d;

	if(!(d = get_device(BLK_DEV, dev))) {
		printk("WARNING: %s(): device major %d not found!\n", __FUNCTION__, MAJOR(dev));
		return NULL;
	}

	if((buf = getblk(dev, block, size))) {
		if(!(buf->flags & BUFFER_VALID)) {
			if(d->fsop && d->fsop->read_block) {
				if(d->fsop->read_block(dev, block, buf->data, size) >= 0) {
					buf->flags |= BUFFER_VALID;
				}
			}
		}
		if(buf->flags & BUFFER_VALID) {
			return buf;
		}
		brelse(buf);
	}
	
	printk("WARNING: %s(): returning NULL!\n", __FUNCTION__);
	return NULL;
}

void bwrite(struct buffer *buf)
{
	buf->flags |= (BUFFER_DIRTY | BUFFER_VALID);
	brelse(buf);
}

void brelse(struct buffer *buf)
{
	unsigned long int flags;

	SAVE_FLAGS(flags); CLI();

	if(buf->flags & BUFFER_DIRTY) {
		insert_on_dirty_list(buf);
	}

	insert_on_free_list(buf);
	buf->flags &= ~BUFFER_LOCKED;

	RESTORE_FLAGS(flags);

	wakeup(&get_free_buffer);
	wakeup(&buffer_wait);
}

void sync_buffers(__dev_t dev)
{
	struct buffer *buf, *next;

	buf = buffer_dirty_head;

	lock_resource(&sync_resource);
	while(buf) {
		next = buf->next_dirty;
		if(!dev || buf->dev == dev) {
			buffer_wait(buf);
			sync_one_buffer(buf);
			buf->flags &= ~BUFFER_LOCKED;
			wakeup(&buffer_wait);
		}
		buf = next;
	}
	unlock_resource(&sync_resource);
}

void invalidate_buffers(__dev_t dev)
{
	unsigned long int flags;
	unsigned int n;
	struct buffer *buf;

	buf = &buffer_table[0];
	SAVE_FLAGS(flags); CLI();

	for(n = 0; n < NR_BUFFERS; n++) {
		if(!(buf->flags & BUFFER_LOCKED) && buf->dev == dev) {
			buffer_wait(buf);
			remove_from_hash(buf);
			buf->flags &= ~(BUFFER_VALID | BUFFER_LOCKED);
			wakeup(&buffer_wait);
		}
		buf++;
	}

	RESTORE_FLAGS(flags);
	/* FIXME: invalidate_pages(dev); */
}

/*
 * When kernel runs out of pages, kswapd is awaken and it calls this function
 * which goes throught the buffer cache, freeing up to NR_BUF_RECLAIM buffers.
 */
int reclaim_buffers(void)
{
	struct buffer *buf, *first;
	int reclaimed;

	reclaimed = 0;
	first = NULL;

	for(;;) {
		if(!(buf = get_free_buffer())) {
			printk("WARNING: %s(): no more buffers on free list!\n", __FUNCTION__);
			sleep(&get_free_buffer, PROC_UNINTERRUPTIBLE);
			continue;
		}

		if(buf->flags & BUFFER_DIRTY) {
			sync_one_buffer(buf);
		}

		/* this ensures the buffer will go to the tail */
		buf->flags |= BUFFER_VALID;

		if(first) {
			if(first == buf) {
				brelse(buf);
				break;
			}
		} else {
			first = buf;
		}
		if(buf->data) {
			kfree((unsigned int)buf->data);
			buf->data = NULL;
			remove_from_hash(buf);
			kstat.buffers -= (PAGE_SIZE / 1024);
			reclaimed++;
			if(reclaimed == NR_BUF_RECLAIM) {
				brelse(buf);
				break;
			}
		}
		brelse(buf);
	}

	wakeup(&buffer_wait);

	/*
	 * If the total number of buffers reclaimed was less or equal to
	 * NR_BUF_RECLAIM, then wakeup any process waiting for a new page
	 * because release_page() won't do it.
	 */
	if(reclaimed && reclaimed <= NR_BUF_RECLAIM) {
		wakeup(&get_free_page);
	}

	return reclaimed;
}

void buffer_init(void)
{
	struct buffer *buf;
	unsigned int n;

	memset_b(buffer_table, NULL, buffer_table_size);
	memset_b(buffer_hash_table, NULL, buffer_hash_table_size);

	for(n = 0; n < NR_BUFFERS; n++) {
		buf = &buffer_table[n];
		insert_on_free_list(buf);
	}
}
