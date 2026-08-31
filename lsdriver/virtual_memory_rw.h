/*
 * 内存读写方式：有缓存跨进程读写。
 * 手动走页表把目标进程 VA 翻译成 PA，再经 phys_to_virt 得到内核线性地址后直接 memcpy。
 */

#ifndef VIRTUAL_MEMORY_RW_H
#define VIRTUAL_MEMORY_RW_H
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <asm/pgtable.h>
#include <asm/pgtable-prot.h>
#include <asm/memory.h>
#include <asm/barrier.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/pid.h>
#include <linux/sort.h>
#include "export_fun.h"
#include "io_struct.h"

// 页表软翻译，支持 PUD 1G / PMD 2M / PTE 4K 大页与普通页
static inline int walk_translate_va_to_pa(struct mm_struct *mm, uint64_t vaddr, phys_addr_t *paddr)
{
    if (!mm || !paddr) return -EINVAL;

    pgd_t *pgd = pgd_offset(mm, vaddr);
    if (pgd_none(*pgd) || pgd_bad(*pgd)) return -EFAULT;

    p4d_t *p4d = p4d_offset(pgd, vaddr);
    if (p4d_none(*p4d) || p4d_bad(*p4d)) return -EFAULT;

    pud_t *pud = pud_offset(p4d, vaddr);
    if (pud_none(*pud)) return -EFAULT;

    if (pud_leaf(*pud))
    {
        unsigned long pfn = pud_pfn(*pud);
        if (!pfn_valid(pfn)) return -EFAULT;

        *paddr = (pfn << PAGE_SHIFT) + (vaddr & ~PUD_MASK);
        return 0;
    }
    if (pud_bad(*pud)) return -EFAULT;

    pmd_t *pmd = pmd_offset(pud, vaddr);
    if (pmd_none(*pmd)) return -EFAULT;

    if (pmd_leaf(*pmd))
    {
        unsigned long pfn = pmd_pfn(*pmd);
        if (!pfn_valid(pfn)) return -EFAULT;

        *paddr = (pfn << PAGE_SHIFT) + (vaddr & ~PMD_MASK);
        return 0;
    }
    if (pmd_bad(*pmd)) return -EFAULT;

    pte_t *ptep = pte_offset_kernel(pmd, vaddr);
    if (!ptep) return -EFAULT;

    pte_t pte = *ptep;

    // 必须检查 pte_present，因为页可能被换出到 Swap 分区
    // 如果 present 为 false，pfn 字段是无效的（存的是 swap offset）
    if (pte_present(pte))
    {
        unsigned long pfn = pte_pfn(pte);
        if (!pfn_valid(pfn)) return -EFAULT;

        *paddr = (pfn << PAGE_SHIFT) + (vaddr & ~PAGE_MASK);
        return 0;
    }

    return -EFAULT;
}

static inline int linear_read_physical(phys_addr_t paddr, void *buffer, size_t size)
{
    void *kernel_vaddr = phys_to_virt(paddr);

    __builtin_memcpy(buffer, kernel_vaddr, size);

    return 0;
}

static inline int linear_write_physical(phys_addr_t paddr, const void *buffer, size_t size)
{
    void *kernel_vaddr = phys_to_virt(paddr);

    __builtin_memcpy(kernel_vaddr, buffer, size);

    return 0;
}

static inline int virtual_memory_rw(enum request_op op, pid_t pid, uint64_t vaddr, void *buffer, size_t size)
{
    static pid_t s_last_pid = 0;
    static struct mm_struct *s_last_mm = NULL;
    static uint64_t s_last_vpage_base = -1ULL;
    static phys_addr_t s_last_ppage_base = 0;

    phys_addr_t paddr_of_page = 0;
    uint64_t current_vaddr = untagged_addr(vaddr);
    size_t bytes_remaining = size;
    size_t bytes_copied = 0;
    size_t bytes_done = 0;
    int status = 0;

    if (!buffer || size == 0) return -EINVAL;

    if (pid != s_last_pid || s_last_mm == NULL)
    {
        s_last_mm = 0;
        s_last_mm = get_mm_by_pid(pid);
        if (s_last_mm)
        {
            mmput(s_last_mm);
        }
        else
        {
            return -EINVAL;
        }

        s_last_pid = pid;
        s_last_vpage_base = -1ULL;
    }

    while (bytes_remaining > 0)
    {
        size_t page_offset = current_vaddr & (PAGE_SIZE - 1);
        size_t bytes_this_page = PAGE_SIZE - page_offset;
        uint64_t current_vpn = current_vaddr & PAGE_MASK;

        if (bytes_this_page > bytes_remaining) bytes_this_page = bytes_remaining;

        if (current_vpn == s_last_vpage_base)
        {
            paddr_of_page = s_last_ppage_base;
        }
        else
        {
            uint64_t task_size = READ_ONCE(s_last_mm->task_size);
            if (current_vaddr >= task_size || bytes_this_page > task_size - current_vaddr)
            {
                status = -EFAULT;
                s_last_vpage_base = -1ULL;
                if (op == request_op_vmem_read && size > 8) __builtin_memset((uint8_t *)buffer + bytes_copied, 0, bytes_this_page);
                goto next_chunk;
            }

            status = walk_translate_va_to_pa(s_last_mm, current_vpn, &paddr_of_page);

            if (status != 0)
            {
                s_last_vpage_base = -1ULL;
                if (op == request_op_vmem_read && size > 8) __builtin_memset((uint8_t *)buffer + bytes_copied, 0, bytes_this_page);
                goto next_chunk;
            }
            s_last_vpage_base = current_vpn;
            s_last_ppage_base = paddr_of_page;
        }

        if (op == request_op_vmem_read)
        {
            status = linear_read_physical(paddr_of_page + page_offset, (uint8_t *)buffer + bytes_copied, bytes_this_page);
        }
        else
        {
            status = linear_write_physical(paddr_of_page + page_offset, (const uint8_t *)buffer + bytes_copied, bytes_this_page);
        }

        if (status != 0)
        {
            s_last_vpage_base = -1ULL;
            if (op == request_op_vmem_read && size > 8) __builtin_memset((uint8_t *)buffer + bytes_copied, 0, bytes_this_page);
            goto next_chunk;
        }

        bytes_done += bytes_this_page;

    next_chunk:
        bytes_remaining -= bytes_this_page;
        bytes_copied += bytes_this_page;
        current_vaddr += bytes_this_page;
    }

    return (bytes_done == 0) ? status : (int)bytes_done;
}

#endif // VIRTUAL_MEMORY_RW_H