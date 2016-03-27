/*
 * base.c - Get the kernel base address.
 *
 * Copyright (c) 2014 Samuel Groß
 * Copyright (c) 2016 Siguza
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mach/mach_init.h>
#include <mach/mach_error.h>
#include <mach/mach_traps.h>
#include <mach/mach_types.h>
#include <mach/host_priv.h>
#include <mach/vm_map.h>
#include <mach-o/loader.h>

#include "libkern.h"

#if __LP64__
#define MACH_TYPE CPU_TYPE_ARM64
#define MACH_HEADER_MAGIC MH_MAGIC_64
typedef struct mach_header_64 mach_hdr_t;
#else
#define MACH_TYPE CPU_TYPE_ARM
#define MACH_HEADER_MAGIC MH_MAGIC
typedef struct mach_header mach_hdr_t;
#endif

kern_return_t get_kernel_task(task_t *task)
{
    static task_t kernel_task;
    static char initialized = 0;
    if(!initialized)
    {
        kern_return_t ret = task_for_pid(mach_task_self(), 0, &kernel_task);
        if(ret != KERN_SUCCESS)
        {
            // Try Pangu's special port
            ret = host_get_special_port(mach_host_self(), HOST_LOCAL_NODE, 4, &kernel_task);
            if(ret != KERN_SUCCESS)
            {
                return ret;
            }
        }
        initialized = 1;
    }
    *task = kernel_task;
    return KERN_SUCCESS;
}

vm_address_t get_kernel_base()
{
    static vm_address_t addr;
    static char initialized = 0;
    if(!initialized)
    {
        kern_return_t ret;
        task_t kernel_task;
        vm_region_submap_info_data_64_t info;
        vm_size_t size;
        mach_msg_type_number_t info_count = VM_REGION_SUBMAP_INFO_COUNT_64;
        unsigned int depth = 0;

        ret = get_kernel_task(&kernel_task);
        if(ret != KERN_SUCCESS)
            return 0;

        for(addr = 0; 1; addr += size)
        {
            // get next memory region
            ret = vm_region_recurse_64(kernel_task, &addr, &size, &depth, (vm_region_info_t)&info, &info_count);
            if(ret != KERN_SUCCESS)
                return 0;

            // the kernel maps over a GB of RAM at the address where it maps itself, and that region has rwx set to ---.
            // we can use those two facts to locate it.
            if(size > 1024*1024*1024 && (info.protection & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)) == 0)
            {
                // now we have to determine the mach header offset from the beginning of the region.
                // for iOS <= 8 this has been 0x1000 for 32bit and 0x2000 for 64bit.
                // starting with iOS 9, 64bit has shifted to 0x4000 and 32bit idk, but probably 0x2000.
                // so we just check both of those offsets for a possible mach header.
                mach_hdr_t hdr1, hdr2;

                ret = vm_read_overwrite(kernel_task, addr + IMAGE_OFFSET, sizeof(mach_hdr_t), (vm_address_t)&hdr1, &size);
                if(ret != KERN_SUCCESS)
                    return 0;

                ret = vm_read_overwrite(kernel_task, addr + 2 * IMAGE_OFFSET, sizeof(mach_hdr_t), (vm_address_t)&hdr2, &size);
                if(ret != KERN_SUCCESS)
                {
                    // if the second address cannot be read, the first one might still be valid
                    if(hdr1.magic == MACH_HEADER_MAGIC)
                    {
                        addr += IMAGE_OFFSET;
                    }
                    // or not
                    else
                    {
                        return 0;
                    }
                }
                else
                {
                    char b1, b2;
                    // we only have a problem if either both or none of the headers have the correct magic
                    b1 = hdr1.magic == MACH_HEADER_MAGIC;
                    b2 = hdr2.magic == MACH_HEADER_MAGIC;
                    if(b1 && b2)
                    {
                        // dig a little deeper
                        b1 = hdr1.cputype == MACH_TYPE && hdr1.filetype == MH_EXECUTE;
                        b2 = hdr2.cputype == MACH_TYPE && hdr2.filetype == MH_EXECUTE;
                        if(b1 && b2)
                        {
                            // go die in a fire
                            return 0;
                        }
                    }

                    if(b1)
                    {
                        addr += IMAGE_OFFSET;
                    }
                    else if(b2)
                    {
                        addr += 2 * IMAGE_OFFSET;
                    }
                    else // no magic match
                    {
                        return 0;
                    }
                }

                initialized = 1;
                break;
            }
        }
    }
    return addr;
}
