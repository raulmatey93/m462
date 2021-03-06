
/*
 *
 * (C) COPYRIGHT ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU licence.
 *
 * A copy of the licence is included with the program, and can also be obtained
 * from Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 */





/**
 * @file mali_kbase_core_linux.c
 * Base kernel driver init.
 */

#include <mali_kbase.h>
#include <mali_kbase_uku.h>
#include <mali_midg_regmap.h>
#include <mali_kbase_gator.h>
#include <mali_kbase_mem_linux.h>
#ifdef CONFIG_MALI_NO_MALI
#include "mali_kbase_model_linux.h"
#endif /* CONFIG_MALI_NO_MALI */

#ifdef CONFIG_KDS
#include <linux/kds.h>
#include <linux/anon_inodes.h>
#include <linux/syscalls.h>
#endif /* CONFIG_KDS */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/poll.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/list.h>
#include <linux/semaphore.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/compat.h>	/* is_compat_task */
#include <mali_kbase_hw.h>
#include <platform/mali_kbase_platform.h>
#ifdef CONFIG_SYNC
#include <mali_kbase_sync.h>
#endif /* CONFIG_SYNC */
#include <linux/pm_qos.h>
#include <linux/sched.h>
#include <mach/cpufreq.h>

#if defined(SET_MINLOCK)
#include "platform/mali_kbase_platform.h"
#include "platform/gpu_dvfs_handler.h"
extern int set_hmp_boost(int enable);
#endif

/*
 * This file is included since when we support device tree we don't
 * use the platform fake code for registering the kbase config attributes.
 */
#ifdef CONFIG_OF
#include <mali_kbase_config.h>
#endif

#ifdef CONFIG_MACH_MANTA
#include <plat/devs.h>
#endif

#define	JOB_IRQ_TAG	0
#define MMU_IRQ_TAG	1
#define GPU_IRQ_TAG	2

struct kbase_irq_table {
	u32 tag;
	irq_handler_t handler;
};
#if MALI_UNIT_TEST
kbase_exported_test_data shared_kernel_test_data;
EXPORT_SYMBOL(shared_kernel_test_data);
#endif /* MALI_UNIT_TEST */

#define KBASE_DRV_NAME "mali"

static const char kbase_drv_name[] = KBASE_DRV_NAME;

static int kbase_dev_nr;

static DEFINE_SEMAPHORE(kbase_dev_list_lock);
static LIST_HEAD(kbase_dev_list);

KBASE_EXPORT_TEST_API(kbase_dev_list_lock)
KBASE_EXPORT_TEST_API(kbase_dev_list)
#define KERNEL_SIDE_DDK_VERSION_STRING "K:" MALI_RELEASE_NAME "(GPL)"
static INLINE void __compile_time_asserts(void)
{
	CSTD_COMPILE_TIME_ASSERT(sizeof(KERNEL_SIDE_DDK_VERSION_STRING) <= KBASE_GET_VERSION_BUFFER_SIZE);
}

#ifdef CONFIG_KDS

typedef struct kbasep_kds_resource_set_file_data {
	struct kds_resource_set *lock;
} kbasep_kds_resource_set_file_data;

static int kds_resource_release(struct inode *inode, struct file *file);

static const struct file_operations kds_resource_fops = {
	.release = kds_resource_release
};

typedef struct kbase_kds_resource_list_data {
	struct kds_resource **kds_resources;
	unsigned long *kds_access_bitmap;
	int num_elems;
} kbase_kds_resource_list_data;

static int kds_resource_release(struct inode *inode, struct file *file)
{
	struct kbasep_kds_resource_set_file_data *data;

	data = (struct kbasep_kds_resource_set_file_data *)file->private_data;
	if (NULL != data) {
		if (NULL != data->lock)
			kds_resource_set_release(&data->lock);

		kfree(data);
	}
	return 0;
}

mali_error kbasep_kds_allocate_resource_list_data(kbase_context *kctx, base_external_resource *ext_res, int num_elems, kbase_kds_resource_list_data *resources_list)
{
	base_external_resource *res = ext_res;
	int res_id;

	/* assume we have to wait for all */

	KBASE_DEBUG_ASSERT(0 != num_elems);
	resources_list->kds_resources = kmalloc(sizeof(struct kds_resource *) * num_elems, GFP_KERNEL);

	if (NULL == resources_list->kds_resources)
		return MALI_ERROR_OUT_OF_MEMORY;

	KBASE_DEBUG_ASSERT(0 != num_elems);
	resources_list->kds_access_bitmap = kzalloc(sizeof(unsigned long) * ((num_elems + BITS_PER_LONG - 1) / BITS_PER_LONG), GFP_KERNEL);

	if (NULL == resources_list->kds_access_bitmap) {
		kfree(resources_list->kds_access_bitmap);
		return MALI_ERROR_OUT_OF_MEMORY;
	}

	kbase_gpu_vm_lock(kctx);
	for (res_id = 0; res_id < num_elems; res_id++, res++) {
		int exclusive;
		kbase_va_region *reg;
		struct kds_resource *kds_res = NULL;

		exclusive = res->ext_resource & BASE_EXT_RES_ACCESS_EXCLUSIVE;
		reg = kbase_region_tracker_find_region_enclosing_address(kctx, res->ext_resource & ~BASE_EXT_RES_ACCESS_EXCLUSIVE);

		/* did we find a matching region object? */
		if (NULL == reg)
			break;

		/* no need to check reg->alloc as only regions with an alloc has
		 * a size, and kbase_region_tracker_find_region_enclosing_address
		 * only returns regions with size > 0 */
		switch (reg->alloc->type) {
#if defined(CONFIG_UMP) && defined(CONFIG_KDS)
		case KBASE_MEM_TYPE_IMPORTED_UMP:
			kds_res = ump_dd_kds_resource_get(reg->alloc->imported.ump_handle);
			break;
#endif /* defined(CONFIG_UMP) && defined(CONFIG_KDS) */
		default:
			break;
		}

		/* no kds resource for the region ? */
		if (!kds_res)
			break;

		resources_list->kds_resources[res_id] = kds_res;

		if (exclusive)
			set_bit(res_id, resources_list->kds_access_bitmap);
	}
	kbase_gpu_vm_unlock(kctx);

	/* did the loop run to completion? */
	if (res_id == num_elems)
		return MALI_ERROR_NONE;

	/* Clean up as the resource list is not valid. */
	kfree(resources_list->kds_resources);
	kfree(resources_list->kds_access_bitmap);

	return MALI_ERROR_FUNCTION_FAILED;
}

mali_bool kbasep_validate_kbase_pointer(kbase_pointer *p)
{
#ifdef CONFIG_COMPAT
	if (is_compat_task()) {
		if (p->compat_value == 0)
			return MALI_FALSE;
	} else {
#endif /* CONFIG_COMPAT */
		if (NULL == p->value)
			return MALI_FALSE;
#ifdef CONFIG_COMPAT
	}
#endif /* CONFIG_COMPAT */
	return MALI_TRUE;
}

mali_error kbase_external_buffer_lock(kbase_context *kctx, kbase_uk_ext_buff_kds_data *args, u32 args_size)
{
	base_external_resource *ext_res_copy;
	size_t ext_resource_size;
	mali_error return_error = MALI_ERROR_FUNCTION_FAILED;
	int fd;

	if (args_size != sizeof(kbase_uk_ext_buff_kds_data))
		return MALI_ERROR_FUNCTION_FAILED;

	/* Check user space has provided valid data */
	if (!kbasep_validate_kbase_pointer(&args->external_resource) || !kbasep_validate_kbase_pointer(&args->file_descriptor) || (0 == args->num_res) || (args->num_res > KBASE_MAXIMUM_EXT_RESOURCES))
		return MALI_ERROR_FUNCTION_FAILED;

	ext_resource_size = sizeof(base_external_resource) * args->num_res;

	KBASE_DEBUG_ASSERT(0 != ext_resource_size);
	ext_res_copy = kmalloc(ext_resource_size, GFP_KERNEL);

	if (NULL != ext_res_copy) {
		base_external_resource *__user ext_res_user;
		int *__user file_descriptor_user;
#ifdef CONFIG_COMPAT
		if (is_compat_task()) {
			ext_res_user = compat_ptr(args->external_resource.compat_value);
			file_descriptor_user = compat_ptr(args->file_descriptor.compat_value);
		} else {
#endif /* CONFIG_COMPAT */
			ext_res_user = args->external_resource.value;
			file_descriptor_user = args->file_descriptor.value;
#ifdef CONFIG_COMPAT
		}
#endif /* CONFIG_COMPAT */

		/* Copy the external resources to lock from user space */
		if (0 == copy_from_user(ext_res_copy, ext_res_user, ext_resource_size)) {
			kbasep_kds_resource_set_file_data *fdata;

			/* Allocate data to be stored in the file */
			fdata = kmalloc(sizeof(kbasep_kds_resource_set_file_data), GFP_KERNEL);

			if (NULL != fdata) {
				kbase_kds_resource_list_data resource_list_data;
				/* Parse given elements and create resource and access lists */
				return_error = kbasep_kds_allocate_resource_list_data(kctx, ext_res_copy, args->num_res, &resource_list_data);
				if (MALI_ERROR_NONE == return_error) {
					long err;

					fdata->lock = NULL;

					fd = anon_inode_getfd("kds_ext", &kds_resource_fops, fdata, 0);

					err = copy_to_user(file_descriptor_user, &fd, sizeof(fd));

					/* If the file descriptor was valid and we successfully copied it to user space, then we
					 * can try and lock the requested kds resources.
					 */
					if ((fd >= 0) && (0 == err)) {
						struct kds_resource_set *lock;

						lock = kds_waitall(args->num_res, resource_list_data.kds_access_bitmap, resource_list_data.kds_resources, KDS_WAIT_BLOCKING);

						if (IS_ERR_OR_NULL(lock)) {
							return_error = MALI_ERROR_FUNCTION_FAILED;
						} else {
							return_error = MALI_ERROR_NONE;
							fdata->lock = lock;
						}
					} else {
						return_error = MALI_ERROR_FUNCTION_FAILED;
					}

					kfree(resource_list_data.kds_resources);
					kfree(resource_list_data.kds_access_bitmap);
				}

				if (MALI_ERROR_NONE != return_error) {
					/* If the file was opened successfully then close it which will clean up
					 * the file data, otherwise we clean up the file data ourself. */
					if (fd >= 0)
						sys_close(fd);
					else
						kfree(fdata);
				}
			} else {
				return_error = MALI_ERROR_OUT_OF_MEMORY;
			}
		}
		kfree(ext_res_copy);
	}
	return return_error;
}
#endif /* CONFIG_KDS */

#ifdef CONFIG_USE_VSYNC_SKIP
void s3c_fb_extra_vsync_wait_set(int);
void s3c_fb_extra_vsync_wait_add(int);
#endif

#ifdef SLSI_INTEGRATION
#if defined(SET_MINLOCK)
extern struct pm_qos_request exynos5_g3d_cpu_egl_min_qos;
#endif /* SE_MINLOCK */
#endif /* SLSI_INTEGRATION */

static mali_error kbase_dispatch(kbase_context *kctx, void * const args, u32 args_size)
{
	struct kbase_device *kbdev;
	uk_header *ukh = args;
	u32 id;

	KBASE_DEBUG_ASSERT(ukh != NULL);

	kbdev = kctx->kbdev;
	id = ukh->id;
	ukh->ret = MALI_ERROR_NONE;	/* Be optimistic */

	if (UKP_FUNC_ID_CHECK_VERSION == id) {
		if (args_size == sizeof(uku_version_check_args)) {
			uku_version_check_args *version_check = (uku_version_check_args *)args;

			if ( (version_check->major == BASE_UK_VERSION_MAJOR) &&
			     (version_check->minor < BASE_UK_VERSION_MINOR))
				kctx->legacy_app = 1;

			version_check->major = BASE_UK_VERSION_MAJOR;
			version_check->minor = BASE_UK_VERSION_MINOR;

			ukh->ret = MALI_ERROR_NONE;
		} else {
			ukh->ret = MALI_ERROR_FUNCTION_FAILED;
		}
		return MALI_ERROR_NONE;
	}


	if (!atomic_read(&kctx->setup_complete)) {
		/* setup pending, try to signal that we'll do the setup */
		if (atomic_cmpxchg(&kctx->setup_in_progress, 0, 1)) {
			/* setup was already in progress, err this call */
			return MALI_ERROR_FUNCTION_FAILED;
		}

		/* we're the one doing setup */

		/* is it the only call we accept? */
		if (id == KBASE_FUNC_SET_FLAGS) {
			kbase_uk_set_flags *kbase_set_flags = (kbase_uk_set_flags *) args;

			if (sizeof(*kbase_set_flags) != args_size) {
				/* not matching the expected call, stay stuck in setup mode */
				goto bad_size;
			}

			if (MALI_ERROR_NONE != kbase_context_set_create_flags(kctx, kbase_set_flags->create_flags)) {
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
				/* bad flags, will stay stuck in setup mode */
				return MALI_ERROR_NONE;
			} else {
				/* we've done the setup, all OK */
				atomic_set(&kctx->setup_complete, 1);
				return MALI_ERROR_NONE;
			}
		} else {
			/* unexpected call, will stay stuck in setup mode */
			return MALI_ERROR_FUNCTION_FAILED;
		}
	}

	/* setup complete, perform normal operation */
	switch (id) {
#if SLSI_INTEGRATION
	case KBASE_FUNC_CREATE_SURFACE:
		{
			kbase_mem_set_max_size(kctx);
			break;
		}
	case KBASE_FUNC_DESTROY_SURFACE:
		{
			kbase_mem_free_list_cleanup(kctx);
			break;
		}
#endif
	case KBASE_FUNC_MEM_ALLOC:
		{
			kbase_uk_mem_alloc *mem = args;
			struct kbase_va_region *reg;

			if (sizeof(*mem) != args_size)
				goto bad_size;

			reg = kbase_mem_alloc(kctx, mem->va_pages, mem->commit_pages, mem->extent, &mem->flags, &mem->gpu_va, &mem->va_alignment);
			if (!reg)
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			break;
		}
	case KBASE_FUNC_MEM_IMPORT:
		{
			kbase_uk_mem_import *mem_import = args;
			int *__user phandle;
			int handle;

			if (sizeof(*mem_import) != args_size)
				goto bad_size;
#ifdef CONFIG_64BIT
			if (is_compat_task())
				phandle = compat_ptr(mem_import->phandle.compat_value);
			else
#endif
				phandle = mem_import->phandle.value;

			switch (mem_import->type) {
			case BASE_MEM_IMPORT_TYPE_UMP:
				get_user(handle, phandle);
				break;
			case BASE_MEM_IMPORT_TYPE_UMM:
				get_user(handle, phandle);
				break;
			default:
				goto bad_type;
				break;
			}

			if (kbase_mem_import(kctx, mem_import->type, handle, &mem_import->gpu_va, &mem_import->va_pages, &mem_import->flags)) {
bad_type:
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			}
			break;
		}
	case KBASE_FUNC_MEM_ALIAS: {
			kbase_uk_mem_alias *alias = args;
			struct base_mem_aliasing_info *__user user_ai;
			struct base_mem_aliasing_info *ai;

			if (sizeof(*alias) != args_size)
				goto bad_size;

			if (alias->nents > 4) {
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
				break;
			}

#ifdef CONFIG_64BIT
			if (is_compat_task())
				user_ai = compat_ptr(alias->ai.compat_value);
			else
#endif
				user_ai = alias->ai.value;

			ai = kmalloc(GFP_KERNEL, sizeof(*ai) * alias->nents);
			if (!ai) {
				ukh->ret = MALI_ERROR_OUT_OF_MEMORY;
				break;
			}

			if (copy_from_user(ai, user_ai,
					   sizeof(*ai) * alias->nents)) {
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
				goto copy_failed;
			}

			alias->gpu_va = kbase_mem_alias(kctx, &alias->flags,
							alias->stride,
							alias->nents, ai,
							&alias->va_pages);
			if (!alias->gpu_va) {
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
				goto no_alias;
			}
no_alias:
copy_failed:
			kfree(ai);
			break;
		}
	case KBASE_FUNC_MEM_COMMIT:
		{
			kbase_uk_mem_commit *commit = args;

			if (sizeof(*commit) != args_size)
				goto bad_size;

			if (commit->gpu_addr & ~PAGE_MASK) {
				KBASE_DEBUG_PRINT_WARN(KBASE_MEM, "kbase_dispatch case KBASE_FUNC_MEM_COMMIT: commit->gpu_addr: passed parameter is invalid");
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
				break;
			}

			if (kbase_mem_commit(kctx, commit->gpu_addr, commit->pages, (base_backing_threshold_status*)&commit->result_subcode))
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			break;
		}

	case KBASE_FUNC_MEM_QUERY:
		{
			kbase_uk_mem_query *query = args;
			if (sizeof(*query) != args_size)
				goto bad_size;

			if (query->gpu_addr & ~PAGE_MASK) {
				KBASE_DEBUG_PRINT_WARN(KBASE_MEM, "kbase_dispatch case KBASE_FUNC_MEM_QUERY: query->gpu_addr: passed parameter is invalid");
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
				break;
			}
			if (query->query != KBASE_MEM_QUERY_COMMIT_SIZE &&
			    query->query != KBASE_MEM_QUERY_VA_SIZE &&
				query->query != KBASE_MEM_QUERY_FLAGS) {
				KBASE_DEBUG_PRINT_WARN(KBASE_MEM, "kbase_dispatch case KBASE_FUNC_MEM_QUERY: query->query = %lld unknown", (unsigned long long)query->query);
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
				break;
			}

			ukh->ret = kbase_mem_query(kctx, query->gpu_addr, query->query, &query->value);
			break;
		}
		break;

	case KBASE_FUNC_MEM_FLAGS_CHANGE:
		{
			kbase_uk_mem_flags_change * fc = args;
			if (sizeof(*fc) != args_size)
				goto bad_size;

			if ((fc->gpu_va & ~PAGE_MASK) && (fc->gpu_va >= PAGE_SIZE)) {
				KBASE_DEBUG_PRINT_WARN(KBASE_MEM, "kbase_dispatch case KBASE_FUNC_MEM_FLAGS_CHANGE: mem->gpu_va: passed parameter is invalid");
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
				break;
			}

			if (kbase_mem_flags_change(kctx, fc->gpu_va, fc->flags, fc->mask))
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;

			break;
		}
	case KBASE_FUNC_MEM_FREE:
		{
			kbase_uk_mem_free *mem = args;

			if (sizeof(*mem) != args_size)
				goto bad_size;

			if ((mem->gpu_addr & ~PAGE_MASK) && (mem->gpu_addr >= PAGE_SIZE)) {
				KBASE_DEBUG_PRINT_WARN(KBASE_MEM, "kbase_dispatch case KBASE_FUNC_MEM_FREE: mem->gpu_addr: passed parameter is invalid");
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
				break;
			}

			if (kbase_mem_free(kctx, mem->gpu_addr))
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			break;
		}

	case KBASE_FUNC_JOB_SUBMIT:
		{
			kbase_uk_job_submit *job = args;

			if (sizeof(*job) != args_size)
				goto bad_size;

			if (MALI_ERROR_NONE != kbase_jd_submit(kctx, job))
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			break;
		}

	case KBASE_FUNC_SYNC:
		{
			kbase_uk_sync_now *sn = args;

			if (sizeof(*sn) != args_size)
				goto bad_size;

			if (sn->sset.basep_sset.mem_handle & ~PAGE_MASK) {
				KBASE_DEBUG_PRINT_WARN(KBASE_MEM, "kbase_dispatch case KBASE_FUNC_SYNC: sn->sset.basep_sset.mem_handle: passed parameter is invalid");
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
				break;
			}

			if (MALI_ERROR_NONE != kbase_sync_now(kctx, &sn->sset))
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			break;
		}

	case KBASE_FUNC_POST_TERM:
		{
			kbase_event_close(kctx);
			break;
		}

	case KBASE_FUNC_HWCNT_SETUP:
		{
			kbase_uk_hwcnt_setup *setup = args;

			if (sizeof(*setup) != args_size)
				goto bad_size;

			if (MALI_ERROR_NONE != kbase_instr_hwcnt_setup(kctx, setup))
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			break;
		}

#if SLSI_INTEGRATION
	case KBASE_FUNC_HWCNT_UTIL_SETUP:
		{
			kbase_uk_hwcnt_setup *setup = args;

			if (sizeof(*setup) != args_size)
				goto bad_size;

			if (MALI_ERROR_NONE != kbase_instr_hwcnt_util_setup(kctx, setup))
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			break;
		}

	case KBASE_FUNC_HWCNT_GPR_DUMP:
		{
			kbase_uk_hwcnt_gpr_dump *dump = args;

			if (sizeof(*dump) != args_size)
				goto bad_size;

			if (MALI_ERROR_NONE != kbase_instr_hwcnt_gpr_dump(kctx, dump))
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			break;
		}

	case KBASE_FUNC_VSYNC_SKIP:
		{
			kbase_uk_vsync_skip *vskip = args;

			if (sizeof(*vskip) != args_size)
				goto bad_size;
#ifdef CONFIG_USE_VSYNC_SKIP
			/* increment vsync skip variable that is used in fimd driver */
			KBASE_TRACE_ADD_EXYNOS(kbdev, LSI_HWCNT_VSYNC_SKIP, NULL, NULL, 0u, vskip->skip_count);

			if (vskip->skip_count == 0) {
				s3c_fb_extra_vsync_wait_set(0);
			} else {
				s3c_fb_extra_vsync_wait_add(vskip->skip_count);
			}
#endif /* CONFIG_USE_VSYNC_SKIP */
			break;
		}
#endif

	case KBASE_FUNC_HWCNT_DUMP:
		{
			/* args ignored */
			if (MALI_ERROR_NONE != kbase_instr_hwcnt_dump(kctx))
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			break;
		}

	case KBASE_FUNC_HWCNT_CLEAR:
		{
			/* args ignored */
			if (MALI_ERROR_NONE != kbase_instr_hwcnt_clear(kctx))
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			break;
		}

	case KBASE_FUNC_CPU_PROPS_REG_DUMP:
		{
			kbase_uk_cpuprops *setup = args;

			if (sizeof(*setup) != args_size)
				goto bad_size;

			if (MALI_ERROR_NONE != kbase_cpuprops_uk_get_props(kctx, setup))
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			break;
		}

	case KBASE_FUNC_GPU_PROPS_REG_DUMP:
		{
			kbase_uk_gpuprops *setup = args;

			if (sizeof(*setup) != args_size)
				goto bad_size;

			if (MALI_ERROR_NONE != kbase_gpuprops_uk_get_props(kctx, setup))
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			break;
		}
	case KBASE_FUNC_FIND_CPU_MAPPING:
		{
			kbase_uk_find_cpu_mapping *find = args;
			struct kbase_cpu_mapping *map;

			if (sizeof(*find) != args_size)
				goto bad_size;

			if (find->gpu_addr & ~PAGE_MASK) {
				KBASE_DEBUG_PRINT_WARN(KBASE_MEM, "kbase_dispatch case KBASE_FUNC_FIND_CPU_MAPPING: find->gpu_addr: passed parameter is invalid");
				goto out_bad;
			}

			KBASE_DEBUG_ASSERT(find != NULL);
			if (find->size > SIZE_MAX || find->cpu_addr > ULONG_MAX)
				map = NULL;
			else
				map = kbasep_find_enclosing_cpu_mapping(kctx, find->gpu_addr, (uintptr_t) find->cpu_addr, (size_t) find->size);

			if (NULL != map) {
				find->uaddr = PTR_TO_U64(map->vma->vm_start);
				find->page_off = map->page_off;
			} else {
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
			}
			break;
		}
	case KBASE_FUNC_GET_VERSION:
		{
			kbase_uk_get_ddk_version *get_version = (kbase_uk_get_ddk_version *) args;

			if (sizeof(*get_version) != args_size)
				goto bad_size;

			/* version buffer size check is made in compile time assert */
			memcpy(get_version->version_buffer, KERNEL_SIDE_DDK_VERSION_STRING, sizeof(KERNEL_SIDE_DDK_VERSION_STRING));
			get_version->version_string_size = sizeof(KERNEL_SIDE_DDK_VERSION_STRING);
			break;
		}

	case KBASE_FUNC_STREAM_CREATE:
		{
#ifdef CONFIG_SYNC
			kbase_uk_stream_create *screate = (kbase_uk_stream_create *) args;

			if (sizeof(*screate) != args_size)
				goto bad_size;

			if (strnlen(screate->name, sizeof(screate->name)) >= sizeof(screate->name)) {
				/* not NULL terminated */
				ukh->ret = MALI_ERROR_FUNCTION_FAILED;
				break;
			}

			ukh->ret = kbase_stream_create(screate->name, &screate->fd);
#else /* CONFIG_SYNC */
			ukh->ret = MALI_ERROR_FUNCTION_FAILED;
#endif /* CONFIG_SYNC */
			break;
		}
	case KBASE_FUNC_FENCE_VALIDATE:
		{
#ifdef CONFIG_SYNC
			kbase_uk_fence_validate *fence_validate = (kbase_uk_fence_validate *) args;
			if (sizeof(*fence_validate) != args_size)
				goto bad_size;

			ukh->ret = kbase_fence_validate(fence_validate->fd);
#endif /* CONFIG_SYNC */
			break;
		}

	case KBASE_FUNC_EXT_BUFFER_LOCK:
		{
#ifdef CONFIG_KDS
			ukh->ret = kbase_external_buffer_lock(kctx, (kbase_uk_ext_buff_kds_data *) args, args_size);
#endif /* CONFIG_KDS */
			break;
		}

	case KBASE_FUNC_SET_TEST_DATA:
		{
#if MALI_UNIT_TEST
			kbase_uk_set_test_data *set_data = args;

			shared_kernel_test_data = set_data->test_data;
			shared_kernel_test_data.kctx.value = kctx;
			shared_kernel_test_data.mm.value = (void *)current->mm;
			ukh->ret = MALI_ERROR_NONE;
#endif /* MALI_UNIT_TEST */
			break;
		}

	case KBASE_FUNC_INJECT_ERROR:
		{
#ifdef CONFIG_MALI_ERROR_INJECT
			unsigned long flags;
			kbase_error_params params = ((kbase_uk_error_params *) args)->params;
			/*mutex lock */
			spin_lock_irqsave(&kbdev->osdev.reg_op_lock, flags);
			ukh->ret = job_atom_inject_error(&params);
			spin_unlock_irqrestore(&kbdev->osdev.reg_op_lock, flags);
			/*mutex unlock */
#endif /* CONFIG_MALI_ERROR_INJECT */
			break;
		}

	case KBASE_FUNC_MODEL_CONTROL:
		{
#ifdef CONFIG_MALI_NO_MALI
			unsigned long flags;
			kbase_model_control_params params = ((kbase_uk_model_control_params *) args)->params;
			/*mutex lock */
			spin_lock_irqsave(&kbdev->osdev.reg_op_lock, flags);
			ukh->ret = midg_model_control(kbdev->osdev.model, &params);
			spin_unlock_irqrestore(&kbdev->osdev.reg_op_lock, flags);
			/*mutex unlock */
#endif /* CONFIG_MALI_NO_MALI */
			break;
		}

	case KBASE_FUNC_KEEP_GPU_POWERED:
		{
			kbase_uk_keep_gpu_powered *kgp = (kbase_uk_keep_gpu_powered *) args;
			/* A suspend won't happen here, because we're in a syscall from a
			 * userspace thread.
			 *
			 * Nevertheless, we'd get the wrong pm_context_active/idle counting
			 * here if a suspend did happen, so let's assert it won't: */
			KBASE_DEBUG_ASSERT(!kbase_pm_is_suspending(kbdev));

			if (kgp->enabled && !kctx->keep_gpu_powered) {
				kbase_pm_context_active(kbdev);
				atomic_inc(&kbdev->keep_gpu_powered_count);
				kctx->keep_gpu_powered = MALI_TRUE;
			} else if (!kgp->enabled && kctx->keep_gpu_powered) {
				atomic_dec(&kbdev->keep_gpu_powered_count);
				kbase_pm_context_idle(kbdev);
				kctx->keep_gpu_powered = MALI_FALSE;
			}

			break;
		}

	case KBASE_FUNC_GET_PROFILING_CONTROLS :
		{
			struct kbase_uk_profiling_controls *controls = \
					(struct kbase_uk_profiling_controls *)args;
			u32 i;

			if (sizeof(*controls) != args_size)
				goto bad_size;

			for (i = FBDUMP_CONTROL_MIN; i < FBDUMP_CONTROL_MAX; i++) {
				controls->profiling_controls[i] = kbase_get_profiling_control(kbdev, i);
			}

			break;
		}

	/* used only for testing purposes; these controls are to be set by gator through gator API */
	case KBASE_FUNC_SET_PROFILING_CONTROLS :
		{
			struct kbase_uk_profiling_controls *controls = \
					(struct kbase_uk_profiling_controls *)args;
			u32 i;

			if (sizeof(*controls) != args_size)
				goto bad_size;

			for (i = FBDUMP_CONTROL_MIN; i < FBDUMP_CONTROL_MAX; i++)
			{
				_mali_profiling_control(i, controls->profiling_controls[i]);
			}

			break;
		}

#if SLSI_INTEGRATION
	case KBASE_FUNC_SET_MIN_LOCK :
		{
			struct exynos_context *platform = (struct exynos_context *)kbdev->platform_context;
			if (!kctx->ctx_need_qos) {
				kctx->ctx_need_qos = true;
				set_hmp_boost(1);
				set_hmp_aggressive_up_migration(true);
				set_hmp_aggressive_yield(true);
#if defined(SET_MINLOCK)
				platform->custom_cpu_max_lock = EGL_MAX_CLOCK;
				platform->target_lock_type = BOOST_LOCK;
				gpu_dvfs_handler_control(kbdev, GPU_HANDLER_DVFS_MIN_LOCK, GPU_MIN_CLOCK);
#ifdef CONFIG_ARM_EXYNOS5430_BUS_DEVFREQ
				pm_qos_update_request(&exynos5_g3d_cpu_egl_min_qos, EGL_MIN_CLOCK);
#endif
#endif
			}
			break;
		}

	case KBASE_FUNC_UNSET_MIN_LOCK :
		{
			struct exynos_context *platform = (struct exynos_context *)kbdev->platform_context;
			if (kctx->ctx_need_qos) {
				kctx->ctx_need_qos = false;
				set_hmp_boost(0);
				set_hmp_aggressive_up_migration(false);
				set_hmp_aggressive_yield(false);
#if defined(SET_MINLOCK)
				platform->custom_cpu_max_lock = 0;
				platform->target_lock_type = BOOST_LOCK;
				gpu_dvfs_handler_control(kbdev, GPU_HANDLER_DVFS_MIN_UNLOCK, 0);
#ifdef CONFIG_ARM_EXYNOS5430_BUS_DEVFREQ
				pm_qos_update_request(&exynos5_g3d_cpu_egl_min_qos, 0);
#endif
#endif
			}
			break;
		}

	case KBASE_FUNC_UNSET_GPU_MIN_LOCK :
		{
#if defined(SET_MINLOCK)
			struct exynos_context *platform = (struct exynos_context *)kbdev->platform_context;
			platform->target_lock_type = BOOST_LOCK;
			gpu_dvfs_handler_control(kbdev, GPU_HANDLER_DVFS_MIN_UNLOCK, 0);
#endif
			break;
		}
#endif /* SLSI_INTEGRATION */

	default:
		dev_err(kbdev->osdev.dev, "unknown ioctl %u", id);
		goto out_bad;
	}

	return MALI_ERROR_NONE;

 bad_size:
	dev_err(kbdev->osdev.dev, "Wrong syscall size (%d) for %08x\n", args_size, id);
 out_bad:
	return MALI_ERROR_FUNCTION_FAILED;
}

static struct kbase_device *to_kbase_device(struct device *dev)
{
	return dev_get_drvdata(dev);
}

/*
 * API to acquire device list semaphore and
 * return pointer to the device list head
 */
const struct list_head *kbase_dev_list_get(void)
{
	down(&kbase_dev_list_lock);
	return &kbase_dev_list;
}

/* API to release the device list semaphore */
void kbase_dev_list_put(const struct list_head *dev_list)
{
	up(&kbase_dev_list_lock);
}

/* Find a particular kbase device (as specified by minor number), or find the "first" device if -1 is specified */
struct kbase_device *kbase_find_device(int minor)
{
	struct kbase_device *kbdev = NULL;
	struct list_head *entry;

	down(&kbase_dev_list_lock);
	list_for_each(entry, &kbase_dev_list) {
		struct kbase_device *tmp;

		tmp = list_entry(entry, struct kbase_device, osdev.entry);
		if (tmp->osdev.mdev.minor == minor || minor == -1) {
			kbdev = tmp;
			get_device(kbdev->osdev.dev);
			break;
		}
	}
	up(&kbase_dev_list_lock);

	return kbdev;
}
EXPORT_SYMBOL(kbase_find_device);

void kbase_release_device(struct kbase_device *kbdev)
{
	put_device(kbdev->osdev.dev);
}
EXPORT_SYMBOL(kbase_release_device);

static int kbase_open(struct inode *inode, struct file *filp)
{
	struct kbase_device *kbdev = NULL;
	kbase_context *kctx;
	int ret = 0;

	kbdev = kbase_find_device(iminor(inode));

	if (!kbdev)
		return -ENODEV;

	kctx = kbase_create_context(kbdev);
	if (!kctx) {
		ret = -ENOMEM;
		goto out;
	}

	init_waitqueue_head(&kctx->osctx.event_queue);
	filp->private_data = kctx;

	dev_dbg(kbdev->osdev.dev, "created base context\n");

	{
		kbasep_kctx_list_element *element;

		element = kzalloc(sizeof(kbasep_kctx_list_element), GFP_KERNEL);
		if (element) {
			mutex_lock(&kbdev->kctx_list_lock);
			element->kctx = kctx;
			list_add(&element->link, &kbdev->kctx_list);
			mutex_unlock(&kbdev->kctx_list_lock);
		} else {
			/* we don't treat this as a fail - just warn about it */
			printk(KERN_WARNING KBASE_DRV_NAME "couldn't add kctx to kctx_list\n");
		}
	}
	return 0;

 out:
	kbase_release_device(kbdev);
	return ret;
}

static int kbase_release(struct inode *inode, struct file *filp)
{
	kbase_context *kctx = filp->private_data;
	struct kbase_device *kbdev = kctx->kbdev;
	kbasep_kctx_list_element *element, *tmp;
	mali_bool found_element = MALI_FALSE;

	mutex_lock(&kbdev->kctx_list_lock);
	list_for_each_entry_safe(element, tmp, &kbdev->kctx_list, link) {
		if (element->kctx == kctx) {
			list_del(&element->link);
			kfree(element);
			found_element = MALI_TRUE;
		}
	}
	mutex_unlock(&kbdev->kctx_list_lock);
	if (!found_element)
		printk(KERN_WARNING KBASE_DRV_NAME "kctx not in kctx_list\n");

	filp->private_data = NULL;
	kbase_destroy_context(kctx);

	dev_dbg(kbdev->osdev.dev, "deleted base context\n");
	kbase_release_device(kbdev);
	return 0;
}

#if SLSI_INTEGRATION
#define CALL_MAX_SIZE 540
#else
#define CALL_MAX_SIZE 536
#endif

static long kbase_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	u64 msg[(CALL_MAX_SIZE + 7) >> 3] = { 0xdeadbeefdeadbeefull };	/* alignment fixup */
	u32 size = _IOC_SIZE(cmd);
	kbase_context *kctx = filp->private_data;

	if (size > CALL_MAX_SIZE)
		return -ENOTTY;

	if (0 != copy_from_user(&msg, (void *)arg, size)) {
		pr_err("failed to copy ioctl argument into kernel space\n");
		return -EFAULT;
	}

	if (MALI_ERROR_NONE != kbase_dispatch(kctx, &msg, size))
		return -EFAULT;

	if (0 != copy_to_user((void *)arg, &msg, size)) {
		pr_err("failed to copy results of UK call back to user space\n");
		return -EFAULT;
	}
	return 0;
}

static ssize_t kbase_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	kbase_context *kctx = filp->private_data;
	base_jd_event_v2 uevent;
	int out_count = 0;

	if (count < sizeof(uevent))
		return -ENOBUFS;

	do {
		while (kbase_event_dequeue(kctx, &uevent)) {
			if (out_count > 0)
				goto out;

			if (filp->f_flags & O_NONBLOCK)
				return -EAGAIN;

			if (wait_event_interruptible(kctx->osctx.event_queue, kbase_event_pending(kctx)))
				return -ERESTARTSYS;
		}
		if (uevent.event_code == BASE_JD_EVENT_DRV_TERMINATED) {
			if (out_count == 0)
				return -EPIPE;
			goto out;
		}

		if (copy_to_user(buf, &uevent, sizeof(uevent)))
			return -EFAULT;

		buf += sizeof(uevent);
		out_count++;
		count -= sizeof(uevent);
	} while (count >= sizeof(uevent));

 out:
	return out_count * sizeof(uevent);
}

static unsigned int kbase_poll(struct file *filp, poll_table *wait)
{
	kbase_context *kctx = filp->private_data;

	poll_wait(filp, &kctx->osctx.event_queue, wait);
	if (kbase_event_pending(kctx))
		return POLLIN | POLLRDNORM;

	return 0;
}

void kbase_event_wakeup(kbase_context *kctx)
{
	KBASE_DEBUG_ASSERT(kctx);

	wake_up_interruptible(&kctx->osctx.event_queue);
}

KBASE_EXPORT_TEST_API(kbase_event_wakeup)

int kbase_check_flags(int flags)
{
	/* Enforce that the driver keeps the O_CLOEXEC flag so that execve() always
	 * closes the file descriptor in a child process.
	 */
	if (0 == (flags & O_CLOEXEC))
		return -EINVAL;

	return 0;
}

static const struct file_operations kbase_fops = {
	.owner = THIS_MODULE,
	.open = kbase_open,
	.release = kbase_release,
	.read = kbase_read,
	.poll = kbase_poll,
	.unlocked_ioctl = kbase_ioctl,
	.compat_ioctl = kbase_ioctl,
	.mmap = kbase_mmap,
	.check_flags = kbase_check_flags,
};

#ifndef CONFIG_MALI_NO_MALI
void kbase_os_reg_write(kbase_device *kbdev, u16 offset, u32 value)
{
	writel(value, kbdev->osdev.reg + offset);
}

u32 kbase_os_reg_read(kbase_device *kbdev, u16 offset)
{
	return readl(kbdev->osdev.reg + offset);
}

static void *kbase_tag(void *ptr, u32 tag)
{
	return (void *)(((uintptr_t) ptr) | tag);
}

static void *kbase_untag(void *ptr)
{
	return (void *)(((uintptr_t) ptr) & ~3);
}

static irqreturn_t kbase_job_irq_handler(int irq, void *data)
{
	unsigned long flags;
	struct kbase_device *kbdev = kbase_untag(data);
	u32 val;

	spin_lock_irqsave(&kbdev->pm.gpu_powered_lock, flags);

	if (!kbdev->pm.gpu_powered) {
		/* GPU is turned off - IRQ is not for us */
		spin_unlock_irqrestore(&kbdev->pm.gpu_powered_lock, flags);
		return IRQ_NONE;
	}

	val = kbase_reg_read(kbdev, JOB_CONTROL_REG(JOB_IRQ_STATUS), NULL);

#ifdef CONFIG_MALI_DEBUG
	if (!kbdev->pm.driver_ready_for_irqs)
		dev_dbg(kbdev->osdev.dev, "%s: irq %d irqstatus 0x%x before driver is ready\n",
				__func__, irq, val );
#endif /* CONFIG_MALI_DEBUG */
	spin_unlock_irqrestore(&kbdev->pm.gpu_powered_lock, flags);

	if (!val)
		return IRQ_NONE;

	dev_dbg(kbdev->osdev.dev, "%s: irq %d irqstatus 0x%x\n", __func__, irq, val);

	kbase_job_done(kbdev, val);

	return IRQ_HANDLED;
}

KBASE_EXPORT_TEST_API(kbase_job_irq_handler);

static irqreturn_t kbase_mmu_irq_handler(int irq, void *data)
{
	unsigned long flags;
	struct kbase_device *kbdev = kbase_untag(data);
	u32 val;

	spin_lock_irqsave(&kbdev->pm.gpu_powered_lock, flags);

	if (!kbdev->pm.gpu_powered) {
		/* GPU is turned off - IRQ is not for us */
		spin_unlock_irqrestore(&kbdev->pm.gpu_powered_lock, flags);
		return IRQ_NONE;
	}

	val = kbase_reg_read(kbdev, MMU_REG(MMU_IRQ_STATUS), NULL);

#ifdef CONFIG_MALI_DEBUG
	if (!kbdev->pm.driver_ready_for_irqs)
		dev_dbg(kbdev->osdev.dev, "%s: irq %d irqstatus 0x%x before driver is ready\n",
				__func__, irq, val );
#endif /* CONFIG_MALI_DEBUG */
	spin_unlock_irqrestore(&kbdev->pm.gpu_powered_lock, flags);

	if (!val)
		return IRQ_NONE;

	dev_dbg(kbdev->osdev.dev, "%s: irq %d irqstatus 0x%x\n", __func__, irq, val);

	kbase_mmu_interrupt(kbdev, val);

	return IRQ_HANDLED;
}

static irqreturn_t kbase_gpu_irq_handler(int irq, void *data)
{
	unsigned long flags;
	struct kbase_device *kbdev = kbase_untag(data);
	u32 val;

	spin_lock_irqsave(&kbdev->pm.gpu_powered_lock, flags);

	if (!kbdev->pm.gpu_powered) {
		/* GPU is turned off - IRQ is not for us */
		spin_unlock_irqrestore(&kbdev->pm.gpu_powered_lock, flags);
		return IRQ_NONE;
	}

	val = kbase_reg_read(kbdev, GPU_CONTROL_REG(GPU_IRQ_STATUS), NULL);

#ifdef CONFIG_MALI_DEBUG
	if (!kbdev->pm.driver_ready_for_irqs)
		dev_dbg(kbdev->osdev.dev, "%s: irq %d irqstatus 0x%x before driver is ready\n",
				__func__, irq, val );
#endif /* CONFIG_MALI_DEBUG */
	spin_unlock_irqrestore(&kbdev->pm.gpu_powered_lock, flags);

	if (!val)
		return IRQ_NONE;

	dev_dbg(kbdev->osdev.dev, "%s: irq %d irqstatus 0x%x\n", __func__, irq, val);

	kbase_gpu_interrupt(kbdev, val);

	return IRQ_HANDLED;
}

static irq_handler_t kbase_handler_table[] = {
	[JOB_IRQ_TAG] = kbase_job_irq_handler,
	[MMU_IRQ_TAG] = kbase_mmu_irq_handler,
	[GPU_IRQ_TAG] = kbase_gpu_irq_handler,
};

#ifdef CONFIG_MALI_DEBUG
#define  JOB_IRQ_HANDLER JOB_IRQ_TAG
#define  MMU_IRQ_HANDLER MMU_IRQ_TAG
#define  GPU_IRQ_HANDLER GPU_IRQ_TAG

/**
 * @brief Registers given interrupt handler for requested interrupt type
 *        Case irq handler is not specified default handler shall be registered
 *
 * @param[in] kbdev           - Device for which the handler is to be registered
 * @param[in] custom_handler  - Handler to be registered
 * @param[in] irq_type        - Interrupt type
 * @return	MALI_ERROR_NONE case success, MALI_ERROR_FUNCTION_FAILED otherwise
 */
static mali_error kbase_set_custom_irq_handler(kbase_device *kbdev, irq_handler_t custom_handler, int irq_type)
{
	struct kbase_os_device *osdev = &kbdev->osdev;
	mali_error result = MALI_ERROR_NONE;
	irq_handler_t requested_irq_handler = NULL;
	KBASE_DEBUG_ASSERT((JOB_IRQ_HANDLER <= irq_type) && (GPU_IRQ_HANDLER >= irq_type));

	/* Release previous handler */
	if (osdev->irqs[irq_type].irq)
		free_irq(osdev->irqs[irq_type].irq, kbase_tag(kbdev, irq_type));

	requested_irq_handler = (NULL != custom_handler) ? custom_handler : kbase_handler_table[irq_type];

	if (0 != request_irq(osdev->irqs[irq_type].irq, requested_irq_handler, osdev->irqs[irq_type].flags | IRQF_SHARED, dev_name(osdev->dev), kbase_tag(kbdev, irq_type))) {
		result = MALI_ERROR_FUNCTION_FAILED;
		dev_err(osdev->dev, "Can't request interrupt %d (index %d)\n", osdev->irqs[irq_type].irq, irq_type);
#ifdef CONFIG_SPARSE_IRQ
		dev_err(osdev->dev, "You have CONFIG_SPARSE_IRQ support enabled - is the interrupt number correct for this configuration?\n");
#endif /* CONFIG_SPARSE_IRQ */
	}

	return result;
}

KBASE_EXPORT_TEST_API(kbase_set_custom_irq_handler)

/* test correct interrupt assigment and reception by cpu */
typedef struct kbasep_irq_test {
	struct hrtimer timer;
	wait_queue_head_t wait;
	int triggered;
	u32 timeout;
} kbasep_irq_test;

static kbasep_irq_test kbasep_irq_test_data;

#define IRQ_TEST_TIMEOUT    500

static irqreturn_t kbase_job_irq_test_handler(int irq, void *data)
{
	unsigned long flags;
	struct kbase_device *kbdev = kbase_untag(data);
	u32 val;

	spin_lock_irqsave(&kbdev->pm.gpu_powered_lock, flags);

	if (!kbdev->pm.gpu_powered) {
		/* GPU is turned off - IRQ is not for us */
		spin_unlock_irqrestore(&kbdev->pm.gpu_powered_lock, flags);
		return IRQ_NONE;
	}

	val = kbase_reg_read(kbdev, JOB_CONTROL_REG(JOB_IRQ_STATUS), NULL);

	spin_unlock_irqrestore(&kbdev->pm.gpu_powered_lock, flags);

	if (!val)
		return IRQ_NONE;

	dev_dbg(kbdev->osdev.dev, "%s: irq %d irqstatus 0x%x\n", __func__, irq, val);

	kbasep_irq_test_data.triggered = 1;
	wake_up(&kbasep_irq_test_data.wait);

	kbase_reg_write(kbdev, JOB_CONTROL_REG(JOB_IRQ_CLEAR), val, NULL);

	return IRQ_HANDLED;
}

static irqreturn_t kbase_mmu_irq_test_handler(int irq, void *data)
{
	unsigned long flags;
	struct kbase_device *kbdev = kbase_untag(data);
	u32 val;

	spin_lock_irqsave(&kbdev->pm.gpu_powered_lock, flags);

	if (!kbdev->pm.gpu_powered) {
		/* GPU is turned off - IRQ is not for us */
		spin_unlock_irqrestore(&kbdev->pm.gpu_powered_lock, flags);
		return IRQ_NONE;
	}

	val = kbase_reg_read(kbdev, MMU_REG(MMU_IRQ_STATUS), NULL);

	spin_unlock_irqrestore(&kbdev->pm.gpu_powered_lock, flags);

	if (!val)
		return IRQ_NONE;

	dev_dbg(kbdev->osdev.dev, "%s: irq %d irqstatus 0x%x\n", __func__, irq, val);

	kbasep_irq_test_data.triggered = 1;
	wake_up(&kbasep_irq_test_data.wait);

	kbase_reg_write(kbdev, MMU_REG(MMU_IRQ_CLEAR), val, NULL);

	return IRQ_HANDLED;
}

static enum hrtimer_restart kbasep_test_interrupt_timeout(struct hrtimer *timer)
{
	kbasep_irq_test *test_data = container_of(timer, kbasep_irq_test, timer);

	test_data->timeout = 1;
	test_data->triggered = 1;
	wake_up(&test_data->wait);
	return HRTIMER_NORESTART;
}

static mali_error kbasep_common_test_interrupt(kbase_device * const kbdev, u32 tag)
{
	struct kbase_os_device *osdev = &kbdev->osdev;
	mali_error err = MALI_ERROR_NONE;
	irq_handler_t test_handler;

	u32 old_mask_val;
	u16 mask_offset;
	u16 rawstat_offset;

	switch (tag) {
	case JOB_IRQ_TAG:
		test_handler = kbase_job_irq_test_handler;
		rawstat_offset = JOB_CONTROL_REG(JOB_IRQ_RAWSTAT);
		mask_offset = JOB_CONTROL_REG(JOB_IRQ_MASK);
		break;
	case MMU_IRQ_TAG:
		test_handler = kbase_mmu_irq_test_handler;
		rawstat_offset = MMU_REG(MMU_IRQ_RAWSTAT);
		mask_offset = MMU_REG(MMU_IRQ_MASK);
		break;
	case GPU_IRQ_TAG:
		/* already tested by pm_driver - bail out */
	default:
		return MALI_ERROR_NONE;
	}

	/* store old mask */
	old_mask_val = kbase_reg_read(kbdev, mask_offset, NULL);
	/* mask interrupts */
	kbase_reg_write(kbdev, mask_offset, 0x0, NULL);

	if (osdev->irqs[tag].irq) {
		/* release original handler and install test handler */
		if (MALI_ERROR_NONE != kbase_set_custom_irq_handler(kbdev, test_handler, tag)) {
			err = MALI_ERROR_FUNCTION_FAILED;
		} else {
			kbasep_irq_test_data.timeout = 0;
			hrtimer_init(&kbasep_irq_test_data.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
			kbasep_irq_test_data.timer.function = kbasep_test_interrupt_timeout;

			/* trigger interrupt */
			kbase_reg_write(kbdev, mask_offset, 0x1, NULL);
			kbase_reg_write(kbdev, rawstat_offset, 0x1, NULL);

			hrtimer_start(&kbasep_irq_test_data.timer, HR_TIMER_DELAY_MSEC(IRQ_TEST_TIMEOUT), HRTIMER_MODE_REL);

			wait_event(kbasep_irq_test_data.wait, kbasep_irq_test_data.triggered != 0);

			if (kbasep_irq_test_data.timeout != 0) {
				dev_err(osdev->dev, "Interrupt %d (index %d) didn't reach CPU.\n", osdev->irqs[tag].irq, tag);
				err = MALI_ERROR_FUNCTION_FAILED;
			} else {
				dev_dbg(osdev->dev, "Interrupt %d (index %d) reached CPU.\n", osdev->irqs[tag].irq, tag);
			}

			hrtimer_cancel(&kbasep_irq_test_data.timer);
			kbasep_irq_test_data.triggered = 0;

			/* mask interrupts */
			kbase_reg_write(kbdev, mask_offset, 0x0, NULL);

			/* release test handler */
			free_irq(osdev->irqs[tag].irq, kbase_tag(kbdev, tag));
		}

		/* restore original interrupt */
		if (request_irq(osdev->irqs[tag].irq, kbase_handler_table[tag], osdev->irqs[tag].flags | IRQF_SHARED, dev_name(osdev->dev), kbase_tag(kbdev, tag))) {
			dev_err(osdev->dev, "Can't restore original interrupt %d (index %d)\n", osdev->irqs[tag].irq, tag);
			err = MALI_ERROR_FUNCTION_FAILED;
		}
	}
	/* restore old mask */
	kbase_reg_write(kbdev, mask_offset, old_mask_val, NULL);

	return err;
}

static mali_error kbasep_common_test_interrupt_handlers(kbase_device * const kbdev)
{
	struct kbase_os_device *osdev = &kbdev->osdev;
	mali_error err;

	init_waitqueue_head(&kbasep_irq_test_data.wait);
	kbasep_irq_test_data.triggered = 0;

	/* A suspend won't happen during startup/insmod */
	kbase_pm_context_active(kbdev);

	err = kbasep_common_test_interrupt(kbdev, JOB_IRQ_TAG);
	if (MALI_ERROR_NONE != err) {
		dev_err(osdev->dev, "Interrupt JOB_IRQ didn't reach CPU. Check interrupt assignments.\n");
		goto out;
	}

	err = kbasep_common_test_interrupt(kbdev, MMU_IRQ_TAG);
	if (MALI_ERROR_NONE != err) {
		dev_err(osdev->dev, "Interrupt MMU_IRQ didn't reach CPU. Check interrupt assignments.\n");
		goto out;
	}

	dev_err(osdev->dev, "Interrupts are correctly assigned.\n");

 out:
	kbase_pm_context_idle(kbdev);

	return err;

}
#endif /* CONFIG_MALI_DEBUG */

static int kbase_install_interrupts(kbase_device *kbdev)
{
	struct kbase_os_device *osdev = &kbdev->osdev;
	u32 nr = ARRAY_SIZE(kbase_handler_table);
	int err;
	u32 i;

	for (i = 0; i < nr; i++) {
		err = request_irq(osdev->irqs[i].irq, kbase_handler_table[i], osdev->irqs[i].flags | IRQF_SHARED, dev_name(osdev->dev), kbase_tag(kbdev, i));
		if (err) {
			dev_err(osdev->dev, "Can't request interrupt %d (index %d)\n", osdev->irqs[i].irq, i);
#ifdef CONFIG_SPARSE_IRQ
			dev_err(osdev->dev, "You have CONFIG_SPARSE_IRQ support enabled - is the interrupt number correct for this configuration?\n");
#endif /* CONFIG_SPARSE_IRQ */
			goto release;
		}
	}

	return 0;

 release:
	while (i-- > 0)
		free_irq(osdev->irqs[i].irq, kbase_tag(kbdev, i));

	return err;
}

static void kbase_release_interrupts(kbase_device *kbdev)
{
	struct kbase_os_device *osdev = &kbdev->osdev;
	u32 nr = ARRAY_SIZE(kbase_handler_table);
	u32 i;

	for (i = 0; i < nr; i++) {
		if (osdev->irqs[i].irq)
			free_irq(osdev->irqs[i].irq, kbase_tag(kbdev, i));
	}
}

void kbase_synchronize_irqs(kbase_device *kbdev)
{
	struct kbase_os_device *osdev = &kbdev->osdev;
	u32 nr = ARRAY_SIZE(kbase_handler_table);
	u32 i;

	for (i = 0; i < nr; i++) {
		if (osdev->irqs[i].irq)
			synchronize_irq(osdev->irqs[i].irq);
	}
}

#endif /* CONFIG_MALI_NO_MALI */


/** Show callback for the @c power_policy sysfs file.
 *
 * This function is called to get the contents of the @c power_policy sysfs
 * file. This is a list of the available policies with the currently active one
 * surrounded by square brackets.
 *
 * @param dev	The device this sysfs file is for
 * @param attr	The attributes of the sysfs file
 * @param buf	The output buffer for the sysfs file contents
 *
 * @return The number of bytes output to @c buf.
 */
static ssize_t show_policy(struct device *dev, struct device_attribute *attr, char *const buf)
{
	struct kbase_device *kbdev;
	const struct kbase_pm_policy *current_policy;
	const struct kbase_pm_policy *const *policy_list;
	int policy_count;
	int i;
	ssize_t ret = 0;

	kbdev = to_kbase_device(dev);

	if (!kbdev)
		return -ENODEV;

	current_policy = kbase_pm_get_policy(kbdev);

	policy_count = kbase_pm_list_policies(&policy_list);

	for (i = 0; i < policy_count && ret < PAGE_SIZE; i++) {
		if (policy_list[i] == current_policy)
			ret += scnprintf(buf + ret, PAGE_SIZE - ret, "[%s] ", policy_list[i]->name);
		else
			ret += scnprintf(buf + ret, PAGE_SIZE - ret, "%s ", policy_list[i]->name);
	}

	if (ret < PAGE_SIZE - 1) {
		ret += scnprintf(buf + ret, PAGE_SIZE - ret, "\n");
	} else {
		buf[PAGE_SIZE - 2] = '\n';
		buf[PAGE_SIZE - 1] = '\0';
		ret = PAGE_SIZE - 1;
	}

	return ret;
}

/** Store callback for the @c power_policy sysfs file.
 *
 * This function is called when the @c power_policy sysfs file is written to.
 * It matches the requested policy against the available policies and if a
 * matching policy is found calls @ref kbase_pm_set_policy to change the
 * policy.
 *
 * @param dev	The device with sysfs file is for
 * @param attr	The attributes of the sysfs file
 * @param buf	The value written to the sysfs file
 * @param count	The number of bytes written to the sysfs file
 *
 * @return @c count if the function succeeded. An error code on failure.
 */
static ssize_t set_policy(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct kbase_device *kbdev;
	const struct kbase_pm_policy *new_policy = NULL;
	const struct kbase_pm_policy *const *policy_list;
	int policy_count;
	int i;

	kbdev = to_kbase_device(dev);

	if (!kbdev)
		return -ENODEV;

	policy_count = kbase_pm_list_policies(&policy_list);

	for (i = 0; i < policy_count; i++) {
		if (sysfs_streq(policy_list[i]->name, buf)) {
			new_policy = policy_list[i];
			break;
		}
	}

	if (!new_policy) {
		dev_err(dev, "power_policy: policy not found\n");
		return -EINVAL;
	}

	kbase_pm_set_policy(kbdev, new_policy);
#if SLSI_INTEGRATION
	kbdev->hwcnt.prev_policy = new_policy;
#endif
	return count;
}

/** The sysfs file @c power_policy.
 *
 * This is used for obtaining information about the available policies,
 * determining which policy is currently active, and changing the active
 * policy.
 */
DEVICE_ATTR(power_policy, S_IRUGO | S_IWUSR, show_policy, set_policy);

/** Show callback for the @c core_availability_policy sysfs file.
 *
 * This function is called to get the contents of the @c core_availability_policy
 * sysfs file. This is a list of the available policies with the currently
 * active one surrounded by square brackets.
 *
 * @param dev	The device this sysfs file is for
 * @param attr	The attributes of the sysfs file
 * @param buf	The output buffer for the sysfs file contents
 *
 * @return The number of bytes output to @c buf.
 */
static ssize_t show_ca_policy(struct device *dev, struct device_attribute *attr, char *const buf)
{
	struct kbase_device *kbdev;
	const struct kbase_pm_ca_policy *current_policy;
	const struct kbase_pm_ca_policy *const *policy_list;
	int policy_count;
	int i;
	ssize_t ret = 0;

	kbdev = to_kbase_device(dev);

	if (!kbdev)
		return -ENODEV;

	current_policy = kbase_pm_ca_get_policy(kbdev);

	policy_count = kbase_pm_ca_list_policies(&policy_list);

	for (i = 0; i < policy_count && ret < PAGE_SIZE; i++) {
		if (policy_list[i] == current_policy)
			ret += scnprintf(buf + ret, PAGE_SIZE - ret, "[%s] ", policy_list[i]->name);
		else
			ret += scnprintf(buf + ret, PAGE_SIZE - ret, "%s ", policy_list[i]->name);
	}

	if (ret < PAGE_SIZE - 1) {
		ret += scnprintf(buf + ret, PAGE_SIZE - ret, "\n");
	} else {
		buf[PAGE_SIZE - 2] = '\n';
		buf[PAGE_SIZE - 1] = '\0';
		ret = PAGE_SIZE - 1;
	}

	return ret;
}

/** Store callback for the @c core_availability_policy sysfs file.
 *
 * This function is called when the @c core_availability_policy sysfs file is
 * written to. It matches the requested policy against the available policies
 * and if a matching policy is found calls @ref kbase_pm_set_policy to change
 * the policy.
 *
 * @param dev	The device with sysfs file is for
 * @param attr	The attributes of the sysfs file
 * @param buf	The value written to the sysfs file
 * @param count	The number of bytes written to the sysfs file
 *
 * @return @c count if the function succeeded. An error code on failure.
 */
static ssize_t set_ca_policy(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct kbase_device *kbdev;
	const struct kbase_pm_ca_policy *new_policy = NULL;
	const struct kbase_pm_ca_policy *const *policy_list;
	int policy_count;
	int i;

	kbdev = to_kbase_device(dev);

	if (!kbdev)
		return -ENODEV;

	policy_count = kbase_pm_ca_list_policies(&policy_list);

	for (i = 0; i < policy_count; i++) {
		if (sysfs_streq(policy_list[i]->name, buf)) {
			new_policy = policy_list[i];
			break;
		}
	}

	if (!new_policy) {
		dev_err(dev, "core_availability_policy: policy not found\n");
		return -EINVAL;
	}

	kbase_pm_ca_set_policy(kbdev, new_policy);

	return count;
}

/** The sysfs file @c core_availability_policy
 *
 * This is used for obtaining information about the available policies,
 * determining which policy is currently active, and changing the active
 * policy.
 */
DEVICE_ATTR(core_availability_policy, S_IRUGO | S_IWUSR, show_ca_policy, set_ca_policy);

/** Show callback for the @c core_mask sysfs file.
 *
 * This function is called to get the contents of the @c core_mask sysfs
 * file.
 *
 * @param dev	The device this sysfs file is for
 * @param attr	The attributes of the sysfs file
 * @param buf	The output buffer for the sysfs file contents
 *
 * @return The number of bytes output to @c buf.
 */
static ssize_t show_core_mask(struct device *dev, struct device_attribute *attr, char *const buf)
{
	struct kbase_device *kbdev;
	ssize_t ret = 0;

	kbdev = to_kbase_device(dev);

	if (!kbdev)
		return -ENODEV;

	ret += scnprintf(buf + ret, PAGE_SIZE - ret, "Current core mask : 0x%llX\n", kbdev->pm.debug_core_mask);
	ret += scnprintf(buf + ret, PAGE_SIZE - ret, "Available core mask : 0x%llX\n", kbdev->shader_present_bitmap);

	return ret;
}

/** Store callback for the @c core_mask sysfs file.
 *
 * This function is called when the @c core_mask sysfs file is written to.
 *
 * @param dev	The device with sysfs file is for
 * @param attr	The attributes of the sysfs file
 * @param buf	The value written to the sysfs file
 * @param count	The number of bytes written to the sysfs file
 *
 * @return @c count if the function succeeded. An error code on failure.
 */
static ssize_t set_core_mask(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct kbase_device *kbdev;
	u64 new_core_mask;

	kbdev = to_kbase_device(dev);

	if (!kbdev)
		return -ENODEV;

	new_core_mask = simple_strtoull(buf, NULL, 16);

	if ((new_core_mask & kbdev->shader_present_bitmap) != new_core_mask ||
	    !(new_core_mask & kbdev->gpu_props.props.coherency_info.group[0].core_mask)) {
		dev_err(dev, "power_policy: invalid core specification\n");
		return -EINVAL;
	}

	if (kbdev->pm.debug_core_mask != new_core_mask) {
		unsigned long flags;

		spin_lock_irqsave(&kbdev->pm.power_change_lock, flags);

		kbdev->pm.debug_core_mask = new_core_mask;
		kbase_pm_update_cores_state_nolock(kbdev);

		spin_unlock_irqrestore(&kbdev->pm.power_change_lock, flags);
	}

	return count;
}

/** The sysfs file @c core_mask.
 *
 * This is used to restrict shader core availability for debugging purposes.
 * Reading it will show the current core mask and the mask of cores available.
 * Writing to it will set the current core mask.
 */
DEVICE_ATTR(core_mask, S_IRUGO | S_IWUSR, show_core_mask, set_core_mask);


#ifdef CONFIG_MALI_DEBUG_SHADER_SPLIT_FS
/* Import the external affinity mask variables */
extern u64 mali_js0_affinity_mask;
extern u64 mali_js1_affinity_mask;
extern u64 mali_js2_affinity_mask;

/**
 * Structure containing a single shader affinity split configuration.
 */
typedef struct {
	char const * tag;
	char const * human_readable;
	u64          js0_mask;
	u64          js1_mask;
	u64          js2_mask;
} sc_split_config;

/**
 * Array of available shader affinity split configurations.
 */
static sc_split_config const sc_split_configs[] =
{
	/* All must be the first config (default). */
	{
		"all", "All cores",
		0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL
	},
	{
		"mp1", "MP1 shader core",
		0x1, 0x1, 0x1
	},
	{
		"mp2", "MP2 shader core",
		0x3, 0x3, 0x3
	},
	{
		"mp4", "MP4 shader core",
		0xF, 0xF, 0xF
	},
	{
		"mp1_vf", "MP1 vertex + MP1 fragment shader core",
		0x2, 0x1, 0xFFFFFFFFFFFFFFFFULL
	},
	{
		"mp2_vf", "MP2 vertex + MP2 fragment shader core",
		0xA, 0x5, 0xFFFFFFFFFFFFFFFFULL
	},
	/* This must be the last config. */
	{
		NULL, NULL,
		0x0, 0x0, 0x0
	},
};

/* Pointer to the currently active shader split configuration. */
static sc_split_config const * current_sc_split_config = &sc_split_configs[0];

/** Show callback for the @c sc_split sysfs file
 *
 * Returns the current shader core affinity policy.
 */
static ssize_t show_split(struct device *dev, struct device_attribute *attr, char * const buf)
{
	ssize_t ret;
	/* We know we are given a buffer which is PAGE_SIZE long. Our strings are all guaranteed
	 * to be shorter than that at this time so no length check needed. */
	ret = scnprintf(buf, PAGE_SIZE, "Current sc_split: '%s'\n", current_sc_split_config->tag );
	return ret;
}

/** Store callback for the @c sc_split sysfs file.
 *
 * This function is called when the @c sc_split sysfs file is written to
 * It modifies the system shader core affinity configuration to allow
 * system profiling with different hardware configurations.
 *
 * @param dev	The device with sysfs file is for
 * @param attr	The attributes of the sysfs file
 * @param buf	The value written to the sysfs file
 * @param count	The number of bytes written to the sysfs file
 *
 * @return @c count if the function succeeded. An error code on failure.
 */
static ssize_t set_split(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	sc_split_config const * config = &sc_split_configs[0];

	/* Try to match: loop until we hit the last "NULL" entry */
	while( config->tag )
	{
		if (sysfs_streq(config->tag, buf))
		{
			current_sc_split_config = config;
			mali_js0_affinity_mask  = config->js0_mask;
			mali_js1_affinity_mask  = config->js1_mask;
			mali_js2_affinity_mask  = config->js2_mask;
			dev_info(dev, "Setting sc_split: '%s'\n", config->tag);
			return count;
		}
		config++;
	}

	/* No match found in config list */
	dev_err(dev, "sc_split: invalid value\n");
	dev_err(dev, "  Possible settings: mp[1|2|4], mp[1|2]_vf\n");
	return -ENOENT;
}

/** The sysfs file @c sc_split
 *
 * This is used for configuring/querying the current shader core work affinity
 * configuration.
 */
DEVICE_ATTR(sc_split, S_IRUGO|S_IWUSR, show_split, set_split);
#endif /* CONFIG_MALI_DEBUG_SHADER_SPLIT_FS */


#if MALI_CUSTOMER_RELEASE == 0
/** Store callback for the @c js_timeouts sysfs file.
 *
 * This function is called to get the contents of the @c js_timeouts sysfs
 * file. This file contains five values separated by whitespace. The values
 * are basically the same as KBASE_CONFIG_ATTR_JS_SOFT_STOP_TICKS,
 * KBASE_CONFIG_ATTR_JS_HARD_STOP_TICKS_SS, KBASE_CONFIG_ATTR_JS_HARD_STOP_TICKS_NSS,
 * KBASE_CONFIG_ATTR_JS_RESET_TICKS_SS, BASE_CONFIG_ATTR_JS_RESET_TICKS_NSS
 * configuration values (in that order), with the difference that the js_timeout
 * valus are expressed in MILLISECONDS.
 *
 * The js_timeouts sysfile file allows the current values in
 * use by the job scheduler to get override. Note that a value needs to
 * be other than 0 for it to override the current job scheduler value.
 *
 * @param dev	The device with sysfs file is for
 * @param attr	The attributes of the sysfs file
 * @param buf	The value written to the sysfs file
 * @param count	The number of bytes written to the sysfs file
 *
 * @return @c count if the function succeeded. An error code on failure.
 */
static ssize_t set_js_timeouts(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct kbase_device *kbdev;
	int items;
	unsigned long js_soft_stop_ms;
	unsigned long js_soft_stop_ms_cl;
	unsigned long js_hard_stop_ms_ss;
	unsigned long js_hard_stop_ms_cl;
	unsigned long js_hard_stop_ms_nss;
	unsigned long js_reset_ms_ss;
	unsigned long js_reset_ms_cl;
	unsigned long js_reset_ms_nss;

	kbdev = to_kbase_device(dev);
	if (!kbdev)
		return -ENODEV;

	items = sscanf(buf, "%lu %lu %lu %lu %lu %lu %lu %lu", &js_soft_stop_ms, &js_soft_stop_ms_cl, &js_hard_stop_ms_ss, &js_hard_stop_ms_cl, &js_hard_stop_ms_nss, &js_reset_ms_ss, &js_reset_ms_cl, &js_reset_ms_nss);
	if (items == 8) {
		u64 ticks;

		ticks = js_soft_stop_ms * 1000000ULL;
		do_div(ticks, kbdev->js_data.scheduling_tick_ns);
		kbdev->js_soft_stop_ticks = ticks;

		ticks = js_soft_stop_ms_cl * 1000000ULL;
		do_div(ticks, kbdev->js_data.scheduling_tick_ns);
		kbdev->js_soft_stop_ticks_cl = ticks;
		ticks = js_hard_stop_ms_ss * 1000000ULL;
		do_div(ticks, kbdev->js_data.scheduling_tick_ns);
		kbdev->js_hard_stop_ticks_ss = ticks;

		ticks = js_hard_stop_ms_cl * 1000000ULL;
		do_div(ticks, kbdev->js_data.scheduling_tick_ns);
		kbdev->js_hard_stop_ticks_cl = ticks;
		ticks = js_hard_stop_ms_nss * 1000000ULL;
		do_div(ticks, kbdev->js_data.scheduling_tick_ns);
		kbdev->js_hard_stop_ticks_nss = ticks;

		ticks = js_reset_ms_ss * 1000000ULL;
		do_div(ticks, kbdev->js_data.scheduling_tick_ns);
		kbdev->js_reset_ticks_ss = ticks;

		ticks = js_reset_ms_cl * 1000000ULL;
		do_div(ticks, kbdev->js_data.scheduling_tick_ns);
		kbdev->js_reset_ticks_cl = ticks;
		ticks = js_reset_ms_nss * 1000000ULL;
		do_div(ticks, kbdev->js_data.scheduling_tick_ns);
		kbdev->js_reset_ticks_nss = ticks;

		dev_info(kbdev->osdev.dev, "Overriding KBASE_CONFIG_ATTR_JS_SOFT_STOP_TICKS with %lu ticks (%lu ms)\n", (unsigned long)kbdev->js_soft_stop_ticks, js_soft_stop_ms);
		dev_info(kbdev->osdev.dev, "Overriding KBASE_CONFIG_ATTR_JS_SOFT_STOP_TICKS_CL with %lu ticks (%lu ms)\n", (unsigned long)kbdev->js_soft_stop_ticks_cl, js_soft_stop_ms_cl);
		dev_info(kbdev->osdev.dev, "Overriding KBASE_CONFIG_ATTR_JS_HARD_STOP_TICKS_SS with %lu ticks (%lu ms)\n", (unsigned long)kbdev->js_hard_stop_ticks_ss, js_hard_stop_ms_ss);
		dev_info(kbdev->osdev.dev, "Overriding KBASE_CONFIG_ATTR_JS_HARD_STOP_TICKS_CL with %lu ticks (%lu ms)\n", (unsigned long)kbdev->js_hard_stop_ticks_cl, js_hard_stop_ms_cl);
		dev_info(kbdev->osdev.dev, "Overriding KBASE_CONFIG_ATTR_JS_HARD_STOP_TICKS_NSS with %lu ticks (%lu ms)\n", (unsigned long)kbdev->js_hard_stop_ticks_nss, js_hard_stop_ms_nss);
		dev_info(kbdev->osdev.dev, "Overriding KBASE_CONFIG_ATTR_JS_RESET_TICKS_SS with %lu ticks (%lu ms)\n", (unsigned long)kbdev->js_reset_ticks_ss, js_reset_ms_ss);
		dev_info(kbdev->osdev.dev, "Overriding KBASE_CONFIG_ATTR_JS_RESET_TICKS_CL with %lu ticks (%lu ms)\n", (unsigned long)kbdev->js_reset_ticks_cl, js_reset_ms_cl);
		dev_info(kbdev->osdev.dev, "Overriding KBASE_CONFIG_ATTR_JS_RESET_TICKS_NSS with %lu ticks (%lu ms)\n", (unsigned long)kbdev->js_reset_ticks_nss, js_reset_ms_nss);

		return count;
	} else {
		dev_err(kbdev->osdev.dev, "Couldn't process js_timeouts write operation.\nUse format " "<soft_stop_ms> <hard_stop_ms_ss> <hard_stop_ms_nss> <reset_ms_ss> <reset_ms_nss>\n");
		return -EINVAL;
	}
}

/** Show callback for the @c js_timeouts sysfs file.
 *
 * This function is called to get the contents of the @c js_timeouts sysfs
 * file. It returns the last set values written to the js_timeouts sysfs file.
 * If the file didn't get written yet, the values will be 0.
 *
 * @param dev	The device this sysfs file is for
 * @param attr	The attributes of the sysfs file
 * @param buf	The output buffer for the sysfs file contents
 *
 * @return The number of bytes output to @c buf.
 */
static ssize_t show_js_timeouts(struct device *dev, struct device_attribute *attr, char * const buf)
{
	struct kbase_device *kbdev;
	ssize_t ret;
	u64 ms;
	unsigned long js_soft_stop_ms;
	unsigned long js_soft_stop_ms_cl;
	unsigned long js_hard_stop_ms_ss;
	unsigned long js_hard_stop_ms_cl;
	unsigned long js_hard_stop_ms_nss;
	unsigned long js_reset_ms_ss;
	unsigned long js_reset_ms_cl;
	unsigned long js_reset_ms_nss;

	kbdev = to_kbase_device(dev);
	if (!kbdev)
		return -ENODEV;

	ms = (u64) kbdev->js_soft_stop_ticks * kbdev->js_data.scheduling_tick_ns;
	do_div(ms, 1000000UL);
	js_soft_stop_ms = (unsigned long)ms;

	ms = (u64) kbdev->js_soft_stop_ticks_cl * kbdev->js_data.scheduling_tick_ns;
	do_div(ms, 1000000UL);
	js_soft_stop_ms_cl = (unsigned long)ms;
	ms = (u64) kbdev->js_hard_stop_ticks_ss * kbdev->js_data.scheduling_tick_ns;
	do_div(ms, 1000000UL);
	js_hard_stop_ms_ss = (unsigned long)ms;

	ms = (u64) kbdev->js_hard_stop_ticks_cl * kbdev->js_data.scheduling_tick_ns;
	do_div(ms, 1000000UL);
	js_hard_stop_ms_cl = (unsigned long)ms;
	ms = (u64) kbdev->js_hard_stop_ticks_nss * kbdev->js_data.scheduling_tick_ns;
	do_div(ms, 1000000UL);
	js_hard_stop_ms_nss = (unsigned long)ms;

	ms = (u64) kbdev->js_reset_ticks_ss * kbdev->js_data.scheduling_tick_ns;
	do_div(ms, 1000000UL);
	js_reset_ms_ss = (unsigned long)ms;

	ms = (u64) kbdev->js_reset_ticks_cl * kbdev->js_data.scheduling_tick_ns;
	do_div(ms, 1000000UL);
	js_reset_ms_cl = (unsigned long)ms;
	ms = (u64) kbdev->js_reset_ticks_nss * kbdev->js_data.scheduling_tick_ns;
	do_div(ms, 1000000UL);
	js_reset_ms_nss = (unsigned long)ms;

	ret = scnprintf(buf, PAGE_SIZE, "%lu %lu %lu %lu %lu %lu %lu %lu\n", js_soft_stop_ms, js_soft_stop_ms_cl, js_hard_stop_ms_ss, js_hard_stop_ms_cl, js_hard_stop_ms_nss, js_reset_ms_ss, js_reset_ms_cl, js_reset_ms_nss);

	if (ret >= PAGE_SIZE) {
		buf[PAGE_SIZE - 2] = '\n';
		buf[PAGE_SIZE - 1] = '\0';
		ret = PAGE_SIZE - 1;
	}

	return ret;
}

/** The sysfs file @c js_timeouts.
 *
 * This is used to override the current job scheduler values for
 * KBASE_CONFIG_ATTR_JS_STOP_STOP_TICKS_SS
 * KBASE_CONFIG_ATTR_JS_STOP_STOP_TICKS_CL
 * KBASE_CONFIG_ATTR_JS_HARD_STOP_TICKS_SS
 * KBASE_CONFIG_ATTR_JS_HARD_STOP_TICKS_CL
 * KBASE_CONFIG_ATTR_JS_HARD_STOP_TICKS_NSS
 * KBASE_CONFIG_ATTR_JS_RESET_TICKS_SS
 * KBASE_CONFIG_ATTR_JS_RESET_TICKS_CL
 * KBASE_CONFIG_ATTR_JS_RESET_TICKS_NSS.
 */
DEVICE_ATTR(js_timeouts, S_IRUGO | S_IWUSR, show_js_timeouts, set_js_timeouts);
#endif /* MALI_CUSTOMER_RELEASE == 0 */

#ifdef CONFIG_MALI_DEBUG
static ssize_t set_js_softstop_always(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct kbase_device *kbdev;
	int items;
	int softstop_always;

	kbdev = to_kbase_device(dev);
	if (!kbdev)
		return -ENODEV;

	items = sscanf(buf, "%d", &softstop_always);
	if ((items == 1) && ((softstop_always == 0) || (softstop_always == 1))) {
		kbdev->js_data.softstop_always = (mali_bool) softstop_always;

		dev_info(kbdev->osdev.dev, "Support for softstop on a single context: %s\n", (kbdev->js_data.softstop_always == MALI_FALSE) ? "Disabled" : "Enabled");
		return count;
	} else {
		dev_err(kbdev->osdev.dev, "Couldn't process js_softstop_always write operation.\nUse format " "<soft_stop_always>\n");
		return -EINVAL;
	}
}

static ssize_t show_js_softstop_always(struct device *dev, struct device_attribute *attr, char * const buf)
{
	struct kbase_device *kbdev;
	ssize_t ret;

	kbdev = to_kbase_device(dev);
	if (!kbdev)
		return -ENODEV;

	ret = scnprintf(buf, PAGE_SIZE, "%d\n", kbdev->js_data.softstop_always);

	if (ret >= PAGE_SIZE) {
		buf[PAGE_SIZE - 2] = '\n';
		buf[PAGE_SIZE - 1] = '\0';
		ret = PAGE_SIZE - 1;
	}

	return ret;
}

/**
 * By default, soft-stops are disabled when only a single context is present. The ability to
 * enable soft-stop when only a single context is present can be used for debug and unit-testing purposes.
 * (see CL t6xx_stress_1 unit-test as an example whereby this feature is used.)
 */
DEVICE_ATTR(js_softstop_always, S_IRUGO | S_IWUSR, show_js_softstop_always, set_js_softstop_always);
#endif /* CONFIG_MALI_DEBUG */

#ifdef CONFIG_MALI_DEBUG
typedef void (kbasep_debug_command_func) (kbase_device *);

typedef enum {
	KBASEP_DEBUG_COMMAND_DUMPTRACE,

	/* This must be the last enum */
	KBASEP_DEBUG_COMMAND_COUNT
} kbasep_debug_command_code;

typedef struct kbasep_debug_command {
	char *str;
	kbasep_debug_command_func *func;
} kbasep_debug_command;

/** Debug commands supported by the driver */
static const kbasep_debug_command debug_commands[] = {
	{
	 .str = "dumptrace",
	 .func = &kbasep_trace_dump,
	 }
};

/** Show callback for the @c debug_command sysfs file.
 *
 * This function is called to get the contents of the @c debug_command sysfs
 * file. This is a list of the available debug commands, separated by newlines.
 *
 * @param dev	The device this sysfs file is for
 * @param attr	The attributes of the sysfs file
 * @param buf	The output buffer for the sysfs file contents
 *
 * @return The number of bytes output to @c buf.
 */
static ssize_t show_debug(struct device *dev, struct device_attribute *attr, char *const buf)
{
	struct kbase_device *kbdev;
	int i;
	ssize_t ret = 0;

	kbdev = to_kbase_device(dev);

	if (!kbdev)
		return -ENODEV;

	for (i = 0; i < KBASEP_DEBUG_COMMAND_COUNT && ret < PAGE_SIZE; i++)
		ret += scnprintf(buf + ret, PAGE_SIZE - ret, "%s\n", debug_commands[i].str);

	if (ret >= PAGE_SIZE) {
		buf[PAGE_SIZE - 2] = '\n';
		buf[PAGE_SIZE - 1] = '\0';
		ret = PAGE_SIZE - 1;
	}

	return ret;
}

/** Store callback for the @c debug_command sysfs file.
 *
 * This function is called when the @c debug_command sysfs file is written to.
 * It matches the requested command against the available commands, and if
 * a matching command is found calls the associated function from
 * @ref debug_commands to issue the command.
 *
 * @param dev	The device with sysfs file is for
 * @param attr	The attributes of the sysfs file
 * @param buf	The value written to the sysfs file
 * @param count	The number of bytes written to the sysfs file
 *
 * @return @c count if the function succeeded. An error code on failure.
 */
static ssize_t issue_debug(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct kbase_device *kbdev;
	int i;

	kbdev = to_kbase_device(dev);

	if (!kbdev)
		return -ENODEV;

	for (i = 0; i < KBASEP_DEBUG_COMMAND_COUNT; i++) {
		if (sysfs_streq(debug_commands[i].str, buf)) {
			debug_commands[i].func(kbdev);
			return count;
		}
	}

	/* Debug Command not found */
	dev_err(dev, "debug_command: command not known\n");
	return -EINVAL;
}

/** The sysfs file @c debug_command.
 *
 * This is used to issue general debug commands to the device driver.
 * Reading it will produce a list of debug commands, separated by newlines.
 * Writing to it with one of those commands will issue said command.
 */
DEVICE_ATTR(debug_command, S_IRUGO | S_IWUSR, show_debug, issue_debug);
#endif /* CONFIG_MALI_DEBUG */

#ifdef CONFIG_MALI_NO_MALI
static int kbase_common_reg_map(kbase_device *kbdev)
{
	return 0;
}
static void kbase_common_reg_unmap(kbase_device * const kbdev)
{
	return;
}
#else /* CONFIG_MALI_NO_MALI */
static int kbase_common_reg_map(kbase_device *kbdev)
{
	struct kbase_os_device *osdev = &kbdev->osdev;
	int err = -ENOMEM;

	osdev->reg_res = request_mem_region(osdev->reg_start, osdev->reg_size, dev_name(osdev->dev));
	if (!osdev->reg_res) {
		dev_err(osdev->dev, "Register window unavailable\n");
		err = -EIO;
		goto out_region;
	}

	osdev->reg = ioremap(osdev->reg_start, osdev->reg_size);
	if (!osdev->reg) {
		dev_err(osdev->dev, "Can't remap register window\n");
		err = -EINVAL;
		goto out_ioremap;
	}

	return 0;

 out_ioremap:
	release_resource(osdev->reg_res);
	kfree(osdev->reg_res);
 out_region:
	return err;
}

static void kbase_common_reg_unmap(kbase_device * const kbdev)
{
	struct kbase_os_device *osdev = &kbdev->osdev;

	iounmap(osdev->reg);
	release_resource(osdev->reg_res);
	kfree(osdev->reg_res);
}
#endif /* CONFIG_MALI_NO_MALI */


static int kbase_common_device_init(kbase_device *kbdev)
{
	struct kbase_os_device *osdev = &kbdev->osdev;
	int err = -ENOMEM;
	mali_error mali_err;
	enum {
		inited_mem = (1u << 0),
		inited_job_slot = (1u << 1),
		inited_pm = (1u << 2),
		inited_js = (1u << 3),
		inited_irqs = (1u << 4),
		inited_debug = (1u << 5),
		inited_js_softstop = (1u << 6),
#if MALI_CUSTOMER_RELEASE == 0
		inited_js_timeouts = (1u << 7),
#endif /* MALI_CUSTOMER_RELEASE == 0 */
		inited_pm_runtime_init = (1u << 8),
#ifdef CONFIG_DEBUG_FS
		inited_gpu_memory = (1u << 9),
#endif /* CONFIG_DEBUG_FS */
#ifdef CONFIG_MALI_DEBUG_SHADER_SPLIT_FS
		inited_sc_split = (1u << 11),
#endif /* CONFIG_MALI_DEBUG_SHADER_SPLIT_FS */
#ifdef CONFIG_MALI_TRACE_TIMELINE
		inited_timeline = (1u << 12),
#endif /* CONFIG_MALI_TRACE_LINE */
	};

	int inited = 0;

	dev_set_drvdata(osdev->dev, kbdev);

	osdev->mdev.minor = MISC_DYNAMIC_MINOR;
	osdev->mdev.name = osdev->devname;
	osdev->mdev.fops = &kbase_fops;
	osdev->mdev.parent = get_device(osdev->dev);

	scnprintf(osdev->devname, DEVNAME_SIZE, "%s%d", kbase_drv_name, kbase_dev_nr++);

	if (misc_register(&osdev->mdev)) {
		dev_err(osdev->dev, "Couldn't register misc dev %s\n", osdev->devname);
		err = -EINVAL;
		goto out_misc;
	}

	if (device_create_file(osdev->dev, &dev_attr_power_policy)) {
		dev_err(osdev->dev, "Couldn't create power_policy sysfs file\n");
		goto out_file;
	}

	if (device_create_file(osdev->dev, &dev_attr_core_availability_policy)) {
		dev_err(osdev->dev, "Couldn't create core_availability_policy sysfs file\n");
		goto out_file_core_availability_policy;
	}

	if (device_create_file(osdev->dev, &dev_attr_core_mask)) {
		dev_err(osdev->dev, "Couldn't create core_mask sysfs file\n");
		goto out_file_core_mask;
	}

	down(&kbase_dev_list_lock);
	list_add(&osdev->entry, &kbase_dev_list);
	up(&kbase_dev_list_lock);
	dev_info(osdev->dev, "Probed as %s\n", dev_name(osdev->mdev.this_device));

	mali_err = kbase_pm_init(kbdev);
	if (MALI_ERROR_NONE != mali_err)
		goto out_partial;

	inited |= inited_pm;

	if (kbdev->pm.callback_power_runtime_init) {
		mali_err = kbdev->pm.callback_power_runtime_init(kbdev);
		if (MALI_ERROR_NONE != mali_err)
			goto out_partial;

		inited |= inited_pm_runtime_init;
	}

	mali_err = kbase_mem_init(kbdev);
	if (MALI_ERROR_NONE != mali_err)
		goto out_partial;

	inited |= inited_mem;

	mali_err = kbase_job_slot_init(kbdev);
	if (MALI_ERROR_NONE != mali_err)
		goto out_partial;

	inited |= inited_job_slot;

	mali_err = kbasep_js_devdata_init(kbdev);
	if (MALI_ERROR_NONE != mali_err)
		goto out_partial;

	inited |= inited_js;

	err = kbase_install_interrupts(kbdev);
	if (err)
		goto out_partial;

	inited |= inited_irqs;

#ifdef CONFIG_MALI_DEBUG_SHADER_SPLIT_FS
	if (device_create_file(osdev->dev, &dev_attr_sc_split))
	{
		dev_err(osdev->dev, "Couldn't create sc_split sysfs file\n");
		goto out_partial;
	}

	inited |= inited_sc_split;
#endif /* CONFIG_MALI_DEBUG_SHADER_SPLIT_FS */

#ifdef CONFIG_DEBUG_FS
	if (kbasep_gpu_memory_debugfs_init(kbdev)) {
		dev_err(osdev->dev, "Couldn't create gpu_memory debugfs file\n");
		goto out_partial;
	}
	inited |= inited_gpu_memory;
#endif /* CONFIG_DEBUG_FS */

#ifdef CONFIG_MALI_DEBUG

	if (device_create_file(osdev->dev, &dev_attr_debug_command)) {
		dev_err(osdev->dev, "Couldn't create debug_command sysfs file\n");
		goto out_partial;
	}
	inited |= inited_debug;

	if (device_create_file(osdev->dev, &dev_attr_js_softstop_always)) {
		dev_err(osdev->dev, "Couldn't create js_softstop_always sysfs file\n");
		goto out_partial;
	}
	inited |= inited_js_softstop;
#endif /* CONFIG_MALI_DEBUG */

#if MALI_CUSTOMER_RELEASE == 0
	if (device_create_file(osdev->dev, &dev_attr_js_timeouts)) {
		dev_err(osdev->dev, "Couldn't create js_timeouts sysfs file\n");
		goto out_partial;
	}
	inited |= inited_js_timeouts;
#endif /* MALI_CUSTOMER_RELEASE */

#ifdef CONFIG_MALI_TRACE_TIMELINE
	if (kbasep_trace_timeline_debugfs_init(kbdev)) {
		dev_err(osdev->dev, "Couldn't create mali_timeline_defs debugfs file\n");
		goto out_partial;
	}
	inited |= inited_timeline;
#endif /* CONFIG_MALI_TRACE_TIMELINE */

	mali_err = kbase_pm_powerup(kbdev);
	if (MALI_ERROR_NONE == mali_err) {
#ifdef CONFIG_MALI_DEBUG
#ifndef CONFIG_MALI_NO_MALI
		if (MALI_ERROR_NONE != kbasep_common_test_interrupt_handlers(kbdev)) {
			dev_err(osdev->dev, "Interrupt assigment check failed.\n");
			err = -EINVAL;
			goto out_partial;
		}
#endif /* CONFIG_MALI_NO_MALI */
#endif /* CONFIG_MALI_DEBUG */

		/* intialise the kctx list */
		mutex_init(&kbdev->kctx_list_lock);
		INIT_LIST_HEAD(&kbdev->kctx_list);
		return 0;
	}

 out_partial:
#ifdef CONFIG_MALI_TRACE_TIMELINE
	if (inited & inited_timeline)
		kbasep_trace_timeline_debugfs_term(kbdev);
#endif /* CONFIG_MALI_TRACE_TIMELINE */
#if MALI_CUSTOMER_RELEASE == 0
	if (inited & inited_js_timeouts)
		device_remove_file(kbdev->osdev.dev, &dev_attr_js_timeouts);
#endif /* MALI_CUSTOMER_RELEASE */
#ifdef CONFIG_MALI_DEBUG
	if (inited & inited_js_softstop)
		device_remove_file(kbdev->osdev.dev, &dev_attr_js_softstop_always);

	if (inited & inited_debug)
		device_remove_file(kbdev->osdev.dev, &dev_attr_debug_command);

#endif /* CONFIG_MALI_DEBUG */

#ifdef CONFIG_DEBUG_FS
	if (inited & inited_gpu_memory)
		kbasep_gpu_memory_debugfs_term(kbdev);
#endif /* CONFIG_DEBUG_FS */

#ifdef CONFIG_MALI_DEBUG_SHADER_SPLIT_FS
	if (inited & inited_sc_split)
	{
		device_remove_file(kbdev->osdev.dev, &dev_attr_sc_split);
	}
#endif /* CONFIG_MALI_DEBUG_SHADER_SPLIT_FS */

	if (inited & inited_js)
		kbasep_js_devdata_halt(kbdev);

	if (inited & inited_job_slot)
		kbase_job_slot_halt(kbdev);

	if (inited & inited_mem)
		kbase_mem_halt(kbdev);

	if (inited & inited_pm)
		kbase_pm_halt(kbdev);

	if (inited & inited_irqs)
		kbase_release_interrupts(kbdev);

	if (inited & inited_js)
		kbasep_js_devdata_term(kbdev);

	if (inited & inited_job_slot)
		kbase_job_slot_term(kbdev);

	if (inited & inited_mem)
		kbase_mem_term(kbdev);

	if (inited & inited_pm_runtime_init) {
		if (kbdev->pm.callback_power_runtime_term)
			kbdev->pm.callback_power_runtime_term(kbdev);
	}

	if (inited & inited_pm)
		kbase_pm_term(kbdev);

	down(&kbase_dev_list_lock);
	list_del(&osdev->entry);
	up(&kbase_dev_list_lock);

	device_remove_file(kbdev->osdev.dev, &dev_attr_core_mask);
 out_file_core_mask:
	device_remove_file(kbdev->osdev.dev, &dev_attr_core_availability_policy);
 out_file_core_availability_policy:
	device_remove_file(kbdev->osdev.dev, &dev_attr_power_policy);
 out_file:
	misc_deregister(&kbdev->osdev.mdev);
 out_misc:
	put_device(osdev->dev);
	return err;
}

#ifdef CONFIG_OF
static const struct of_device_id exynos_mali_match[] = {
	{
		.compatible = "arm,mali",
	},
	{},
};

MODULE_DEVICE_TABLE(of, exynos_mali_match);
#endif

static int kbase_platform_device_probe(struct platform_device *pdev)
{
	struct kbase_device *kbdev;
	struct kbase_os_device *osdev;
	struct resource *reg_res;
	kbase_attribute *platform_data;
	int err;
	int i;
	struct mali_base_gpu_core_props *core_props;
#ifdef CONFIG_MALI_NO_MALI
	mali_error mali_err;
#endif /* CONFIG_MALI_NO_MALI */
#ifdef CONFIG_OF

#ifdef CONFIG_MALI_PLATFORM_FAKE
	kbase_platform_config *config;
	int attribute_count;
	config = kbase_get_platform_config();
	attribute_count = kbasep_get_config_attribute_count(config->attributes);

	err = platform_device_add_data(pdev, config->attributes,
			attribute_count * sizeof(config->attributes[0]));
	if (err)
		return err;
#else
	/* Call any hooks required for platform initialization at this stage */
	err = kbase_platform_early_init(pdev);
	if (err) {
		return err;
	}
#endif /* CONFIG_MALI_PLATFORM_FAKE */
#endif /* CONFIG_OF */

	kbdev = kbase_device_alloc();
	if (!kbdev) {
		dev_err(&pdev->dev, "Can't allocate device\n");
		err = -ENOMEM;
		goto out;
	}
#ifdef CONFIG_MALI_NO_MALI
	mali_err = midg_device_create(kbdev);
	if (MALI_ERROR_NONE != mali_err) {
		dev_err(&pdev->dev, "Can't initialize dummy model\n");
		err = -ENOMEM;
		goto out_midg;
	}
#endif /* CONFIG_MALI_NO_MALI */

	osdev = &kbdev->osdev;
	osdev->dev = &pdev->dev;
	platform_data = (kbase_attribute *) osdev->dev->platform_data;

	if (NULL == platform_data) {
		dev_err(osdev->dev, "Platform data not specified\n");
		err = -ENOENT;
		goto out_free_dev;
	}

	if (MALI_TRUE != kbasep_validate_configuration_attributes(kbdev, platform_data)) {
		dev_err(osdev->dev, "Configuration attributes failed to validate\n");
		err = -EINVAL;
		goto out_free_dev;
	}
	kbdev->config_attributes = platform_data;

	/* 3 IRQ resources */
	for (i = 0; i < 3; i++) {
		struct resource *irq_res;
		int irqtag;

		irq_res = platform_get_resource(pdev, IORESOURCE_IRQ, i);
		if (!irq_res) {
			dev_err(osdev->dev, "No IRQ resource at index %d\n", i);
			err = -ENOENT;
			goto out_free_dev;
		}

#if 0 /*SS*/ /*#ifdef CONFIG_OF*/
		if (!strcmp(irq_res->name, "JOB"))
			irqtag = JOB_IRQ_TAG;
		else if (!strcmp(irq_res->name, "MMU"))
			irqtag = MMU_IRQ_TAG;
		else if (!strcmp(irq_res->name, "GPU"))
			irqtag = GPU_IRQ_TAG;
		else {
			dev_err(&pdev->dev, "Invalid irq res name: '%s'\n",
				irq_res->name);
			err = -EINVAL;
			goto out_free_dev;
		}
#else
		irqtag = i;
#endif /* CONFIG_OF */
		osdev->irqs[irqtag].irq = irq_res->start;
		osdev->irqs[irqtag].flags = (irq_res->flags & IRQF_TRIGGER_MASK);
	}

	/* the first memory resource is the physical address of the GPU registers */
	reg_res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!reg_res) {
		dev_err(&pdev->dev, "Invalid register resource\n");
		err = -ENOENT;
		goto out_free_dev;
	}

	osdev->reg_start = reg_res->start;
	osdev->reg_size = resource_size(reg_res);

	err = kbase_common_reg_map(kbdev);
	if (err)
		goto out_free_dev;

#ifdef CONFIG_DEBUG_FS
	kbdev->mali_debugfs_directory = debugfs_create_dir("mali", NULL);
	if (NULL == kbdev->mali_debugfs_directory) {
		dev_err(osdev->dev, "Couldn't create mali debugfs directory\n");
		goto out_reg_unmap;
	}
#endif /* CONFIG_DEBUG_FS */
	if (MALI_ERROR_NONE != kbase_device_init(kbdev)) {
		dev_err(&pdev->dev, "Can't initialize device\n");
		err = -ENOMEM;
		goto out_debugfs_remove;
	}

	/* obtain min/max configured gpu frequencies */
	core_props = &(kbdev->gpu_props.props.core_props);
	core_props->gpu_freq_khz_min = kbasep_get_config_value(kbdev, platform_data, KBASE_CONFIG_ATTR_GPU_FREQ_KHZ_MIN);
	core_props->gpu_freq_khz_max = kbasep_get_config_value(kbdev, platform_data, KBASE_CONFIG_ATTR_GPU_FREQ_KHZ_MAX);
	kbdev->gpu_props.irq_throttle_time_us = kbasep_get_config_value(kbdev, platform_data, KBASE_CONFIG_ATTR_GPU_IRQ_THROTTLE_TIME_US);

	wake_lock_init(&kbdev->pm.kbase_wake_lock, WAKE_LOCK_SUSPEND, "kbase_wake_lock");

	err = kbase_common_device_init(kbdev);
	if (err) {
		dev_err(osdev->dev, "Failed kbase_common_device_init\n");
		goto out_term_dev;
	}
	return 0;

out_term_dev:
	kbase_device_term(kbdev);
out_debugfs_remove:
#ifdef CONFIG_DEBUG_FS
	debugfs_remove(kbdev->mali_debugfs_directory);
out_reg_unmap:
#endif /* CONFIG_DEBUG_FS */
	kbase_common_reg_unmap(kbdev);
out_free_dev:
#ifdef CONFIG_MALI_NO_MALI
	midg_device_destroy(kbdev);
out_midg:
#endif /* CONFIG_MALI_NO_MALI */
	kbase_device_free(kbdev);
out:
	return err;
}

static int kbase_common_device_remove(struct kbase_device *kbdev)
{
	if (kbdev->pm.callback_power_runtime_term)
		kbdev->pm.callback_power_runtime_term(kbdev);

	wake_lock_destroy(&kbdev->pm.kbase_wake_lock);

	/* Remove the sys power policy file */
	device_remove_file(kbdev->osdev.dev, &dev_attr_power_policy);
	device_remove_file(kbdev->osdev.dev, &dev_attr_core_availability_policy);
	device_remove_file(kbdev->osdev.dev, &dev_attr_core_mask);

#ifdef CONFIG_MALI_TRACE_TIMELINE
	kbasep_trace_timeline_debugfs_term(kbdev);
#endif /* CONFIG_MALI_TRACE_TIMELINE */

#ifdef CONFIG_MALI_DEBUG
	device_remove_file(kbdev->osdev.dev, &dev_attr_js_softstop_always);
	device_remove_file(kbdev->osdev.dev, &dev_attr_debug_command);
#endif /* CONFIG_MALI_DEBUG */
#if MALI_CUSTOMER_RELEASE == 0
	device_remove_file(kbdev->osdev.dev, &dev_attr_js_timeouts);
#endif /* MALI_CUSTOMER_RELEASE */
#ifdef CONFIG_DEBUG_FS
	kbasep_gpu_memory_debugfs_term(kbdev);
#endif

#ifdef CONFIG_MALI_DEBUG_SHADER_SPLIT_FS
	device_remove_file(kbdev->osdev.dev, &dev_attr_sc_split);
#endif /* CONFIG_MALI_DEBUG_SHADER_SPLIT_FS */

	kbasep_js_devdata_halt(kbdev);
	kbase_job_slot_halt(kbdev);
	kbase_mem_halt(kbdev);
	kbase_pm_halt(kbdev);

	kbase_release_interrupts(kbdev);

	kbasep_js_devdata_term(kbdev);
	kbase_job_slot_term(kbdev);
	kbase_mem_term(kbdev);
	kbase_pm_term(kbdev);

	down(&kbase_dev_list_lock);
	list_del(&kbdev->osdev.entry);
	up(&kbase_dev_list_lock);

	misc_deregister(&kbdev->osdev.mdev);
	put_device(kbdev->osdev.dev);
	kbase_common_reg_unmap(kbdev);
	kbase_device_term(kbdev);
#ifdef CONFIG_DEBUG_FS
	debugfs_remove(kbdev->mali_debugfs_directory);
#endif /* CONFIG_DEBUG_FS */
#ifdef CONFIG_MALI_NO_MALI
	midg_device_destroy(kbdev);
#endif /* CONFIG_MALI_NO_MALI */
	kbase_device_free(kbdev);

	return 0;
}

static int kbase_platform_device_remove(struct platform_device *pdev)
{
	struct kbase_device *kbdev = to_kbase_device(&pdev->dev);

	if (!kbdev)
		return -ENODEV;

	return kbase_common_device_remove(kbdev);
}

/** Suspend callback from the OS.
 *
 * This is called by Linux when the device should suspend.
 *
 * @param dev  The device to suspend
 *
 * @return A standard Linux error code
 */
static int kbase_device_suspend(struct device *dev)
{
	struct kbase_device *kbdev = to_kbase_device(dev);

	if (!kbdev)
		return -ENODEV;

	kbase_pm_suspend(kbdev);
	return 0;
}

/** Resume callback from the OS.
 *
 * This is called by Linux when the device should resume from suspension.
 *
 * @param dev  The device to resume
 *
 * @return A standard Linux error code
 */
static int kbase_device_resume(struct device *dev)
{
	struct kbase_device *kbdev = to_kbase_device(dev);

	if (!kbdev)
		return -ENODEV;

	kbase_pm_resume(kbdev);
	return 0;
}

/** Runtime suspend callback from the OS.
 *
 * This is called by Linux when the device should prepare for a condition in which it will
 * not be able to communicate with the CPU(s) and RAM due to power management.
 *
 * @param dev  The device to suspend
 *
 * @return A standard Linux error code
 */
#ifdef CONFIG_PM_RUNTIME
static int kbase_device_runtime_suspend(struct device *dev)
{
	struct kbase_device *kbdev = to_kbase_device(dev);

	if (!kbdev)
		return -ENODEV;

	if (kbdev->pm.callback_power_runtime_off) {
		kbdev->pm.callback_power_runtime_off(kbdev);
		KBASE_DEBUG_PRINT_INFO(KBASE_PM, "runtime suspend\n");
	}
	return 0;
}
#endif /* CONFIG_PM_RUNTIME */

/** Runtime resume callback from the OS.
 *
 * This is called by Linux when the device should go into a fully active state.
 *
 * @param dev  The device to suspend
 *
 * @return A standard Linux error code
 */

#ifdef CONFIG_PM_RUNTIME
int kbase_device_runtime_resume(struct device *dev)
{
	int ret = 0;
	struct kbase_device *kbdev = to_kbase_device(dev);

	if (!kbdev)
		return -ENODEV;

	if (kbdev->pm.callback_power_runtime_on) {
		ret = kbdev->pm.callback_power_runtime_on(kbdev);
		KBASE_DEBUG_PRINT_INFO(KBASE_PM, "runtime resume\n");
	}
	return ret;
}
#endif /* CONFIG_PM_RUNTIME */

/** Runtime idle callback from the OS.
 *
 * This is called by Linux when the device appears to be inactive and it might be
 * placed into a low power state
 *
 * @param dev  The device to suspend
 *
 * @return A standard Linux error code
 */

#ifdef CONFIG_PM_RUNTIME
static int kbase_device_runtime_idle(struct device *dev)
{
	/* Avoid pm_runtime_suspend being called */
	return 1;
}
#endif /* CONFIG_PM_RUNTIME */

/** The power management operations for the platform driver.
 */
static const struct dev_pm_ops kbase_pm_ops = {
	.suspend = kbase_device_suspend,
	.resume = kbase_device_resume,
#ifdef CONFIG_PM_RUNTIME
	.runtime_suspend = kbase_device_runtime_suspend,
	.runtime_resume = kbase_device_runtime_resume,
	.runtime_idle = kbase_device_runtime_idle,
#endif /* CONFIG_PM_RUNTIME */
};

#ifdef CONFIG_OF
static const struct of_device_id kbase_dt_ids[] = {
	{ .compatible = "arm,malit6xx" },
	{ .compatible = "arm,mali-midgard" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, kbase_dt_ids);
#endif

static struct platform_driver kbase_platform_driver = {
	.probe = kbase_platform_device_probe,
	.remove = kbase_platform_device_remove,
	.driver = {
		   .name = kbase_drv_name,
		   .owner = THIS_MODULE,
		   .pm = &kbase_pm_ops,
		   .of_match_table = exynos_mali_match, /*of_match_ptr(kbase_dt_ids),*/
	},
};

/*
 * The driver will not provide a shortcut to create the Mali platform device
 * anymore when using Device Tree.
 */
#ifdef CONFIG_OF
module_platform_driver(kbase_platform_driver);
#else /* CONFIG_MALI_PLATFORM_FAKE */

#ifdef CONFIG_MALI_PLATFORM_FAKE
#ifndef MALI_PLATFORM_FAKE_MODULE
extern int kbase_platform_fake_register(void);
extern void kbase_platform_fake_unregister(void);
#endif
#endif

static int __init kbase_driver_init(void)
{
	int ret;
#ifdef 0 /*SS*/
	ret = kbase_platform_early_init();
	if (ret)
		return ret;
#endif

#ifdef CONFIG_MALI_PLATFORM_FAKE
#ifndef MALI_PLATFORM_FAKE_MODULE
	ret = kbase_platform_fake_register();
	if (ret)
		return ret;
#endif
#endif
	ret = platform_driver_register(&kbase_platform_driver);
#ifdef CONFIG_MALI_PLATFORM_FAKE
#ifndef MALI_PLATFORM_FAKE_MODULE
	if (ret)
		kbase_platform_fake_unregister();
#endif
#endif

	return ret;
}

static void __exit kbase_driver_exit(void)
{
	platform_driver_unregister(&kbase_platform_driver);
#ifdef CONFIG_MALI_PLATFORM_FAKE
#ifndef MALI_PLATFORM_FAKE_MODULE
	kbase_platform_fake_unregister();
#endif
#endif
}

module_init(kbase_driver_init);
module_exit(kbase_driver_exit);

#endif /* CONFIG_OF */

MODULE_LICENSE("GPL");
MODULE_VERSION(MALI_RELEASE_NAME);

#ifdef CONFIG_MALI_GATOR_SUPPORT
/* Create the trace points (otherwise we just get code to call a tracepoint) */
#define CREATE_TRACE_POINTS
#include "mali_linux_trace.h"

void kbase_trace_mali_pm_status(u32 event, u64 value)
{
	trace_mali_pm_status(event, value);
}

void kbase_trace_mali_pm_power_off(u32 event, u64 value)
{
	trace_mali_pm_power_off(event, value);
}

void kbase_trace_mali_pm_power_on(u32 event, u64 value)
{
	trace_mali_pm_power_on(event, value);
}

void kbase_trace_mali_job_slots_event(u32 event, const kbase_context *kctx, u8 atom_id)
{
	trace_mali_job_slots_event(event, (kctx != NULL ? kctx->osctx.tgid : 0), (kctx != NULL ? kctx->osctx.pid : 0), atom_id);
}

void kbase_trace_mali_page_fault_insert_pages(int event, u32 value)
{
	trace_mali_page_fault_insert_pages(event, value);
}

void kbase_trace_mali_mmu_as_in_use(int event)
{
	trace_mali_mmu_as_in_use(event);
}

void kbase_trace_mali_mmu_as_released(int event)
{
	trace_mali_mmu_as_released(event);
}

void kbase_trace_mali_total_alloc_pages_change(long long int event)
{
	trace_mali_total_alloc_pages_change(event);
}
#endif /* CONFIG_MALI_GATOR_SUPPORT */
