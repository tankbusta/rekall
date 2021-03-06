//  MacPmem - Rekall Memory Forensics
//  Copyright (c) 2015 Google Inc. All rights reserved.
//
//  Implements the /dev/pmem device to provide read/write access to
//  physical memory.
//
//  Acknowledgments:
//   PTE remapping technique based on "Anti-Forensic Resilient Memory
//   Acquisition" (http://www.dfrws.org/2013/proceedings/DFRWS2013-13.pdf)
//   and OSXPmem reference implementation by Johannes Stüttgen.
//
//  Authors:
//   Adam Sindelar (adam.sindelar@gmail.com)
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "MacPmem.h"
#include "pte_mmap.h"
#include "logging.h"
#include "meta.h"
#include "i386_ptable.h"

#include <kern/task.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <mach/mach_vm.h>

// The headers don't include this, so this is just copied from vm internals.
#define SUPERPAGE_SIZE (2*1024*1024)

// This extern totally exists and kxld will find it, even though it's
// technically part of the unsupported kpi and not in any headers. If Apple
// eventually ends up not exporting this symbol we'll just have to get the
// kernel map some other way (probably from the kernel_task).
extern vm_map_t kernel_map;

// More unsupported, but exported symbols we need. All these two routines do
// is basically add 'paddr' to physmap_base and dereference the result, but
// because physmap_base is private (sigh...) we have to use these for now.
extern "C" {
extern unsigned long long ml_phys_read_double_64(addr64_t paddr);
extern void ml_phys_write_double_64(addr64_t paddr64, unsigned long long data);
}

// Flush this page's TLB.
static void pmem_pte_flush_tlb(vm_address_t page) {
    __asm__ __volatile__("invlpg (%0);"::"r"(page):);
}

// Keeps track of a rogue page, and its original paging structure.
typedef struct _pmem_pte_mapping {
    addr64_t paddr;
    vm_address_t vaddr;
    vm_size_t pagesize;
    union {
        struct {
            addr64_t pte_addr;
            PTE orig_pte;
        };
        struct {
            addr64_t pde_addr;
            PDE orig_pde;
        };
    };
} pmem_pte_mapping;


// Reads the PML4E for the 'page' virtual address.
//
// Arguments:
// page: virtual address of the address whose PML4E is wanted.
//       page-aligned automatically.
// pml4e: If provided, the PML4E struct is copied here.
// pml4e_phys: If provided, the physical address of the PML4E is copied here.
//
// Returns: KERN_SUCCESS or KERN_FAILURE
static kern_return_t pmem_read_pml4e(vm_address_t page, PML4E *pml4e,
                                     addr64_t *pml4e_phys) {
    VIRT_ADDR vaddr;
    vaddr.value = page;
    kern_return_t error = KERN_FAILURE;
    CR3 cr3;
    pmem_meta_t *meta = nullptr;
    addr64_t entry_paddr;

    error = pmem_fillmeta(&meta, PMEM_INFO_CR3);
    if (error != KERN_SUCCESS) {
        pmem_error("pmem_fillmeta failed to get CR3.");
        goto bail;
    }

    cr3.value = meta->cr3;
    entry_paddr = PFN_TO_PAGE(cr3.pml4_p) + (vaddr.pml4_index * sizeof(PML4E));

    pmem_debug("PML4E for vaddr %#016lx is at physical address %#016llx.",
               page, entry_paddr);

    if (pml4e) {
        // ml_phys_* can only succeed or panic, there are no recoverable errors.
        pml4e->value = ml_phys_read_double_64(entry_paddr);
    }

    if (pml4e_phys) {
        *pml4e_phys = entry_paddr;
    }

bail:
    if (meta) {
        pmem_metafree(meta);
    }

    return error;
}


// See docstring for pmem_read_pml4e.
static kern_return_t pmem_read_pdpte(vm_address_t page, PDPTE *pdpte,
                                     addr64_t *pdpte_phys) {
    VIRT_ADDR vaddr;
    vaddr.value = page;
    kern_return_t error;
    PML4E pml4e;
    addr64_t entry_paddr;

    error = pmem_read_pml4e(page, &pml4e, nullptr);
    if (error != KERN_SUCCESS) {
        return error;
    }

    if (!pml4e.present) {
        pmem_error("PML4E %u for vaddr %#016llx is not present.",
                   vaddr.pml4_index, vaddr.value);
        return KERN_FAILURE;
    }

    entry_paddr = PFN_TO_PAGE(pml4e.pdpt_p) + (vaddr.pdpt_index *sizeof(pdpte));

    pmem_debug("PDPTE for vaddr %#016lx is at physical address %#016llx.",
               page, entry_paddr);

    if (pdpte) {
        pdpte->value = ml_phys_read_double_64(entry_paddr);
    }

    if (pdpte_phys) {
        *pdpte_phys = entry_paddr;
    }

    return KERN_SUCCESS;
}


// See docstring for pmem_read_pml4e.
static kern_return_t pmem_read_pde(vm_address_t page, PDE *pde,
                                   addr64_t *pde_phys) {
    VIRT_ADDR vaddr;
    vaddr.value = page;
    kern_return_t error;
    PDPTE pdpte;
    addr64_t entry_paddr;

    error = pmem_read_pdpte(page, &pdpte, nullptr);
    if (error != KERN_SUCCESS) {
        return error;
    }

    if (!pdpte.present) {
        pmem_error("PDPTE %u of vaddr %#016llx is not present.",
                   vaddr.pdpt_index, vaddr.value);
        return KERN_FAILURE;
    }

    if (pdpte.page_size) {
        pmem_error("PDPTE %u of vaddr %#016llx is for a large (1 GB) page.",
                   vaddr.pdpt_index, vaddr.value);
        return KERN_FAILURE;
    }

    entry_paddr = PFN_TO_PAGE(pdpte.pd_p) + (vaddr.pd_index * sizeof(PDE));

    pmem_debug("PDE for vaddr %#016lx is at physical address %#016llx.",
               page, entry_paddr);

    if (pde) {
        pde->value = ml_phys_read_double_64(entry_paddr);
    }

    if (pde_phys) {
        *pde_phys = entry_paddr;
    }

    return KERN_SUCCESS;
}


// See docstring for pmem_read_pml4e.
static kern_return_t pmem_read_pte(vm_address_t page, PTE *pte,
                                   addr64_t *pte_phys) {
    VIRT_ADDR vaddr;
    vaddr.value = page;
    kern_return_t error;
    PDE pde;
    addr64_t entry_paddr;

    error = pmem_read_pde(page, &pde, nullptr);
    if (error != KERN_SUCCESS) {
        return error;
    }

    if (!pde.present) {
        pmem_error("PDE %u of vaddr %#016llx is not present.",
                   vaddr.pd_index, vaddr.value);
        return KERN_FAILURE;
    }

    if (pde.page_size) {
        pmem_error("PDE %u of vaddr %#016llx is for a huge (2 MB) page.",
                   vaddr.pd_index, vaddr.value);
        return KERN_FAILURE;
    }

    entry_paddr = PFN_TO_PAGE(pde.pt_p) + (vaddr.pt_index * sizeof(PTE));

    pmem_debug("PTE for vaddr %#016lx is at physical address %#016llx.",
               page, entry_paddr);

    if (pte) {
        pte->value = ml_phys_read_double_64(entry_paddr);
    }

    if (pte_phys) {
        *pte_phys = entry_paddr;
    }

    return KERN_SUCCESS;
}


// Overwrites the PTE at physical offset.
//
// Arguments:
// pte_phys: The physical address to overwrite. This must be a valid PTE.
// pte: The PTE struct to overwrite the address with.
//
// Returns KERN_SUCCESS. If you provide invalid values, you'll notice quickly,
// don't worry.
kern_return_t pmem_write_pte(addr64_t pte_phys, PTE *pte) {
    ml_phys_write_double_64(pte_phys, pte->value);
    return KERN_SUCCESS;
}


// See pmem_write_pte.
kern_return_t pmem_write_pde(addr64_t pde_phys, PDE *pde) {
    ml_phys_write_double_64(pde_phys, pde->value);
    return KERN_SUCCESS;
}


// Creates a new (non-global) rogue page mapped to paddr.
//
// Arguments:
//  paddr: The desired physical page. Must be aligned.
//  mapping: If successful, mapping->vaddr will contain a virtual address
//      mapped to paddr, of size mapping->pagesize. After being used, this
//      mapping must be passed to pmem_pte_destroy_mapping to be cleaned up.
//
// Notes:
//  Currently, only 4K pages are used. 2MB page support will be added in the
//  future.
//
// Returns: KERN_SUCCESS or KERN_FAILURE.
kern_return_t pmem_pte_create_mapping(addr64_t paddr,
                                      pmem_pte_mapping *mapping) {
    kern_return_t error = KERN_FAILURE;
    int flags = VM_FLAGS_ANYWHERE;

#if PMEM_USE_LARGE_PAGES
    mapping->pagesize = SUPERPAGE_SIZE_2MB;
#else
    mapping->pagesize = PAGE_SIZE;
#endif

    error = vm_allocate(kernel_map, &mapping->vaddr, mapping->pagesize, flags);

    if (error != KERN_SUCCESS) {
        bzero(mapping, sizeof(pmem_pte_mapping));
        pmem_error("Could not reserve a page. Error code: %d.", error);
        return error;
    }

    // We now have a speculative page. Write to it to force a pagefault.
    // After this the paging structures will exist.
    memset((void *)mapping->vaddr, 1, sizeof(int));

    // Grab a copy of the paging structure (PTE or PDE).
#if PMEM_USE_LARGE_PAGES
    error = pmem_read_pde(mapping->vaddr, &mapping->orig_pde,
                          &mapping->pde_addr);
#else
    error = pmem_read_pte(mapping->vaddr, &mapping->orig_pte,
                          &mapping->pte_addr);
#endif

    if (error != KERN_SUCCESS) {
        bzero(mapping, sizeof(pmem_pte_mapping));
        pmem_error("Could not find the PTE or PDE for rogue page. Bailing.");
        return error;
    }

    // pmem_read_* functions already verify the paging structure is present,
    // but for PDEs we also need to ensure the size flag is set.
#if PMEM_USE_LARGE_PAGES
    if (!mapping->orig_pde.page_size) {
        pmem_error("PDE was reserved for a 2MB page, but page_size flag is "
                   "not set. Bailing.");
        bzero(mapping, sizeof(pmem_pte_mapping));
        return KERN_FAILURE;
    }
#endif

    // Now we have a page of our own and can do horrible things to it.
#ifdef PMEM_USE_LARGE_PAGES
    PDE new_pde = mapping->orig_pde;
    new_pde.pt_p = PAGE_TO_PFN(paddr);

    pmem_write_pde(mapping->pde_addr, &new_pde);
#else
    PTE new_pte = mapping->orig_pte;
    new_pte.page_frame = PAGE_TO_PFN(paddr);

    // We absolutely want the TLB for this page flushed when switching context.
    new_pte.global = 0;

    pmem_write_pte(mapping->pte_addr, &new_pte);
#endif

    pmem_pte_flush_tlb(mapping->vaddr);
    return KERN_SUCCESS;
}


// Destroys a mapping created by pmme_pte_create_mapping.
//
// Arguments:
//  mapping: The paging structures of the virtual page will be restored to
//      their original values, and the page will be deallocated. The mapping
//      struct will be bzero'd.
//
// Returns:
//  KERN_SUCCESS or KERN_FAILURE.
kern_return_t pmem_pte_destroy_mapping(pmem_pte_mapping *mapping) {
    if (!mapping->vaddr) {
        return KERN_SUCCESS;
    }

#if PMEM_USE_LARGE_PAGES
    pmem_write_pde(mapping->pde_addr, &mapping->orig_pde);
#else
    pmem_write_pte(mapping->pte_addr, &mapping->orig_pte);
#endif

    kern_return_t error = vm_deallocate(kernel_map, mapping->vaddr,
                                        mapping->pagesize);

    if (error != KERN_SUCCESS) {
        pmem_error("Could not free reserved page %#016lx.", mapping->vaddr);
        return error;
    }

    bzero(mapping, sizeof(pmem_pte_mapping));
    return KERN_SUCCESS;
}


// Read handler for /dev/pmem. Does what you'd expect.
//
// It's alright to call this for reads that cross page boundaries.
//
// NOTE: This function does absolutely no verification that the physical
// offset being read from is actually backed by conventional, or, indeed, any
// memory at all. It is the responsibility of the caller to ensure the offset
// is valid.
kern_return_t pmem_read_rogue(struct uio *uio) {
    kern_return_t error = KERN_SUCCESS;

    if (uio_offset(uio) < 0) {
        // Negative offsets into physical memory really make no sense. Without
        // this check, this call would just return all zeroes, but it's
        // probably better to just fail.
        return KERN_FAILURE;
    }

    pmem_pte_mapping mapping;

    user_ssize_t resid = uio_resid(uio);
    off_t offset = uio_offset(uio);
    unsigned long amount, rv;
    while (resid > 0) {
        error = pmem_pte_create_mapping(offset & ~PAGE_MASK, &mapping);
        if (error != KERN_SUCCESS) {
            pmem_error("Could not acquire a rogue page.");
            goto bail;
        }
        user_ssize_t page_offset = offset % mapping.pagesize;
        amount = MIN(resid, mapping.pagesize - page_offset);
        rv = uiomove((char *)mapping.vaddr + page_offset, (int)amount, uio);

        if (rv != 0) {
            // If this happens, it's basically the kernel's problem.
            // All we can do is fail and log.
            pmem_error("uiomove returned %lu.", rv);
            error = KERN_FAILURE;
            goto bail;
        }

        offset += amount;
        resid = uio_resid(uio);
        error = pmem_pte_destroy_mapping(&mapping);
        if (error != KERN_SUCCESS) {
            pmem_error("Could not release a rogue page.");
            goto bail;
        }
    }

bail:
    return error;
}


// Finds physical address corresponding to the virtual address.
//
// Arguments:
//  vaddr: The virtual address whose physical offset is desired.
//  paddr: If successful, the physical offset will be written here.
//
// Returns:
//  KERN_SUCCESS or KERN_FAILURE.
kern_return_t pmem_pte_vtop(vm_offset_t vaddr, unsigned long long *paddr) {
    kern_return_t error;

    PTE pte;
    error = pmem_read_pte(vaddr, &pte, 0);

    if (error == KERN_SUCCESS) {
        // This returns the address of a 4K page.
        *paddr = (pte.page_frame << PAGE_SHIFT) + (vaddr % PAGE_SIZE);
        return error;
    }

    // If that failed, the page is either paged out (no phys address) or a
    // huge page.
    PDE pde;
    error = pmem_read_pde(vaddr, &pde, 0);

    if (error == KERN_SUCCESS && pde.page_size) {
        // Not SUPERPAGE_SHIFT (16) because the bit offset of the page in PD
        // and PT entries is the same (9).
        *paddr = ((pde.pt_p << PAGE_SHIFT) +
                  (vaddr % SUPERPAGE_SIZE));
    }

    // If we got here the vaddr is likely paged out (or part of a 1GB page,
    // which is currently unlikely.)
    return error;
}


kern_return_t pmem_pte_init() {
    // No initialization currently needed.
    return KERN_SUCCESS;
}


void pmem_pte_cleanup() {
    // No cleanup code needed.
}
