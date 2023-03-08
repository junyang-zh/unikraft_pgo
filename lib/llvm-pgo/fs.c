#define pr_fmt(fmt)	"pgo: " fmt

#include <uk/essentials.h>
#include <uk/alloc.h>
#include <uk/plat/lcpu.h>
#include <uk/ctors.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stddef.h>
#include "prf_data.h"
#include "uk/pgo.h"

struct prf_private_data {
	void *buffer;
	unsigned long size;
};

/*
 * Raw profile data format:
 *
 *	- llvm_prf_header
 *	- __llvm_prf_data
 *	- __llvm_prf_cnts
 *	- __llvm_prf_names
 *	- zero padding to 8 bytes
 *	- for each llvm_prf_data in __llvm_prf_data:
 *		- llvm_prf_value_data
 *			- llvm_prf_value_record + site count array
 *				- llvm_prf_value_node_data
 *				...
 *			...
 *		...
 */

static void prf_fill_header(void **buffer)
{
	struct llvm_prf_header *header = *(struct llvm_prf_header **)buffer;

#if defined(CONFIG_ARCH_ARM_32)
	header->magic = LLVM_INSTR_PROF_RAW_MAGIC_32;
#else
	header->magic = LLVM_INSTR_PROF_RAW_MAGIC_64;
#endif
	header->version = LLVM_VARIANT_MASK_IR_PROF | LLVM_INSTR_PROF_RAW_VERSION;
	header->data_size = prf_data_count();
	header->padding_bytes_before_counters = 0;
	header->counters_size = prf_cnts_count();
	header->padding_bytes_after_counters = 0;
	header->names_size = prf_names_count();
	header->counters_delta = (__u64)__llvm_prf_cnts_start;
	header->names_delta = (__u64)__llvm_prf_names_start;
	header->value_kind_last = LLVM_INSTR_PROF_IPVK_LAST;

	*buffer += sizeof(*header);
}

/*
 * Copy the source into the buffer, incrementing the pointer into buffer in the
 * process.
 */
static void prf_copy_to_buffer(void **buffer, void *src, unsigned long size)
{
	memcpy(*buffer, src, size);
	*buffer += size;
}

static __u32 __prf_get_value_size(struct llvm_prf_data *p, __u32 *value_kinds)
{
	struct llvm_prf_value_node **nodes =
		(struct llvm_prf_value_node **)p->values;
	__u32 kinds = 0;
	__u32 size = 0;
	unsigned int kind;
	unsigned int n;
	unsigned int s = 0;

	for (kind = 0; kind < ARRAY_SIZE(p->num_value_sites); kind++) {
		unsigned int sites = p->num_value_sites[kind];

		if (!sites)
			continue;

		/* Record + site count array */
		size += prf_get_value_record_size(sites);
		kinds++;

		if (!nodes)
			continue;

		for (n = 0; n < sites; n++) {
			__u32 count = 0;
			struct llvm_prf_value_node *site = nodes[s + n];

			while (site && ++count <= __U8_MAX)
				site = site->next;

			size += count *
				sizeof(struct llvm_prf_value_node_data);
		}

		s += sites;
	}

	if (size)
		size += sizeof(struct llvm_prf_value_data);

	if (value_kinds)
		*value_kinds = kinds;

	return size;
}

static __u32 prf_get_value_size(void)
{
	__u32 size = 0;
	struct llvm_prf_data *p;

	for (p = __llvm_prf_data_start; p < __llvm_prf_data_end; p++)
		size += __prf_get_value_size(p, NULL);

	return size;
}

/* Serialize the profiling's value. */
static void prf_serialize_value(struct llvm_prf_data *p, void **buffer)
{
	struct llvm_prf_value_data header;
	struct llvm_prf_value_node **nodes =
		(struct llvm_prf_value_node **)p->values;
	unsigned int kind;
	unsigned int n;
	unsigned int s = 0;

	header.total_size = __prf_get_value_size(p, &header.num_value_kinds);

	if (!header.num_value_kinds)
		/* Nothing to write. */
		return;

	prf_copy_to_buffer(buffer, &header, sizeof(header));

	for (kind = 0; kind < ARRAY_SIZE(p->num_value_sites); kind++) {
		struct llvm_prf_value_record *record;
		__u8 *counts;
		unsigned int sites = p->num_value_sites[kind];

		if (!sites)
			continue;

		/* Profiling value record. */
		record = *(struct llvm_prf_value_record **)buffer;
		*buffer += prf_get_value_record_header_size();

		record->kind = kind;
		record->num_value_sites = sites;

		/* Site count array. */
		counts = *(__u8 **)buffer;
		*buffer += prf_get_value_record_site_count_size(sites);

		/*
		 * If we don't have nodes, we can skip updating the site count
		 * array, because the buffer is zero filled.
		 */
		if (!nodes)
			continue;

		for (n = 0; n < sites; n++) {
			__u32 count = 0;
			struct llvm_prf_value_node *site = nodes[s + n];

			while (site && ++count <= __U8_MAX) {
				prf_copy_to_buffer(buffer, site,
						   sizeof(struct llvm_prf_value_node_data));
				site = site->next;
			}

			counts[n] = (__u8)count;
		}

		s += sites;
	}
}

static void prf_serialize_values(void **buffer)
{
	struct llvm_prf_data *p;

	for (p = __llvm_prf_data_start; p < __llvm_prf_data_end; p++)
		prf_serialize_value(p, buffer);
}

static inline unsigned long prf_get_padding(unsigned long size)
{
	return 7 & (sizeof(__u64) - size % sizeof(__u64));
}

static unsigned long prf_buffer_size(void)
{
	return sizeof(struct llvm_prf_header) +
			prf_data_size()	+
			prf_cnts_size() +
			prf_names_size() +
			prf_get_padding(prf_names_size()) +
			prf_get_value_size();
}

/*
 * Serialize the profiling data into a format LLVM's tools can understand.
 * Note: caller *must* hold pgo_lock.
 */
static int prf_serialize(struct prf_private_data *p)
{
	int err = 0;
	void *buffer;

	p->size = prf_buffer_size();
	// FIXME: p->size may be larger than zalloc limit, try using vzalloc
	p->buffer = uk_zalloc(uk_alloc_get_default(), p->size);

	if (!p->buffer) {
		err = -ENOMEM;
		goto out;
	}

	buffer = p->buffer;

	prf_fill_header(&buffer);
	prf_copy_to_buffer(&buffer, __llvm_prf_data_start,  prf_data_size());
	prf_copy_to_buffer(&buffer, __llvm_prf_cnts_start,  prf_cnts_size());
	prf_copy_to_buffer(&buffer, __llvm_prf_names_start, prf_names_size());
	buffer += prf_get_padding(prf_names_size());

	prf_serialize_values(&buffer);

out:
	return err;
}

/* Creates a copy of the profiling data set. */
int llvm_perf_open(struct llvm_perf_file *file)
{
	struct prf_private_data *data;
	unsigned long flags;
	int err;

	data = uk_zalloc(uk_alloc_get_default(), sizeof(*data));
	if (!data) {
		err = -ENOMEM;
		goto out;
	}

	flags = prf_lock();

	err = prf_serialize(data);
	if (unlikely(err)) {
		uk_free(uk_alloc_get_default(), data);
		goto out_unlock;
	}

	file->private_data = data;

out_unlock:
	prf_unlock(flags);
out:
	return err;
}

/* read() implementation for PGO. */
ssize_t llvm_perf_read(struct llvm_perf_file *file, char *buf, size_t count, size_t off)
{
	struct prf_private_data *data = file->private_data;

	if (!data) {
		return -1;
	}

	size_t i;

	for (i = off; i < count && i < data->size; ++i) {
		buf[i] = ((char*)(data->buffer))[i];
	}

	return i;
}

/* release() implementation for PGO. Release resources allocated by open(). */
int llvm_perf_release(struct llvm_perf_file *file)
{
	struct prf_private_data *data = file->private_data;

	if (data) {
		uk_free(uk_alloc_get_default(), data->buffer);
		uk_free(uk_alloc_get_default(), data);
	}

	return 0;
}

/* Resetting PGO's profile data. */
ssize_t llvm_perf_reset()
{
	struct llvm_prf_data *data;

	memset(__llvm_prf_cnts_start, 0, prf_cnts_size());

	for (data = __llvm_prf_data_start; data < __llvm_prf_data_end; data++) {
		struct llvm_prf_value_node **vnodes;
		__u64 current_vsite_count;
		__u32 i;

		if (!data->values)
			continue;

		current_vsite_count = 0;
		vnodes = (struct llvm_prf_value_node **)data->values;

		for (i = LLVM_INSTR_PROF_IPVK_FIRST; i <= LLVM_INSTR_PROF_IPVK_LAST; i++)
			current_vsite_count += data->num_value_sites[i];

		for (i = 0; i < current_vsite_count; i++) {
			struct llvm_prf_value_node *current_vnode = vnodes[i];

			while (current_vnode) {
				current_vnode->count = 0;
				current_vnode = current_vnode->next;
			}
		}
	}

	return 0;
}

void *llvm_perf_dump_thread_fn(void *arg) {
	const char *file_name = "/redis.perfraw";
	size_t dump_period = 1;
	struct prf_private_data data;
	while (1) {
		int fd = open(file_name, O_RDWR | O_CREAT);
		unsigned long flags = prf_lock();
		prf_serialize(&data);
		prf_unlock(flags);
		write(fd, data.buffer, data.size);
		close(fd);
		uk_free(uk_alloc_get_default(), data.buffer);
		uk_pr_crit("[uk_pgo] %lu bytes wrote to \'%s\'\n", data.size, file_name);
		sleep(dump_period);
	}
}
