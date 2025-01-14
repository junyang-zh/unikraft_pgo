#define pr_fmt(fmt)	"pgo: " fmt

//#include <linux/bitops.h>
//#include <linux/kernel.h>
//#include <linux/spinlock.h>
//#include <linux/types.h>
#include <uk/essentials.h>
#include <uk/bitops.h>
#include <uk/plat/spinlock.h>
#include <stddef.h>
#include "prf_data.h"

/*
 * This lock guards both profile count updating and serialization of the
 * profiling data. Keeping both of these activities separate via locking
 * ensures that we don't try to serialize data that's only partially updated.
 */
static __spinlock pgo_lock = UKARCH_SPINLOCK_INITIALIZER();
static int current_node;

unsigned long prf_lock(void)
{
	unsigned long flags;

	ukplat_spin_lock_irqsave(&pgo_lock, flags);

	return flags;
}

void prf_unlock(unsigned long flags)
{
	ukplat_spin_unlock_irqrestore(&pgo_lock, flags);
}

/*
 * Return a newly allocated profiling value node which contains the tracked
 * value by the value profiler.
 * Note: caller *must* hold pgo_lock.
 */
static struct llvm_prf_value_node *allocate_node(struct llvm_prf_data *p,
						 __u32 index, __u64 value)
{
	if (&__llvm_prf_vnds_start[current_node + 1] >= __llvm_prf_vnds_end)
		return NULL; /* Out of nodes */

	current_node++;

	/* Make sure the node is entirely within the section */
	if (&__llvm_prf_vnds_start[current_node] >= __llvm_prf_vnds_end ||
		&__llvm_prf_vnds_start[current_node + 1] > __llvm_prf_vnds_end)
		return NULL;

	return &__llvm_prf_vnds_start[current_node];
}

/*
 * Counts the number of times a target value is seen.
 *
 * Records the target value for the index if not seen before. Otherwise,
 * increments the counter associated w/ the target value.
 */
void __llvm_profile_instrument_target(__u64 target_value, void *data, __u32 index);
void __llvm_profile_instrument_target(__u64 target_value, void *data, __u32 index)
{
	struct llvm_prf_data *p = (struct llvm_prf_data *)data;
	struct llvm_prf_value_node **counters;
	struct llvm_prf_value_node *curr;
	struct llvm_prf_value_node *min = NULL;
	struct llvm_prf_value_node *prev = NULL;
	__u64 min_count = __U64_MAX;
	__u8 values = 0;
	unsigned long flags;

	if (!p || !p->values)
		return;

	counters = (struct llvm_prf_value_node **)p->values;
	curr = counters[index];

	while (curr) {
		if (target_value == curr->value) {
			curr->count++;
			return;
		}

		if (curr->count < min_count) {
			min_count = curr->count;
			min = curr;
		}

		prev = curr;
		curr = curr->next;
		values++;
	}

	if (values >= LLVM_INSTR_PROF_MAX_NUM_VAL_PER_SITE) {
		if (!min->count || !(--min->count)) {
			curr = min;
			curr->value = target_value;
			curr->count++;
		}
		return;
	}

	/* Lock when updating the value node structure. */
	flags = prf_lock();

	curr = allocate_node(p, index, target_value);
	if (!curr)
		goto out;

	curr->value = target_value;
	curr->count++;

	if (!counters[index])
		counters[index] = curr;
	else if (prev && !prev->next)
		prev->next = curr;

out:
	prf_unlock(flags);
}

/* Counts the number of times a range of targets values are seen. */
void __llvm_profile_instrument_range(__u64 target_value, void *data,
					 __u32 index, __s64 precise_start,
					 __s64 precise_last, __s64 large_value);
void __llvm_profile_instrument_range(__u64 target_value, void *data,
					 __u32 index, __s64 precise_start,
					 __s64 precise_last, __s64 large_value)
{
	if (large_value != __S64_MIN && (__s64)target_value >= large_value)
		target_value = large_value;
	else if ((__s64)target_value < precise_start ||
		 (__s64)target_value > precise_last)
		target_value = precise_last + 1;

	__llvm_profile_instrument_target(target_value, data, index);
}

static __u64 inst_prof_get_range_rep_value(__u64 value)
{
	if (value <= 8)
		/* The first ranges are individually tracked, use it as is. */
		return value;
	else if (value >= 513)
		/* The last range is mapped to its lowest value. */
		return 513;
	else if (uk_hweight64(value) == 1)
		/* If it's a power of two, use it as is. */
		return value;

	/* Otherwise, take to the previous power of two + 1. */
	return ((__u64)1 << (64 - __builtin_clzll(value) - 1)) + 1;
}

/*
 * The target values are partitioned into multiple ranges. The range spec is
 * defined in compiler-rt/include/profile/InstrProfData.inc.
 */
void __llvm_profile_instrument_memop(__u64 target_value, void *data,
					 __u32 counter_index);
void __llvm_profile_instrument_memop(__u64 target_value, void *data,
					 __u32 counter_index)
{
	__u64 rep_value;

	/* Map the target value to the representative value of its range. */
	rep_value = inst_prof_get_range_rep_value(target_value);
	__llvm_profile_instrument_target(rep_value, data, counter_index);
}
