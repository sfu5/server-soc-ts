/** @file
 * Copyright (c) 2023, Arm Limited or its affiliates. All rights reserved.
 * SPDX-License-Identifier : Apache-2.0

 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
**/


#include "pal_pcie_enum.h"
#include "pal_common_support.h"
#include "platform_image_def.h"
#include "platform_override_fvp.h"

#define __ADDR_ALIGN_MASK(a, mask)    (((a) + (mask)) & ~(mask))
#define ADDR_ALIGN(a, b)              __ADDR_ALIGN_MASK(a, (typeof(a))(b) - 1)

void *mem_alloc(size_t alignment, size_t size);
void mem_free(void *ptr);

typedef struct {
    uint64_t base;
    uint64_t size;
} val_host_alloc_region_ts;

static uint64_t heap_base;
static uint64_t heap_top;
static uint64_t heap_init_done = 0;

#ifdef ENABLE_OOB
/* Below code is not applicable for Bare-metal
 * Only for FVP OOB experience
 */

#include  <Library/ShellCEntryLib.h>
#include  <Library/UefiBootServicesTableLib.h>
#include  <Library/UefiLib.h>
#include  <Library/ShellLib.h>
#include  <Library/PrintLib.h>
#include  <Library/BaseMemoryLib.h>
#include <Protocol/Cpu.h>

#endif

/**
  @brief  Sends a formatted string to the output console

  @param  string  An ASCII string
  @param  data    data for the formatted output

  @return None
**/
void
pal_print(char *string, uint64_t data)
{

#ifdef ENABLE_OOB
 /* Below code is not applicable for Bare-metal
  * Only for FVP OOB experience
 */
    AsciiPrint(string, data);
#else
    (void) string;
    (void) data;
#endif
}

/**
  @brief  Allocates memory of the requested size.

  @param  Bdf:  BDF of the requesting PCIe device
  @param  Size: size of the memory region to be allocated
  @param  Pa:   physical address of the allocated memory
**/
void *
pal_mem_alloc_cacheable(uint32_t Bdf, uint32_t Size, void **Pa)
{

#ifdef ENABLE_OOB
 /* Below code is not applicable for Bare-metal
  * Only for FVP OOB experience
  */

  EFI_PHYSICAL_ADDRESS      Address;
  EFI_CPU_ARCH_PROTOCOL     *Cpu;
  EFI_STATUS                Status;

  Status = gBS->AllocatePages (AllocateAnyPages,
                               EfiBootServicesData,
                               EFI_SIZE_TO_PAGES(Size),
                               &Address);
  if (EFI_ERROR(Status)) {
    print(ACS_PRINT_ERR, "Allocate Pool failed %x\n", Status);
    return NULL;
  }

  /* Check Whether Cpu architectural protocol is installed */
  Status = gBS->LocateProtocol ( &gEfiCpuArchProtocolGuid, NULL, (VOID **)&Cpu);
  if (EFI_ERROR(Status)) {
    print(ACS_PRINT_ERR, "Could not get Cpu Arch Protocol %x\n", Status);
    return NULL;
  }

  /* Set Memory Attributes */
  Status = Cpu->SetMemoryAttributes (Cpu,
                                     Address,
                                     Size,
                                     EFI_MEMORY_WB);
  if (EFI_ERROR (Status)) {
    print(ACS_PRINT_ERR, "Could not Set Memory Attribute %x\n", Status);
    return NULL;
  }

  *Pa = (VOID *)Address;
  return (VOID *)Address;
#elif defined (TARGET_BM_BOOT)
  void *address;
  uint32_t alignment = 0x08;
  (void) Bdf;
  address = (void *)mem_alloc(alignment, Size);
  *Pa = (void *)address;
  return (void *)address;
#endif
  return 0;
}

/**
  @brief  Frees the memory allocated

  @param  Bdf:  BDF of the requesting PCIe device
  @param  Size: size of the memory region to be freed
  @param  Va:   virtual address of the memory to be freed
  @param  Pa:   physical address of the memory to be freed
**/
void
pal_mem_free_cacheable(uint32_t Bdf, uint32_t Size, void *Va, void *Pa)
{

#ifdef ENABLE_OOB
 /* Below code is not applicable for Bare-metal
  * Only for FVP OOB experience
  */

  gBS->FreePages((EFI_PHYSICAL_ADDRESS)(UINTN)Va, EFI_SIZE_TO_PAGES(Size));
#else
  (void) Bdf;
  (void) Size;
  (void) Va;
  (void) Pa;
#endif

}

/**
  @brief  Returns the physical address of the input virtual address.

  @param Va virtual address of the memory to be converted

  Returns the physical address.
**/
void *
pal_mem_virt_to_phys(void *Va)
{
  /* Place holder function. Need to be
   * implemented if needed in later releases
   */
  return Va;
}

/**
  @brief  Returns the virtual address of the input physical address.

  @param Pa physical address of the memory to be converted

  Returns the virtual address.
**/
void *
pal_mem_phys_to_virt (
  uint64_t Pa
  )
{
  /* Place holder function*/
  return (void*)Pa;
}


/**
  Stalls the CPU for the number of microseconds specified by MicroSeconds.

  @param  MicroSeconds  The minimum number of microseconds to delay.

  @return 0 - Success

**/
uint64_t
pal_time_delay_ms(uint64_t MicroSeconds)
{
  /**Need to implement**/
#ifdef ENABLE_OOB
 /* Below code is not applicable for Bare-metal
 * Only for FVP OOB experience
 */

  gBS->Stall(MicroSeconds);
#else
  (void) MicroSeconds;
#endif
  return 0;
}

/**
  @brief  page size being used in current translation regime.

  @return page size being used
**/
uint32_t
pal_mem_page_size()
{
#ifdef ENABLE_OOB
 /* Below code is not applicable for Bare-metal
 * Only for FVP OOB experience
 */

    return EFI_PAGE_SIZE;
#else
    return PLATFORM_PAGE_SIZE;
#endif
}

/**
  @brief  allocates contiguous numpages of size
          returned by pal_mem_page_size()

  @return Start address of base page
**/
void *
pal_mem_alloc_pages (uint32_t NumPages)
{
#ifdef ENABLE_OOB
 /* Below code is not applicable for Bare-metal
 * Only for FVP OOB experience
 */

  EFI_STATUS Status;
  EFI_PHYSICAL_ADDRESS PageBase;

  Status = gBS->AllocatePages (AllocateAnyPages,
                               EfiBootServicesData,
                               NumPages,
                               &PageBase);
  if (EFI_ERROR(Status))
  {
    print(ACS_PRINT_ERR, "Allocate Pages failed %x\n", Status);
    return NULL;
  }

  return (VOID*)(UINTN)PageBase;
#else
  return (void *)mem_alloc(MEM_ALIGN_4K, NumPages * PLATFORM_PAGE_SIZE);
#endif
}

/**
  @brief  frees continguous numpages starting from page
          at address PageBase

**/
void
pal_mem_free_pages(void *PageBase, uint32_t NumPages)
{
#ifdef ENABLE_OOB
 /* Below code is not applicable for Bare-metal
 * Only for FVP OOB experience
 */

  gBS->FreePages((EFI_PHYSICAL_ADDRESS)(UINTN)PageBase, NumPages);
#else
  (void) PageBase;
  (void) NumPages;
#endif
}

/**
  @brief  Allocates memory with the given alignement.

  @param  Alignment   Specifies the alignment.
  @param  Size        Requested memory allocation size.

  @return Pointer to the allocated memory with requested alignment.
**/
void
*pal_aligned_alloc( uint32_t alignment, uint32_t size )
{
#ifdef ENABLE_OOB
  VOID *Mem = NULL;
  VOID **Aligned_Ptr = NULL;

  /* Generate mask for the Alignment parameter*/
  UINT64 Mask = ~(UINT64)(alignment - 1);

  /* Allocate memory with extra bytes, so we can return an aligned address*/
  Mem = (VOID *)pal_mem_alloc(size + alignment);

  if( Mem == NULL)
    return 0;

  /* Add the alignment to allocated memory address and align it to target alignment*/
  Aligned_Ptr = (VOID **)(((UINT64) Mem + alignment - 1) & Mask);

  /* Using a double pointer to store the address of allocated
     memory location so that it can be used to free the memory later*/
  Aligned_Ptr[-1] = Mem;

  return Aligned_Ptr;

#else
  return (void *)mem_alloc(alignment, size);

#endif
}

/**
  @brief  Free the Aligned memory allocated by UEFI Framework APIs

  @param  Buffer        the base address of the aligned memory range

  @return None
*/

void
pal_mem_free_aligned (void *Buffer)
{
#ifdef ENABLE_OOB
    free(((VOID **)Buffer)[-1]);
    return;
#else
    mem_free(Buffer);
    return;
#endif
}

/* Functions implemented below are used to allocate memory from heap. Baremetal implementation
   of memory allocation.
*/

static int is_power_of_2(uint32_t n)
{
    return n && !(n & (n - 1));
}

/**
 * @brief Allocates contiguous memory of requested size(no_of_bytes) and alignment.
 * @param alignment - alignment for the address. It must be in power of 2.
 * @param Size - Size of the region. It must not be zero.
 * @return - Returns allocated memory base address if allocation is successful.
 *           Otherwise returns NULL.
 **/
void *heap_alloc(size_t alignment, size_t size)
{
    uint64_t addr;

    addr = ADDR_ALIGN(heap_base, alignment);
    size += addr - heap_base;

    if ((heap_top - heap_base) < size)
    {
       return NULL;
    }

    heap_base += size;

    return (void *)addr;
}

/**
 * @brief  Initialisation of allocation data structure
 * @param  void
 * @return Void
 **/
void mem_alloc_init(void)
{
    heap_base = PLATFORM_HEAP_REGION_BASE;
    heap_top = PLATFORM_HEAP_REGION_BASE + PLATFORM_HEAP_REGION_SIZE;
    heap_init_done = 1;
}

/**
 * @brief Allocates contiguous memory of requested size(no_of_bytes) and alignment.
 * @param alignment - alignment for the address. It must be in power of 2.
 * @param Size - Size of the region. It must not be zero.
 * @return - Returns allocated memory base address if allocation is successful.
 *           Otherwise returns NULL.
 **/
void *mem_alloc(size_t alignment, size_t size)
{
  void *addr = NULL;

  if(heap_init_done != 1)
    mem_alloc_init();

  if (size <= 0)
  {
    return NULL;
  }

  if (!is_power_of_2((uint32_t)alignment))
  {
    return NULL;
  }

  size += alignment - 1;
  addr = heap_alloc(alignment, size);

  return addr;
}

/**
 * TODO: Free the memory for given memory address
 * Currently acs code is initialisazing from base for every test,
 * the regions data structure is internal and below code only setting to zero
 * not actually freeing memory.
 * If require can revisit in future.
 **/
void mem_free(void *ptr)
{
  if (!ptr)
    return;

  return;
}
