/* drivers/misc/lowmemorykiller.c
 *
 * The lowmemorykiller driver lets user-space specify a set of memory thresholds
 * where processes with a range of oom_score_adj values will get killed. Specify
 * the minimum oom_score_adj values in
 * /sys/module/lowmemorykiller/parameters/adj and the number of free pages in
 * /sys/module/lowmemorykiller/parameters/minfree. Both files take a comma
 * separated list of numbers in ascending order.
 *
 * For example, write "0,8" to /sys/module/lowmemorykiller/parameters/adj and
 * "1024,4096" to /sys/module/lowmemorykiller/parameters/minfree to kill
 * processes with a oom_score_adj value of 8 or higher when the free memory
 * drops below 4096 pages and kill processes with a oom_score_adj value of 0 or
 * higher when the free memory drops below 1024 pages.
 *
 * The driver considers memory used for caches to be free, but if a large
 * percentage of the cached memory is locked this can be very inaccurate
 * and processes may not get killed until the normal oom killer is triggered.
 *
 * Copyright (C) 2007-2008 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/oom.h>
#include <linux/sched.h>
#include <linux/swap.h>
#include <linux/rcupdate.h>
#include <linux/profile.h>
#include <linux/notifier.h>

#define CREATE_TRACE_POINTS
#include "trace/lowmemorykiller.h"

static u32 lowmem_debug_level = 1;
static short lowmem_adj[6] = {
	0,
	1,
	6,
	12,
};

static int lowmem_adj_size = 4;
#ifdef CONFIG_MP_CMA_PATCH_CMA_DYNAMIC_STRATEGY
int lowmem_minfree[6] = {
#else
int lowmem_minfree[6] = {
#endif
	3 * 512,	/* 6MB */
	2 * 1024,	/* 8MB */
	4 * 1024,	/* 16MB */
	16 * 1024,	/* 64MB */
};

#ifdef CONFIG_MP_CMA_PATCH_DELAY_FREE
int lowmem_minfree_size = 4;
#else
static int lowmem_minfree_size = 4;
#endif

static unsigned long lowmem_deathpending_timeout;

#define lowmem_print(level, x...)			\
	do {						\
		if (lowmem_debug_level >= (level))	\
			pr_info(x);			\
	} while (0)

static unsigned long lowmem_count(struct shrinker *s,
				  struct shrink_control *sc)
{
	return global_node_page_state(NR_ACTIVE_ANON) +
		global_node_page_state(NR_ACTIVE_FILE) +
		global_node_page_state(NR_INACTIVE_ANON) +
		global_node_page_state(NR_INACTIVE_FILE);
}

#ifdef CONFIG_MP_DEBUG_TOOL_MEMORY_USAGE_MONITOR
extern db_time_table time_cnt_table[DB_MAX_CNT];
#endif

#ifdef CONFIG_MP_CMA_PATCH_DELAY_FREE
extern void set_delay_free_min_mem(int min_mem);
#endif

#ifdef CONFIG_MP_MMA_ENABLE
extern int total_ion_system_pages(void);
#endif

static unsigned long lowmem_scan(struct shrinker *s, struct shrink_control *sc)
{
#ifdef CONFIG_MP_DEBUG_TOOL_MEMORY_USAGE_MONITOR
	unsigned long time_start = jiffies;
#endif

	struct task_struct *tsk;
	struct task_struct *selected = NULL;
	unsigned long rem = 0;
	int tasksize;
	int i;
	short min_score_adj = OOM_SCORE_ADJ_MAX + 1;
	int minfree = 0;
	int selected_tasksize = 0;
	short selected_oom_score_adj;
	int array_size = ARRAY_SIZE(lowmem_adj);
	int other_free = global_page_state(NR_FREE_PAGES) - totalreserve_pages;
	int other_file = global_node_page_state(NR_FILE_PAGES) -
				global_node_page_state(NR_SHMEM) -
				global_node_page_state(NR_UNEVICTABLE) -
				total_swapcache_pages();

#ifdef CONFIG_MP_Android_MSTAR_ADJUST_LOW_MEM_KILLER_POLICY
	int active_file = global_node_page_state(NR_ACTIVE_FILE);
	int inactive_file = global_node_page_state(NR_INACTIVE_FILE);
#endif

	int free_cma = 0;
#ifdef CONFIG_MP_Android_MSTAR_ADJUST_LOW_MEM_KILLER_POLICY
	int total_free = 0;
#endif

#ifdef CONFIG_CMA
	if (gfpflags_to_migratetype(sc->gfp_mask) != MIGRATE_MOVABLE)
		free_cma = global_page_state(NR_FREE_CMA_PAGES);

#ifdef CONFIG_MP_CMA_PATCH_AGRESSIVE_KILL_PROCESS_TO_FREE_CMA_PAGE
	if(lowmem_adj[4])
		set_early_kill_oom_adj_threshold(lowmem_adj[4]);
#endif

#ifdef CONFIG_MP_CMA_PATCH_DELAY_FREE
	if(lowmem_minfree[5])
		set_delay_free_min_mem(lowmem_minfree[5]);
#endif

#endif

#ifdef CONFIG_MP_Android_MSTAR_ADJUST_LOW_MEM_KILLER_POLICY
	// the file cache can be writted as "other_file - mmaped"(unmapped) or "other_file - active"(inactive)
	total_free = other_free - free_cma + (other_file - active_file);
#ifdef CONFIG_MP_MMA_ENABLE
	total_free += total_ion_system_pages();
#endif
	//total_free = other_free - free_cma + other_file - global_node_page_state(NR_FILE_MAPPED);
#endif

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;
	if (lowmem_minfree_size < array_size)
		array_size = lowmem_minfree_size;
	for (i = 0; i < array_size; i++) {
		minfree = lowmem_minfree[i];
#ifdef CONFIG_MP_Android_MSTAR_ADJUST_LOW_MEM_KILLER_POLICY
		if(lowmem_adj[i] == 0)
			total_free += active_file;
		if (total_free < minfree) {
#else
		if ((other_free - free_cma) < minfree && other_file < minfree) {
#endif
			min_score_adj = lowmem_adj[i];
			break;
		}
	}

	lowmem_print(3, "lowmem_scan %lu, %x, ofree %d %d, ma %hd\n",
		     sc->nr_to_scan, sc->gfp_mask, other_free,
		     other_file, min_score_adj);

	if (min_score_adj == OOM_SCORE_ADJ_MAX + 1) {
		lowmem_print(5, "lowmem_scan %lu, %x, return 0\n",
			     sc->nr_to_scan, sc->gfp_mask);
#ifdef CONFIG_MP_Android_MSTAR_ADJUST_LOW_MEM_KILLER_POLICY
		lowmem_print(5, "total_free is %d, other_file is %d, free_cma is %d\n", total_free, other_file, free_cma);
		lowmem_print(5, "active_file is %d, inactive_file is %d\n", active_file, inactive_file);
#if 0
		int print_index = 0;
		for(print_index = 0; print_index < array_size; print_index++)
		{
			printk("\033[35mlowmem_adj: %d, lowmem_minfree: %d\033[m\n", lowmem_adj[print_index], lowmem_minfree[print_index]); // page_count
		}
#endif
#endif
#ifdef CONFIG_MP_DEBUG_TOOL_MEMORY_USAGE_MONITOR
		atomic_add((jiffies-time_start), &time_cnt_table[lowmem_scan_count].lone_time);
		atomic_inc(&time_cnt_table[lowmem_scan_count].do_cnt);
#endif
		return 0;
	}

	selected_oom_score_adj = min_score_adj;

	rcu_read_lock();
	for_each_process(tsk) {
		struct task_struct *p;
		short oom_score_adj;

		if (tsk->flags & PF_KTHREAD)
			continue;

		p = find_lock_task_mm(tsk);
		if (!p)
			continue;

		if (task_lmk_waiting(p) &&
		    time_before_eq(jiffies, lowmem_deathpending_timeout)) {
			task_unlock(p);
			rcu_read_unlock();
#ifdef CONFIG_MP_DEBUG_TOOL_MEMORY_USAGE_MONITOR
			atomic_add((jiffies-time_start), &time_cnt_table[lowmem_scan_count].lone_time);
			atomic_inc(&time_cnt_table[lowmem_scan_count].do_cnt);
#endif
			return 0;
		}
		oom_score_adj = p->signal->oom_score_adj;
		if (oom_score_adj < min_score_adj) {
			task_unlock(p);
			continue;
		}
		tasksize = get_mm_rss(p->mm);
		task_unlock(p);
		if (tasksize <= 0)
			continue;
		if (selected) {
			if (oom_score_adj < selected_oom_score_adj)
				continue;
			if (oom_score_adj == selected_oom_score_adj &&
			    tasksize <= selected_tasksize)
				continue;
		}
		selected = p;
		selected_tasksize = tasksize;
		selected_oom_score_adj = oom_score_adj;
		lowmem_print(2, "select '%s' (%d), adj %hd, size %d, to kill\n",
			     p->comm, p->pid, oom_score_adj, tasksize);
	}
	if (selected) {
		long cache_size = other_file * (long)(PAGE_SIZE / 1024);
		long cache_limit = minfree * (long)(PAGE_SIZE / 1024);
		long free = other_free * (long)(PAGE_SIZE / 1024);

		task_lock(selected);
		send_sig(SIGKILL, selected, 0);
		if (selected->mm)
			task_set_lmk_waiting(selected);
		task_unlock(selected);
		trace_lowmemory_kill(selected, cache_size, cache_limit, free);
		lowmem_print(1, "Killing '%s' (%d) (tgid %d), adj %hd,\n"
				 "   to free %ldkB on behalf of '%s' (%d) because\n"
				 "   cache %ldkB is below limit %ldkB for oom_score_adj %hd\n"
				 "   Free memory is %ldkB above reserved\n",
			     selected->comm, selected->pid, selected->tgid,
			     selected_oom_score_adj,
			     selected_tasksize * (long)(PAGE_SIZE / 1024),
			     current->comm, current->pid,
			     cache_size, cache_limit,
			     min_score_adj,
			     free);

#ifdef CONFIG_MP_Android_MSTAR_ADJUST_LOW_MEM_KILLER_POLICY
		printk("   Total_free = %ldkB, free_cma=%ldkB, Totalreserve_pages = %ldkB, MAPPED = %ldkB\n",
			total_free * (long)(PAGE_SIZE / 1024),
			free_cma * (long)(PAGE_SIZE / 1024),
			totalreserve_pages * (long)(PAGE_SIZE / 1024),
			global_node_page_state(NR_FILE_MAPPED) * (long)(PAGE_SIZE / 1024));
		lowmem_print(1, "total_free is %d, other_free is %d, free_cma is %d\n", total_free, other_free, free_cma);
		lowmem_print(1, "other_file is %d, active_file is %d, inactive_file is %d\n\n\n", other_file, active_file, inactive_file);
#endif
		lowmem_deathpending_timeout = jiffies + HZ;
		rem += selected_tasksize;
	}

	lowmem_print(4, "lowmem_scan %lu, %x, return %lu\n",
		     sc->nr_to_scan, sc->gfp_mask, rem);
	rcu_read_unlock();
#ifdef CONFIG_MP_DEBUG_TOOL_MEMORY_USAGE_MONITOR
	atomic_add((jiffies-time_start), &time_cnt_table[lowmem_scan_count].lone_time);
	atomic_inc(&time_cnt_table[lowmem_scan_count].do_cnt);
#endif
	return rem;
}

static struct shrinker lowmem_shrinker = {
	.scan_objects = lowmem_scan,
	.count_objects = lowmem_count,
	.seeks = DEFAULT_SEEKS * 16
};

static int __init lowmem_init(void)
{
	register_shrinker(&lowmem_shrinker);
	return 0;
}
device_initcall(lowmem_init);

#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
static short lowmem_oom_adj_to_oom_score_adj(short oom_adj)
{
	if (oom_adj == OOM_ADJUST_MAX)
		return OOM_SCORE_ADJ_MAX;
	else
		return (oom_adj * OOM_SCORE_ADJ_MAX) / -OOM_DISABLE;
}

static void lowmem_autodetect_oom_adj_values(void)
{
	int i;
	short oom_adj;
	short oom_score_adj;
	int array_size = ARRAY_SIZE(lowmem_adj);

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;

	if (array_size <= 0)
		return;

	oom_adj = lowmem_adj[array_size - 1];
	if (oom_adj > OOM_ADJUST_MAX)
		return;

	oom_score_adj = lowmem_oom_adj_to_oom_score_adj(oom_adj);
	if (oom_score_adj <= OOM_ADJUST_MAX)
		return;

	lowmem_print(1, "lowmem_shrink: convert oom_adj to oom_score_adj:\n");
	for (i = 0; i < array_size; i++) {
		oom_adj = lowmem_adj[i];
		oom_score_adj = lowmem_oom_adj_to_oom_score_adj(oom_adj);
		lowmem_adj[i] = oom_score_adj;
		lowmem_print(1, "oom_adj %d => oom_score_adj %d, with lowmem_minfree is %dkB\n",
			     oom_adj, oom_score_adj, lowmem_minfree[i]);
	}
}

static int lowmem_adj_array_set(const char *val, const struct kernel_param *kp)
{
	int ret;

	ret = param_array_ops.set(val, kp);

	/* HACK: Autodetect oom_adj values in lowmem_adj array */
	lowmem_autodetect_oom_adj_values();

	return ret;
}

static int lowmem_adj_array_get(char *buffer, const struct kernel_param *kp)
{
	return param_array_ops.get(buffer, kp);
}

static void lowmem_adj_array_free(void *arg)
{
	param_array_ops.free(arg);
}

static struct kernel_param_ops lowmem_adj_array_ops = {
	.set = lowmem_adj_array_set,
	.get = lowmem_adj_array_get,
	.free = lowmem_adj_array_free,
};

static const struct kparam_array __param_arr_adj = {
	.max = ARRAY_SIZE(lowmem_adj),
	.num = &lowmem_adj_size,
	.ops = &param_ops_short,
	.elemsize = sizeof(lowmem_adj[0]),
	.elem = lowmem_adj,
};
#endif

/*
 * not really modular, but the easiest way to keep compat with existing
 * bootargs behaviour is to continue using module_param here.
 */
module_param_named(cost, lowmem_shrinker.seeks, int, 0644);
#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
module_param_cb(adj, &lowmem_adj_array_ops,
		.arr = &__param_arr_adj,
		0644);
__MODULE_PARM_TYPE(adj, "array of short");
#else
module_param_array_named(adj, lowmem_adj, short, &lowmem_adj_size, 0644);
#endif
module_param_array_named(minfree, lowmem_minfree, uint, &lowmem_minfree_size,
			 0644);
module_param_named(debug_level, lowmem_debug_level, uint, 0644);
