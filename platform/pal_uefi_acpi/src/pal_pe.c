/** @file
 * Copyright (c) 2016-2023, Arm Limited or its affiliates. All rights reserved.
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
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include "Include/IndustryStandard/Acpi61.h"
#include <Protocol/AcpiTable.h>
#include <Protocol/Cpu.h>

#include <sbi/riscv_asm.h>
#include <sbi/riscv_encoding.h>

#include "include/pal_uefi.h"

static   EFI_ACPI_6_1_MULTIPLE_APIC_DESCRIPTION_TABLE_HEADER *gMadtHdr;
static EFI_ACPI_6_5_RISC_V_HART_CAPABILITIES_TABLE_STRUCTURE *gRhctHdr;

UINT8   *gSecondaryPeStack;
UINT64  gMpidrMax;
static UINT32 g_num_pe;
extern INT32 gPsciConduit;

#define SIZE_STACK_SECONDARY_PE  0x100		//256 bytes per core
#define UPDATE_AFF_MAX(src,dest,mask)  ((dest & mask) > (src & mask) ? (dest & mask) : (src & mask))

#define ENABLED_BIT(flags)  (flags & 0x1)
#define ONLINE_CAP_BIT(flags)  ((flags > 1) & 0x1)

UINT64
pal_get_madt_ptr();

UINT64
pal_get_fadt_ptr (
  VOID
  );

UINT64
pal_get_rhct_ptr();

VOID
ArmCallSmc (
  IN OUT ARM_SMC_ARGS *Args,
  IN     INT32        Conduit
  );

/**
  @brief   Queries the FADT ACPI table to check whether PSCI is implemented and,
           if so, using which conduit (HVC or SMC).

  @retval  CONDUIT_UNKNOWN:       The FADT table could not be discovered.
  @retval  CONDUIT_NONE:          PSCI is not implemented
  @retval  CONDUIT_SMC:           PSCI is implemented and uses SMC as
                                  the conduit.
  @retval  CONDUIT_HVC:           PSCI is implemented and uses HVC as
                                  the conduit.
**/
INT32
pal_psci_get_conduit (
  VOID
  )
{
  EFI_ACPI_6_1_FIXED_ACPI_DESCRIPTION_TABLE  *Fadt;

  Fadt = (EFI_ACPI_6_1_FIXED_ACPI_DESCRIPTION_TABLE *)pal_get_fadt_ptr ();
  if (!Fadt) {
    return CONDUIT_UNKNOWN;
  } else if (!(Fadt->ArmBootArch & EFI_ACPI_6_1_ARM_PSCI_COMPLIANT)) {
    return CONDUIT_NONE;
  } else if (Fadt->ArmBootArch & EFI_ACPI_6_1_ARM_PSCI_USE_HVC) {
    return CONDUIT_HVC;
  } else {
    return CONDUIT_SMC;
  }
}

/**
  @brief   Return the base address of the region allocated for Stack use for the Secondary
           PEs.
  @param   None
  @return  base address of the Stack
**/
UINT64
PalGetSecondaryStackBase()
{

  return (UINT64)gSecondaryPeStack;
}

/**
  @brief   Return the number of PEs in the System.
  @param   None
  @return  num_of_pe
**/
UINT32
pal_pe_get_num()
{

  return (UINT32)g_num_pe;
}

/**
  @brief   Returns the Max of each 8-bit Affinity fields in MPIDR.
  @param   None
  @return  Max MPIDR
**/
UINT64
PalGetMaxMpidr()
{

  return gMpidrMax;
}

/**
  @brief  Allocate memory region for secondary PE stack use. SIZE of stack for each PE
          is a #define

  @param  mpidr Pass MIPDR register content
  @return  None
**/
VOID
PalAllocateSecondaryStack(UINT64 mpidr)
{
  EFI_STATUS Status;
  UINT8 *Buffer;
  UINT32 NumPe, Aff0, Aff1, Aff2, Aff3, StackSize;

  Aff0 = ((mpidr & 0x00000000ff) >>  0);
  Aff1 = ((mpidr & 0x000000ff00) >>  8);
  Aff2 = ((mpidr & 0x0000ff0000) >> 16);
  Aff3 = ((mpidr & 0xff00000000) >> 32);

  NumPe = ((Aff3+1) * (Aff2+1) * (Aff1+1) * (Aff0+1));

  if (gSecondaryPeStack == NULL) {
      // AllocatePool guarantees 8b alignment, but stack pointers must be 16b
      // aligned for aarch64. Pad the size with an extra 8b so that we can
      // force-align the returned buffer to 16b. We store the original address
      // returned if we do have to align we still have the proper address to
      // free.

      StackSize = (NumPe * SIZE_STACK_SECONDARY_PE) + CPU_STACK_ALIGNMENT;
      Status = gBS->AllocatePool ( EfiBootServicesData,
                    StackSize,
                    (VOID **) &Buffer);
      if (EFI_ERROR(Status)) {
          bsa_print(ACS_PRINT_ERR, L"\n FATAL - Allocation for Seconday stack failed %x\n", Status);
      }
      pal_pe_data_cache_ops_by_va((UINT64)&Buffer, CLEAN_AND_INVALIDATE);

      // Check if we need alignment
      if ((UINT8*)(((UINTN) Buffer) & (0xFll))) {
        // Needs alignment, so just store the original address and return +1
        ((UINTN*)Buffer)[0] = (UINTN)Buffer;
        gSecondaryPeStack = (UINT8*)(((UINTN*)Buffer)+1);
      } else {
        // None needed. Just store the address with padding and return.
        ((UINTN*)Buffer)[1] = (UINTN)Buffer;
        gSecondaryPeStack = (UINT8*)(((UINTN*)Buffer)+2);
      }
  }

}

/**
  @brief  This API fills in the PE_INFO Table with information about the PEs in the
          system. This is achieved by parsing the ACPI - MADT table.

  @param  PeTable  - Address where the PE information needs to be filled.

  @return  None
**/
VOID
pal_pe_create_info_table(PE_INFO_TABLE *PeTable)
{
  EFI_ACPI_6_5_RINTC_STRUCTURE                *Entry = NULL;
  EFI_ACPI_6_5_RHCT_NODE_HEADER               *RhctNodeEntry = NULL;
  EFI_ACPI_6_5_RHCT_HART_INFO_NODE_STRUCTURE  *HartInfoNode = NULL;
  EFI_ACPI_6_5_RHCT_ISA_STRING_NODE_STRUCTURE *IsaStringNode = NULL;
  PE_INFO_ENTRY                               *Ptr = NULL;
  UINT32                                      MadtTableLength = 0;
  UINT32                                      RhctTableLength = 0;
  UINT32                                      Length = 0;
  UINT32                                      Index = 0;
  UINT32                                      Index2 = 0;
  UINT32                                      Flags = 0;

  if (PeTable == NULL) {
    bsa_print(ACS_PRINT_ERR, L" Input PE Table Pointer is NULL. Cannot create PE INFO\n");
    return;
  }

  /* initialise number of PEs to zero */
  PeTable->header.num_of_pe = 0;

  gMadtHdr = (EFI_ACPI_6_1_MULTIPLE_APIC_DESCRIPTION_TABLE_HEADER *) pal_get_madt_ptr();

  if (gMadtHdr != NULL) {
    MadtTableLength =  gMadtHdr->Header.Length;
    bsa_print(ACS_PRINT_INFO, L"  MADT is at %x and length is %x\n", gMadtHdr, MadtTableLength);
  } else {
    bsa_print(ACS_PRINT_ERR, L" MADT not found\n");
    return;
  }

  gRhctHdr = (EFI_ACPI_6_5_RISC_V_HART_CAPABILITIES_TABLE_STRUCTURE *) pal_get_rhct_ptr();
  if (gRhctHdr != NULL) {
    RhctTableLength =  gRhctHdr->Header.Length;
    bsa_print(ACS_PRINT_INFO, L"  RHCT is at %x and length is %x\n", gRhctHdr, RhctTableLength);
  } else {
    bsa_print(ACS_PRINT_ERR, L" RHCT not found\n");
    return;
  }

  Entry = (EFI_ACPI_6_5_RINTC_STRUCTURE *) (gMadtHdr + 1);
  Length = sizeof (EFI_ACPI_6_1_MULTIPLE_APIC_DESCRIPTION_TABLE_HEADER);
  Ptr = PeTable->pe_info;

  do {
    if ((Entry->Type == EFI_ACPI_6_5_RINTC)) {
      //Fill in the hart num and the id in pe info table
      Flags           = Entry->Flags;
      bsa_print(ACS_PRINT_INFO, L"  RINTC Flags %x\n", Flags);
      bsa_print(ACS_PRINT_DEBUG, L"  PE Enabled %d, Online Capable %d\n", ENABLED_BIT(Flags), ONLINE_CAP_BIT(Flags));

      /* As per MADT (RISC-V INTC Flags) a processor is usable when
           Enabled bit is set
           Enabled bit is clear and Online Capable bit is set
           if both bits are clear, processor is not usable
      */
      if ((ENABLED_BIT(Flags) == 1) || (ONLINE_CAP_BIT(Flags) == 1)) {
        Ptr->hart_id = Entry->HartId;
        Ptr->pe_num     = PeTable->header.num_of_pe;
        Ptr->acpi_processor_uid = Entry->AcpiProcessorUid;
        Ptr->ext_intc_id = Entry->ExternalINTCId;
        Ptr->imsic_base = Entry->IMSICBase;
        Ptr->imsic_size = Entry->IMSICSize;
        bsa_print(ACS_PRINT_DEBUG, L"  HartID 0x%lx PE num 0x%x\n", Ptr->hart_id, Ptr->pe_num);
        bsa_print(ACS_PRINT_DEBUG, L"    Processor UID %d\n", Ptr->acpi_processor_uid);
        bsa_print(ACS_PRINT_DEBUG, L"    IMSIC Base 0x%lx IMSIC Soze 0x%x\n", Ptr->imsic_base, Ptr->imsic_size);

        // Find Hart Info node first.
        HartInfoNode = (EFI_ACPI_6_5_RHCT_HART_INFO_NODE_STRUCTURE *)((UINTN)gRhctHdr + gRhctHdr->RHCTNodeOffset);
        for (Index = 0; Index < gRhctHdr->RHCTNodeNumber; Index++) {
          if (HartInfoNode->Header.Type == EFI_ACPI_6_5_RHCT_NODE_TYPE_HART_INFO_NODE &&
              HartInfoNode->AcpiProcessorUid == Entry->AcpiProcessorUid) {
            bsa_print(ACS_PRINT_INFO, L"      HART Info is found\n");

            // Go through HartInfoNode.Offsets for other RHCT nodes of this hart
            for (Index2 = 0; Index2 < HartInfoNode->OffsetNumber; Index2++) {
              RhctNodeEntry = (EFI_ACPI_6_5_RHCT_NODE_HEADER *)((UINTN)gRhctHdr + HartInfoNode->Offsets[Index2]);
              switch (RhctNodeEntry->Type) {
                case EFI_ACPI_6_5_RHCT_NODE_TYPE_ISA_STRING_NODE:
                  IsaStringNode = (EFI_ACPI_6_5_RHCT_ISA_STRING_NODE_STRUCTURE *) RhctNodeEntry;
                  if (IsaStringNode->ISALength > sizeof(Ptr->isa_string)) {
                    bsa_print(ACS_PRINT_ERR, L"      Error: ISA String size overflow %d\n", IsaStringNode->ISALength);
                  }
                  CopyMem(Ptr->isa_string, IsaStringNode->ISAString, IsaStringNode->ISALength);
                  bsa_print(ACS_PRINT_INFO, L"      ISA string found: %a\n", Ptr->isa_string);
                  break;

                case EFI_ACPI_6_5_RHCT_NODE_TYPE_CMO_EXTENSION_NODE:
                  bsa_print(ACS_PRINT_INFO, L"      CMO found\n");
                  break;

                case EFI_ACPI_6_5_RHCT_NODE_TYPE_MMU_NODE:
                  bsa_print(ACS_PRINT_INFO, L"      MMU found\n");
                  break;

                default:
                  bsa_print(ACS_PRINT_INFO, L"      Unknow type %d found\n", RhctNodeEntry->Type);
              }
            }
          }
          HartInfoNode = (EFI_ACPI_6_5_RHCT_HART_INFO_NODE_STRUCTURE *)((UINTN)HartInfoNode + HartInfoNode->Header.Length);
        }
        pal_pe_data_cache_ops_by_va((UINT64)Ptr, CLEAN_AND_INVALIDATE);
        Ptr++;
        PeTable->header.num_of_pe++;
      }
    }

    Length += Entry->Length;
    Entry = (EFI_ACPI_6_5_RINTC_STRUCTURE *) ((UINT8 *)Entry + (Entry->Length));
  }while(Length < MadtTableLength);

  // gMpidrMax = MpidrAff0Max | MpidrAff1Max | MpidrAff2Max | MpidrAff3Max;
  g_num_pe = PeTable->header.num_of_pe;

  pal_pe_data_cache_ops_by_va((UINT64)PeTable, CLEAN_AND_INVALIDATE);
  // RV porting TODO: secondary stack allocationg should be ported for RV multi-processor tests
  // pal_pe_data_cache_ops_by_va((UINT64)&gMpidrMax, CLEAN_AND_INVALIDATE);
  // PalAllocateSecondaryStack(gMpidrMax);
}

/**
  @brief  Install Exception Handler using UEFI CPU Architecture protocol's
          Register Interrupt Handler API

  @param  ExceptionType  - AARCH64 Exception type
  @param  esr            - Function pointer of the exception handler

  @return status of the API
**/
UINT32
pal_pe_install_esr(UINT32 ExceptionType,  VOID (*esr)(UINT64, VOID *))
{

  EFI_STATUS  Status;
  EFI_CPU_ARCH_PROTOCOL   *Cpu;

  // Get the CPU protocol that this driver requires.
  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&Cpu);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Unregister the default exception handler.
  Status = Cpu->RegisterInterruptHandler (Cpu, ExceptionType, NULL);
  if (EFI_ERROR (Status) && Status != EFI_INVALID_PARAMETER) {
    // return EFI_INVALID_PARAMETER means no previous handler exists.
    bsa_print(ACS_PRINT_ERR, L"  fail to unregister esr: %r\n", Status);
    return Status;
  }

  // Register to receive interrupts
  Status = Cpu->RegisterInterruptHandler (Cpu, ExceptionType, (EFI_CPU_INTERRUPT_HANDLER)esr);
  if (EFI_ERROR (Status)) {
    bsa_print(ACS_PRINT_ERR, L"  fail to register esr: %r\n", Status);
    return Status;
  }

  return EFI_SUCCESS;
}

/**
  @brief  Make the SMC call using AARCH64 Assembly code
          SMC calls can take up to 7 arguments and return up to 4 return values.
          Therefore, the 4 first fields in the ARM_SMC_ARGS structure are used
          for both input and output values.

  @param  Argumets to pass to the EL3 firmware
  @param  Conduit  SMC or HVC

  @return  None
**/
VOID
pal_pe_call_smc(ARM_SMC_ARGS *ArmSmcArgs, INT32 Conduit)
{
  ArmCallSmc (ArmSmcArgs, Conduit);
}

VOID
ModuleEntryPoint();

/**
  @brief  Make a PSCI CPU_ON call using SMC instruction.
          Pass PAL Assembly code entry as the start vector for the PSCI ON call

  @param  Argumets to pass to the EL3 firmware

  @return  None
**/
VOID
pal_pe_execute_payload(ARM_SMC_ARGS *ArmSmcArgs)
{
  ArmSmcArgs->Arg2 = (UINT64)ModuleEntryPoint;
  pal_pe_call_smc(ArmSmcArgs, gPsciConduit);
}

/**
  @brief Update the ELR to return from exception handler to a desired address

  @param  context - exception context structure
  @param  offset - address with which ELR should be updated

  @return  None
**/
VOID
pal_pe_update_elr(VOID *context, UINT64 offset)
{
  // update sepc for RISC-V
  ((EFI_SYSTEM_CONTEXT_RISCV64*)context)->SEPC = offset;
}

/**
  @brief Get the Exception syndrome from UEFI exception handler

  @param  context - exception context structure

  @return  ESR
**/
UINT64
pal_pe_get_esr(VOID *context)
{
  return ((EFI_SYSTEM_CONTEXT_AARCH64*)context)->ESR;
}

/**
  @brief Get the FAR from UEFI exception handler

  @param  context - exception context structure

  @return  FAR
**/
UINT64
pal_pe_get_far(VOID *context)
{
  return ((EFI_SYSTEM_CONTEXT_AARCH64*)context)->FAR;
}

VOID
DataCacheCleanInvalidateVA(UINT64 addr);

VOID
DataCacheCleanVA(UINT64 addr);

VOID
DataCacheInvalidateVA(UINT64 addr);

/**
  @brief Perform cache maintenance operation on an address

  @param addr - address on which cache ops to be performed
  @param type - type of cache ops

  @return  None
**/
VOID
pal_pe_data_cache_ops_by_va(UINT64 addr, UINT32 type)
{
  switch(type){
      case CLEAN_AND_INVALIDATE:
          DataCacheCleanInvalidateVA(addr);
      break;
      case CLEAN:
          DataCacheCleanVA(addr);
      break;
      case INVALIDATE:
          DataCacheInvalidateVA(addr);
      break;
      default:
          DataCacheCleanInvalidateVA(addr);
  }
}


#define CSR_HSTATUS			0x600

UINT64
pal_pe_get_hstatus (void)
{
  return csr_read(CSR_HSTATUS);
}

VOID
pal_pe_set_hstatus (UINT64 val)
{
  csr_write(CSR_HSTATUS, val);
}