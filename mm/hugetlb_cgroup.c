/*
 *
 * Copyright IBM Corporation, 2012
 * Author Aneesh Kumar K.V <aneesh.kumar@linux.vnet.ibm.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2.1 of the GNU Lesser General Public License
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 */

#include <linux/cgroup.h>
#include <linux/slab.h>
#include <linux/hugetlb.h>
#include <linux/hugetlb_cgroup.h>

struct hugetlb_cgroup {
	struct cgroup_subsys_state css;
	/*
	 * the counter to account for hugepages from hugetlb.
	 */
	struct res_counter hugepage[HUGE_MAX_HSTATE];
};

#define MEMFILE_PRIVATE(x, val)	(((x) << 16) | (val))
#define MEMFILE_IDX(val)	(((val) >> 16) & 0xffff)
#define MEMFILE_ATTR(val)	((val) & 0xffff)

struct cgroup_subsys hugetlb_subsys __read_mostly;
static struct hugetlb_cgroup *root_h_cgroup __read_mostly;

static inline
struct hugetlb_cgroup *hugetlb_cgroup_from_css(struct cgroup_subsys_state *s)
{
	return container_of(s, struct hugetlb_cgroup, css);
}

static inline
struct hugetlb_cgroup *hugetlb_cgroup_from_cgroup(struct cgroup *cgroup)
{
	return hugetlb_cgroup_from_css(cgroup_subsys_state(cgroup,
							   hugetlb_subsys_id));
}

static inline
struct hugetlb_cgroup *hugetlb_cgroup_from_task(struct task_struct *task)
{
	return hugetlb_cgroup_from_css(task_subsys_state(task,
							 hugetlb_subsys_id));
}

static inline bool hugetlb_cgroup_is_root(struct hugetlb_cgroup *h_cg)
{
	return (h_cg == root_h_cgroup);
}

static inline struct hugetlb_cgroup *parent_hugetlb_cgroup(struct cgroup *cg)
{
	if (!cg->parent)
		return NULL;
	return hugetlb_cgroup_from_cgroup(cg->parent);
}

static inline bool hugetlb_cgroup_have_usage(struct cgroup *cg)
{
	int idx;
	struct hugetlb_cgroup *h_cg = hugetlb_cgroup_from_cgroup(cg);

	for (idx = 0; idx < hugetlb_max_hstate; idx++) {
		if ((res_counter_read_u64(&h_cg->hugepage[idx], RES_USAGE)) > 0)
			return true;
	}
	return false;
}

static struct cgroup_subsys_state *hugetlb_cgroup_create(struct cgroup *cgroup)
{
	int idx;
	struct cgroup *parent_cgroup;
	struct hugetlb_cgroup *h_cgroup, *parent_h_cgroup;

	h_cgroup = kzalloc(sizeof(*h_cgroup), GFP_KERNEL);
	if (!h_cgroup)
		return ERR_PTR(-ENOMEM);

	parent_cgroup = cgroup->parent;
	if (parent_cgroup) {
		parent_h_cgroup = hugetlb_cgroup_from_cgroup(parent_cgroup);
		for (idx = 0; idx < HUGE_MAX_HSTATE; idx++)
			res_counter_init(&h_cgroup->hugepage[idx],
					 &parent_h_cgroup->hugepage[idx]);
	} else {
		root_h_cgroup = h_cgroup;
		for (idx = 0; idx < HUGE_MAX_HSTATE; idx++)
			res_counter_init(&h_cgroup->hugepage[idx], NULL);
	}
	return &h_cgroup->css;
}

static void hugetlb_cgroup_destroy(struct cgroup *cgroup)
{
	struct hugetlb_cgroup *h_cgroup;

	h_cgroup = hugetlb_cgroup_from_cgroup(cgroup);
	kfree(h_cgroup);
}

static int hugetlb_cgroup_pre_destroy(struct cgroup *cgroup)
{
	/* We will add the cgroup removal support in later patches */
	   return -EBUSY;
}

int hugetlb_cgroup_charge_cgroup(int idx, unsigned long nr_pages,
				 struct hugetlb_cgroup **ptr)
{
	int ret = 0;
	struct res_counter *fail_res;
	struct hugetlb_cgroup *h_cg = NULL;
	unsigned long csize = nr_pages * PAGE_SIZE;

	if (hugetlb_cgroup_disabled())
		goto done;
	/*
	 * We don't charge any cgroup if the compound page have less
	 * than 3 pages.
	 */
	if (huge_page_order(&hstates[idx]) < HUGETLB_CGROUP_MIN_ORDER)
		goto done;
again:
	rcu_read_lock();
	h_cg = hugetlb_cgroup_from_task(current);
	if (!css_tryget(&h_cg->css)) {
		rcu_read_unlock();
		goto again;
	}
	rcu_read_unlock();

	ret = res_counter_charge(&h_cg->hugepage[idx], csize, &fail_res);
	css_put(&h_cg->css);
done:
	*ptr = h_cg;
	return ret;
}

void hugetlb_cgroup_commit_charge(int idx, unsigned long nr_pages,
				  struct hugetlb_cgroup *h_cg,
				  struct page *page)
{
	if (hugetlb_cgroup_disabled() || !h_cg)
		return;

	spin_lock(&hugetlb_lock);
	set_hugetlb_cgroup(page, h_cg);
	spin_unlock(&hugetlb_lock);
	return;
}

/*
 * Should be called with hugetlb_lock held
 */
void hugetlb_cgroup_uncharge_page(int idx, unsigned long nr_pages,
				  struct page *page)
{
	struct hugetlb_cgroup *h_cg;
	unsigned long csize = nr_pages * PAGE_SIZE;

	if (hugetlb_cgroup_disabled())
		return;
	VM_BUG_ON(!spin_is_locked(&hugetlb_lock));
	h_cg = hugetlb_cgroup_from_page(page);
	if (unlikely(!h_cg))
		return;
	set_hugetlb_cgroup(page, NULL);
	res_counter_uncharge(&h_cg->hugepage[idx], csize);
	return;
}

void hugetlb_cgroup_uncharge_cgroup(int idx, unsigned long nr_pages,
				    struct hugetlb_cgroup *h_cg)
{
	unsigned long csize = nr_pages * PAGE_SIZE;

	if (hugetlb_cgroup_disabled() || !h_cg)
		return;

	if (huge_page_order(&hstates[idx]) < HUGETLB_CGROUP_MIN_ORDER)
		return;

	res_counter_uncharge(&h_cg->hugepage[idx], csize);
	return;
}

static ssize_t hugetlb_cgroup_read(struct cgroup *cgroup, struct cftype *cft,
				   struct file *file, char __user *buf,
				   size_t nbytes, loff_t *ppos)
{
	u64 val;
	char str[64];
	int idx, name, len;
	struct hugetlb_cgroup *h_cg = hugetlb_cgroup_from_cgroup(cgroup);

	idx = MEMFILE_IDX(cft->private);
	name = MEMFILE_ATTR(cft->private);

	val = res_counter_read_u64(&h_cg->hugepage[idx], name);
	len = scnprintf(str, sizeof(str), "%llu\n", (unsigned long long)val);
	return simple_read_from_buffer(buf, nbytes, ppos, str, len);
}

static int hugetlb_cgroup_write(struct cgroup *cgroup, struct cftype *cft,
				const char *buffer)
{
	int idx, name, ret;
	unsigned long long val;
	struct hugetlb_cgroup *h_cg = hugetlb_cgroup_from_cgroup(cgroup);

	idx = MEMFILE_IDX(cft->private);
	name = MEMFILE_ATTR(cft->private);

	switch (name) {
	case RES_LIMIT:
		if (hugetlb_cgroup_is_root(h_cg)) {
			/* Can't set limit on root */
			ret = -EINVAL;
			break;
		}
		/* This function does all necessary parse...reuse it */
		ret = res_counter_memparse_write_strategy(buffer, &val);
		if (ret)
			break;
		ret = res_counter_set_limit(&h_cg->hugepage[idx], val);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int hugetlb_cgroup_reset(struct cgroup *cgroup, unsigned int event)
{
	int idx, name, ret = 0;
	struct hugetlb_cgroup *h_cg = hugetlb_cgroup_from_cgroup(cgroup);

	idx = MEMFILE_IDX(event);
	name = MEMFILE_ATTR(event);

	switch (name) {
	case RES_MAX_USAGE:
		res_counter_reset_max(&h_cg->hugepage[idx]);
		break;
	case RES_FAILCNT:
		res_counter_reset_failcnt(&h_cg->hugepage[idx]);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static char *mem_fmt(char *buf, int size, unsigned long hsize)
{
	if (hsize >= (1UL << 30))
		snprintf(buf, size, "%luGB", hsize >> 30);
	else if (hsize >= (1UL << 20))
		snprintf(buf, size, "%luMB", hsize >> 20);
	else
		snprintf(buf, size, "%luKB", hsize >> 10);
	return buf;
}

int __init hugetlb_cgroup_file_init(int idx)
{
	char buf[32];
	struct cftype *cft;
	struct hstate *h = &hstates[idx];

	/* format the size */
	mem_fmt(buf, 32, huge_page_size(h));

	/* Add the limit file */
	cft = &h->cgroup_files[0];
	snprintf(cft->name, MAX_CFTYPE_NAME, "%s.limit_in_bytes", buf);
	cft->private = MEMFILE_PRIVATE(idx, RES_LIMIT);
	cft->read = hugetlb_cgroup_read;
	cft->write_string = hugetlb_cgroup_write;

	/* Add the usage file */
	cft = &h->cgroup_files[1];
	snprintf(cft->name, MAX_CFTYPE_NAME, "%s.usage_in_bytes", buf);
	cft->private = MEMFILE_PRIVATE(idx, RES_USAGE);
	cft->read = hugetlb_cgroup_read;

	/* Add the MAX usage file */
	cft = &h->cgroup_files[2];
	snprintf(cft->name, MAX_CFTYPE_NAME, "%s.max_usage_in_bytes", buf);
	cft->private = MEMFILE_PRIVATE(idx, RES_MAX_USAGE);
	cft->trigger = hugetlb_cgroup_reset;
	cft->read = hugetlb_cgroup_read;

	/* Add the failcntfile */
	cft = &h->cgroup_files[3];
	snprintf(cft->name, MAX_CFTYPE_NAME, "%s.failcnt", buf);
	cft->private  = MEMFILE_PRIVATE(idx, RES_FAILCNT);
	cft->trigger  = hugetlb_cgroup_reset;
	cft->read = hugetlb_cgroup_read;

	/* NULL terminate the last cft */
	cft = &h->cgroup_files[4];
	memset(cft, 0, sizeof(*cft));

	WARN_ON(cgroup_add_cftypes(&hugetlb_subsys, h->cgroup_files));

	return 0;
}

void hugetlb_cgroup_migrate(struct page *oldhpage, struct page *newhpage)
{
	struct hugetlb_cgroup *h_cg;

	if (hugetlb_cgroup_disabled())
		return;

	VM_BUG_ON(!PageHuge(oldhpage));
	spin_lock(&hugetlb_lock);
	h_cg = hugetlb_cgroup_from_page(oldhpage);
	set_hugetlb_cgroup(oldhpage, NULL);
	cgroup_exclude_rmdir(&h_cg->css);

	/* move the h_cg details to new cgroup */
	set_hugetlb_cgroup(newhpage, h_cg);
	spin_unlock(&hugetlb_lock);
	cgroup_release_and_wakeup_rmdir(&h_cg->css);
	return;
}

struct cgroup_subsys hugetlb_subsys = {
	.name = "hugetlb",
	.create     = hugetlb_cgroup_create,
	.pre_destroy = hugetlb_cgroup_pre_destroy,
	.destroy    = hugetlb_cgroup_destroy,
	.subsys_id  = hugetlb_subsys_id,
};
