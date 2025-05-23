/** @file
*  File managing the MMU for ARMv8 architecture. This file is based on the original from the original file located
*  in ArmPkg/Library/ArmMmuLib/AArch64/ArmMmuLibCore.c with some key changes:
*  - external API function removed
*  - dynamic memory allocations replaced by a slab-based allocator since dynamic allocations are not yet possible this
*    early in the boot
*
*  SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
*
*  SPDX-License-Identifier: BSD-2-Clause-Patent
*
**/

#include "Uefi/UefiBaseType.h"
#include <Uefi.h>
#include <AArch64/AArch64.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/ArmMmuLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/StandaloneMmHafniumSlabMemAllocLib.h>
#include <Library/StandaloneMmArmLib.h>

STATIC
UINT64
ArmMemoryAttributeToPageAttribute (
  IN STMM_ARM_MEMORY_REGION_ATTRIBUTES  Attributes
  )
{
  switch (Attributes) {
    case STMM_ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_NONSHAREABLE:
      return TT_ATTR_INDX_MEMORY_WRITE_BACK;
    case STMM_ARM_MEMORY_REGION_ATTRIBUTE_NONSECURE_WRITE_BACK_NONSHAREABLE:
      return TT_ATTR_INDX_MEMORY_WRITE_BACK | TT_NS;

    case STMM_ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK:
      return TT_ATTR_INDX_MEMORY_WRITE_BACK | TT_SH_INNER_SHAREABLE;
    case STMM_ARM_MEMORY_REGION_ATTRIBUTE_NONSECURE_WRITE_BACK:
      return TT_ATTR_INDX_MEMORY_WRITE_BACK | TT_SH_INNER_SHAREABLE | TT_NS;

    case STMM_ARM_MEMORY_REGION_ATTRIBUTE_WRITE_THROUGH:
      return TT_ATTR_INDX_MEMORY_WRITE_THROUGH | TT_SH_INNER_SHAREABLE;
    case STMM_ARM_MEMORY_REGION_ATTRIBUTE_NONSECURE_WRITE_THROUGH:
      return TT_ATTR_INDX_MEMORY_WRITE_THROUGH | TT_SH_INNER_SHAREABLE | TT_NS;

    // Uncached and device mappings are treated as outer shareable by default,
    case STMM_ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED:
      return TT_ATTR_INDX_MEMORY_NON_CACHEABLE;
    case STMM_ARM_MEMORY_REGION_ATTRIBUTE_NONSECURE_UNCACHED_UNBUFFERED:
      return TT_ATTR_INDX_MEMORY_NON_CACHEABLE | TT_NS;

    default:
      ASSERT (0);
    case STMM_ARM_MEMORY_REGION_ATTRIBUTE_DEVICE:
      if (ArmReadCurrentEL () == AARCH64_EL2) {
        return TT_ATTR_INDX_DEVICE_MEMORY | TT_XN_MASK;
      } else {
        return TT_ATTR_INDX_DEVICE_MEMORY | TT_UXN_MASK | TT_PXN_MASK;
      }

    case STMM_ARM_MEMORY_REGION_ATTRIBUTE_NONSECURE_DEVICE:
      if (ArmReadCurrentEL () == AARCH64_EL2) {
        return TT_ATTR_INDX_DEVICE_MEMORY | TT_XN_MASK | TT_NS;
      } else {
        return TT_ATTR_INDX_DEVICE_MEMORY | TT_UXN_MASK | TT_PXN_MASK | TT_NS;
      }
  }
}

#define MIN_T0SZ        16
#define BITS_PER_LEVEL  9
#define MAX_VA_BITS     48

STATIC
UINTN
GetRootTableEntryCount (
  IN  UINTN  T0SZ
  )
{
  return TT_ENTRY_COUNT >> (T0SZ - MIN_T0SZ) % BITS_PER_LEVEL;
}

STATIC
UINTN
GetRootTableLevel (
  IN  UINTN  T0SZ
  )
{
  return (T0SZ - MIN_T0SZ) / BITS_PER_LEVEL;
}

STATIC
VOID
ReplaceTableEntry (
  IN  UINT64   *Entry,
  IN  UINT64   Value,
  IN  UINT64   RegionStart,
  IN  BOOLEAN  IsLiveBlockMapping
  )
{
  if (!ArmMmuEnabled () || !IsLiveBlockMapping) {
    *Entry = Value;
    ArmUpdateTranslationTableEntry (Entry, (VOID *)(UINTN)RegionStart);
  } else {
    ArmReplaceLiveTranslationEntry (Entry, Value, RegionStart, TRUE);
  }
}

STATIC
VOID
FreePageTablesRecursive (
  IN  UINT64  *TranslationTable,
  IN  UINTN   Level
  )
{
  UINTN  Index;

  ASSERT (Level <= 3);

  if (Level < 3) {
    for (Index = 0; Index < TT_ENTRY_COUNT; Index++) {
      if ((TranslationTable[Index] & TT_TYPE_MASK) == TT_TYPE_TABLE_ENTRY) {
        FreePageTablesRecursive (
          (VOID *)(UINTN)(TranslationTable[Index] &
                          TT_ADDRESS_MASK_BLOCK_ENTRY),
          Level + 1
          );
      }
    }
  }

  FreePagesSlabMmst (TranslationTable, 1);
  DEBUG ((DEBUG_ERROR, "%a:%d\n", __FUNCTION__, __LINE__));
}

STATIC
BOOLEAN
IsBlockEntry (
  IN  UINT64  Entry,
  IN  UINTN   Level
  )
{
  if (Level == 3) {
    return (Entry & TT_TYPE_MASK) == TT_TYPE_BLOCK_ENTRY_LEVEL3;
  }

  return (Entry & TT_TYPE_MASK) == TT_TYPE_BLOCK_ENTRY;
}

STATIC
BOOLEAN
IsTableEntry (
  IN  UINT64  Entry,
  IN  UINTN   Level
  )
{
  if (Level == 3) {
    //
    // TT_TYPE_TABLE_ENTRY aliases TT_TYPE_BLOCK_ENTRY_LEVEL3
    // so we need to take the level into account as well.
    //
    return FALSE;
  }

  return (Entry & TT_TYPE_MASK) == TT_TYPE_TABLE_ENTRY;
}

STATIC
EFI_STATUS
UpdateRegionMappingRecursive (
  IN  UINT64  RegionStart,
  IN  UINT64  RegionEnd,
  IN  UINT64  AttributeSetMask,
  IN  UINT64  AttributeClearMask,
  IN  UINT64  *PageTable,
  IN  UINTN   Level
  )
{
  UINTN       BlockShift;
  UINT64      BlockMask;
  UINT64      BlockEnd;
  UINT64      *Entry;
  UINT64      EntryValue;
  VOID        *TranslationTable;
  EFI_STATUS  Status;

  ASSERT (((RegionStart | RegionEnd) & EFI_PAGE_MASK) == 0);

  BlockShift = (Level + 1) * BITS_PER_LEVEL + MIN_T0SZ;
  BlockMask  = MAX_UINT64 >> BlockShift;

  DEBUG ((
    DEBUG_VERBOSE,
    "%a(%u): %llx - %llx set %lx clr %lx\n",
    __FUNCTION__,
    Level,
    RegionStart,
    RegionEnd,
    AttributeSetMask,
    AttributeClearMask
    ));

  for ( ; RegionStart < RegionEnd; RegionStart = BlockEnd) {
    BlockEnd = MIN (RegionEnd, (RegionStart | BlockMask) + 1);
    Entry    = &PageTable[(RegionStart >> (64 - BlockShift)) & (TT_ENTRY_COUNT - 1)];

    //
    // If RegionStart or BlockEnd is not aligned to the block size at this
    // level, we will have to create a table mapping in order to map less
    // than a block, and recurse to create the block or page entries at
    // the next level. No block mappings are allowed at all at level 0,
    // so in that case, we have to recurse unconditionally.
    // If we are changing a table entry and the AttributeClearMask is non-zero,
    // we cannot replace it with a block entry without potentially losing
    // attribute information, so keep the table entry in that case.
    //
    if ((Level == 0) || (((RegionStart | BlockEnd) & BlockMask) != 0) ||
        (IsTableEntry (*Entry, Level) && (AttributeClearMask != 0)))
    {
      ASSERT (Level < 3);

      if (!IsTableEntry (*Entry, Level)) {
        //
        // No table entry exists yet, so we need to allocate a page table
        // for the next level.
        //
        TranslationTable = AllocatePagesSlabMmSt (1);
        if (TranslationTable == NULL) {
          return EFI_OUT_OF_RESOURCES;
        }

        if (!ArmMmuEnabled ()) {
          //
          // Make sure we are not inadvertently hitting in the caches
          // when populating the page tables.
          //
          InvalidateDataCacheRange (TranslationTable, EFI_PAGE_SIZE);
        }

        ZeroMem (TranslationTable, EFI_PAGE_SIZE);

        if (IsBlockEntry (*Entry, Level)) {
          //
          // We are splitting an existing block entry, so we have to populate
          // the new table with the attributes of the block entry it replaces.
          //
          Status = UpdateRegionMappingRecursive (
                     RegionStart & ~BlockMask,
                     (RegionStart | BlockMask) + 1,
                     *Entry & TT_ATTRIBUTES_MASK,
                     0,
                     TranslationTable,
                     Level + 1
                     );
          if (EFI_ERROR (Status)) {
            //
            // The range we passed to UpdateRegionMappingRecursive () is block
            // aligned, so it is guaranteed that no further pages were allocated
            // by it, and so we only have to free the page we allocated here.
            //
            // FreePages (TranslationTable, 1);
            ASSERT (FALSE);
            return Status;
          }
        }
      } else {
        TranslationTable = (VOID *)(UINTN)(*Entry & TT_ADDRESS_MASK_BLOCK_ENTRY);
      }

      //
      // Recurse to the next level
      //
      Status = UpdateRegionMappingRecursive (
                 RegionStart,
                 BlockEnd,
                 AttributeSetMask,
                 AttributeClearMask,
                 TranslationTable,
                 Level + 1
                 );
      if (EFI_ERROR (Status)) {
        if (!IsTableEntry (*Entry, Level)) {
          //
          // We are creating a new table entry, so on failure, we can free all
          // allocations we made recursively, given that the whole subhierarchy
          // has not been wired into the live page tables yet. (This is not
          // possible for existing table entries, since we cannot revert the
          // modifications we made to the subhierarchy it represents.)
          //
          FreePageTablesRecursive (TranslationTable, Level + 1);
        }

        return Status;
      }

      if (!IsTableEntry (*Entry, Level)) {
        EntryValue = (UINTN)TranslationTable | TT_TYPE_TABLE_ENTRY;
        ReplaceTableEntry (
          Entry,
          EntryValue,
          RegionStart,
          IsBlockEntry (*Entry, Level)
          );
      }
    } else {
      EntryValue  = (*Entry & AttributeClearMask) | AttributeSetMask;
      EntryValue |= RegionStart;
      EntryValue |= (Level == 3) ? TT_TYPE_BLOCK_ENTRY_LEVEL3
                                 : TT_TYPE_BLOCK_ENTRY;

      if (IsTableEntry (*Entry, Level)) {
        //
        // We are replacing a table entry with a block entry. This is only
        // possible if we are keeping none of the original attributes.
        // We can free the table entry's page table, and all the ones below
        // it, since we are dropping the only possible reference to it.
        //
        ASSERT (AttributeClearMask == 0);
        TranslationTable = (VOID *)(UINTN)(*Entry & TT_ADDRESS_MASK_BLOCK_ENTRY);
        ReplaceTableEntry (Entry, EntryValue, RegionStart, TRUE);
        FreePageTablesRecursive (TranslationTable, Level + 1);
      } else {
        ReplaceTableEntry (Entry, EntryValue, RegionStart, FALSE);
      }
    }
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
UpdateRegionMapping (
  IN  UINT64  RegionStart,
  IN  UINT64  RegionLength,
  IN  UINT64  AttributeSetMask,
  IN  UINT64  AttributeClearMask
  )
{
  UINTN  T0SZ;

  if (((RegionStart | RegionLength) & EFI_PAGE_MASK) != 0) {
    return EFI_INVALID_PARAMETER;
  }

  T0SZ = ArmGetTCR () & TCR_T0SZ_MASK;

  return UpdateRegionMappingRecursive (
           RegionStart,
           RegionStart + RegionLength,
           AttributeSetMask,
           AttributeClearMask,
           ArmGetTTBR0BaseAddress (),
           GetRootTableLevel (T0SZ)
           );
}

STATIC
UINT64
GcdAttributeToPageAttribute (
  IN UINT64  GcdAttributes
  )
{
  UINT64  PageAttributes;

  switch (GcdAttributes & EFI_MEMORY_CACHETYPE_MASK) {
    case EFI_MEMORY_UC:
      PageAttributes = TT_ATTR_INDX_DEVICE_MEMORY;
      break;
    case EFI_MEMORY_WC:
      PageAttributes = TT_ATTR_INDX_MEMORY_NON_CACHEABLE;
      break;
    case EFI_MEMORY_WT:
      PageAttributes = TT_ATTR_INDX_MEMORY_WRITE_THROUGH | TT_SH_INNER_SHAREABLE;
      break;
    case EFI_MEMORY_WB:
      PageAttributes = TT_ATTR_INDX_MEMORY_WRITE_BACK | TT_SH_INNER_SHAREABLE;
      break;
    default:
      PageAttributes = TT_ATTR_INDX_MASK;
      break;
  }

  if (((GcdAttributes & EFI_MEMORY_XP) != 0) ||
      ((GcdAttributes & EFI_MEMORY_CACHETYPE_MASK) == EFI_MEMORY_UC))
  {
    if (ArmReadCurrentEL () == AARCH64_EL2) {
      PageAttributes |= TT_XN_MASK;
    } else {
      PageAttributes |= TT_UXN_MASK | TT_PXN_MASK;
    }
  }

  if ((GcdAttributes & EFI_MEMORY_RO) != 0) {
    PageAttributes |= TT_AP_NO_RO;
  }

  if ((GcdAttributes & EFI_MEMORY_RP) == 0) {
    PageAttributes |= TT_AF;
  }

  return PageAttributes;
}

/**
  Set the requested memory permission attributes on a region of memory.

  BaseAddress and Length must be aligned to EFI_PAGE_SIZE.

  If Attributes contains a memory type attribute (EFI_MEMORY_UC/WC/WT/WB), the
  region is mapped according to this memory type, and additional memory
  permission attributes (EFI_MEMORY_RP/RO/XP) are taken into account as well,
  discarding any permission attributes that are currently set for the region.
  AttributeMask is ignored in this case, and must be set to 0x0.

  If Attributes contains only a combination of memory permission attributes
  (EFI_MEMORY_RP/RO/XP), each page in the region will retain its existing
  memory type, even if it is not uniformly set across the region. In this case,
  AttributesMask may be set to a mask of permission attributes, and memory
  permissions omitted from this mask will not be updated for any page in the
  region. All attributes appearing in Attributes must appear in AttributeMask
  as well. (Attributes & ~AttributeMask must produce 0x0)

  @param[in]  BaseAddress     The physical address that is the start address of
                              a memory region.
  @param[in]  Length          The size in bytes of the memory region.
  @param[in]  Attributes      Mask of memory attributes to set.
  @param[in]  AttributeMask   Mask of memory attributes to take into account.

  @retval EFI_SUCCESS           The attributes were set for the memory region.
  @retval EFI_INVALID_PARAMETER BaseAddress or Length is not suitably aligned.
                                Invalid combination of Attributes and
                                AttributeMask.
  @retval EFI_OUT_OF_RESOURCES  Requested attributes cannot be applied due to
                                lack of system resources.

**/
EFI_STATUS
StMmSetMemoryAttributes (
  IN EFI_PHYSICAL_ADDRESS  BaseAddress,
  IN UINT64                Length,
  IN UINT64                Attributes,
  IN UINT64                AttributeMask
  )
{
  UINT64  PageAttributes;
  UINT64  PageAttributeMask;

  PageAttributes    = GcdAttributeToPageAttribute (Attributes);
  PageAttributeMask = 0;

  if ((Attributes & EFI_MEMORY_CACHETYPE_MASK) == 0) {
    //
    // No memory type was set in Attributes, so we are going to update the
    // permissions only.
    //
    PageAttributes   &= TT_AP_MASK | TT_UXN_MASK | TT_PXN_MASK | TT_AF;
    PageAttributeMask = ~(TT_ADDRESS_MASK_BLOCK_ENTRY | TT_AP_MASK |
                          TT_PXN_MASK | TT_XN_MASK | TT_AF);
    if (AttributeMask != 0) {
      if (((AttributeMask & ~(UINT64)(EFI_MEMORY_RP|EFI_MEMORY_RO|EFI_MEMORY_XP)) != 0) ||
          ((Attributes & ~AttributeMask) != 0))
      {
        DEBUG ((
          DEBUG_ERROR,
          "%a Invalid Mask Attribute 0x%lx Mask 0x%lx",
          __FUNCTION__,
          Attributes,
          AttributeMask
          ));
        ASSERT_EFI_ERROR (EFI_INVALID_PARAMETER);
        return EFI_INVALID_PARAMETER;
      }

      // Add attributes omitted from AttributeMask to the set of attributes to preserve
      PageAttributeMask |= GcdAttributeToPageAttribute (~AttributeMask) &
                           (TT_AP_MASK | TT_UXN_MASK | TT_PXN_MASK | TT_AF);
    }
  } else {
    ASSERT (AttributeMask == 0);
    if (AttributeMask != 0) {
      return EFI_INVALID_PARAMETER;
    }
  }

  return UpdateRegionMapping (
           BaseAddress,
           Length,
           PageAttributes,
           PageAttributeMask
           );
}

STATIC
EFI_STATUS
FillTranslationTable (
  IN  UINT64                             *RootTable,
  IN  STMM_ARM_MEMORY_REGION_DESCRIPTOR  *MemoryRegion
  )
{
  return UpdateRegionMapping (
           MemoryRegion->VirtualBase,
           MemoryRegion->Length,
           ArmMemoryAttributeToPageAttribute (MemoryRegion->Attributes) | TT_AF,
           0
           );
}

EFI_STATUS
EFIAPI
SlabArmConfigureMmu (
  IN  STMM_ARM_MEMORY_REGION_DESCRIPTOR  *MemoryTable,
  OUT VOID                               **TranslationTableBase OPTIONAL,
  OUT UINTN                              *TranslationTableSize OPTIONAL
  )
{
  VOID        *TranslationTable;
  UINTN       MaxAddressBits;
  UINT64      MaxAddress;
  UINTN       T0SZ;
  UINTN       RootTableEntryCount;
  UINT64      TCR;
  EFI_STATUS  Status;

  if (MemoryTable == NULL) {
    ASSERT (MemoryTable != NULL);
    return EFI_INVALID_PARAMETER;
  }

  //
  // Limit the virtual address space to what we can actually use: UEFI
  // mandates a 1:1 mapping, so no point in making the virtual address
  // space larger than the physical address space. We also have to take
  // into account the architectural limitations that result from UEFI's
  // use of 4 KB pages.
  //
  MaxAddressBits = MIN (ArmGetPhysicalAddressBits (), MAX_VA_BITS);
  MaxAddress     = LShiftU64 (1ULL, MaxAddressBits) - 1;

  T0SZ                = 64 - MaxAddressBits;
  RootTableEntryCount = GetRootTableEntryCount (T0SZ);

  //
  // Set TCR that allows us to retrieve T0SZ in the subsequent functions
  //
  // Ideally we will be running at EL2, but should support EL1 as well.
  // UEFI should not run at EL3.
  if (ArmReadCurrentEL () == AARCH64_EL2) {
    // Note: Bits 23 and 31 are reserved(RES1) bits in TCR_EL2
    TCR = T0SZ | (1UL << 31) | (1UL << 23) | TCR_TG0_4KB;

    // Set the Physical Address Size using MaxAddress
    if (MaxAddress < SIZE_4GB) {
      TCR |= TCR_PS_4GB;
    } else if (MaxAddress < SIZE_64GB) {
      TCR |= TCR_PS_64GB;
    } else if (MaxAddress < SIZE_1TB) {
      TCR |= TCR_PS_1TB;
    } else if (MaxAddress < SIZE_4TB) {
      TCR |= TCR_PS_4TB;
    } else if (MaxAddress < SIZE_16TB) {
      TCR |= TCR_PS_16TB;
    } else if (MaxAddress < SIZE_256TB) {
      TCR |= TCR_PS_256TB;
    } else {
      DEBUG ((
        DEBUG_ERROR,
        "ArmConfigureMmu: The MaxAddress 0x%lX is not supported by this MMU configuration.\n",
        MaxAddress
        ));
      ASSERT (0); // Bigger than 48-bit memory space are not supported
      return EFI_UNSUPPORTED;
    }
  } else if (ArmReadCurrentEL () == AARCH64_EL1) {
    // Due to Cortex-A57 erratum #822227 we must set TG1[1] == 1, regardless of EPD1.
    TCR = T0SZ | TCR_TG0_4KB | TCR_TG1_4KB | TCR_EPD1;

    // Set the Physical Address Size using MaxAddress
    if (MaxAddress < SIZE_4GB) {
      TCR |= TCR_IPS_4GB;
    } else if (MaxAddress < SIZE_64GB) {
      TCR |= TCR_IPS_64GB;
    } else if (MaxAddress < SIZE_1TB) {
      TCR |= TCR_IPS_1TB;
    } else if (MaxAddress < SIZE_4TB) {
      TCR |= TCR_IPS_4TB;
    } else if (MaxAddress < SIZE_16TB) {
      TCR |= TCR_IPS_16TB;
    } else if (MaxAddress < SIZE_256TB) {
      TCR |= TCR_IPS_256TB;
    } else {
      DEBUG ((
        DEBUG_ERROR,
        "ArmConfigureMmu: The MaxAddress 0x%lX is not supported by this MMU configuration.\n",
        MaxAddress
        ));
      ASSERT (0); // Bigger than 48-bit memory space are not supported
      return EFI_UNSUPPORTED;
    }
  } else {
    ASSERT (0); // UEFI is only expected to run at EL2 and EL1, not EL3.
    return EFI_UNSUPPORTED;
  }

  //
  // Translation table walks are always cache coherent on ARMv8-A, so cache
  // maintenance on page tables is never needed. Since there is a risk of
  // loss of coherency when using mismatched attributes, and given that memory
  // is mapped cacheable except for extraordinary cases (such as non-coherent
  // DMA), have the page table walker perform cached accesses as well, and
  // assert below that that matches the attributes we use for CPU accesses to
  // the region.
  //
  TCR |= TCR_SH_INNER_SHAREABLE |
         TCR_RGN_OUTER_WRITE_BACK_ALLOC |
         TCR_RGN_INNER_WRITE_BACK_ALLOC;

  // Set TCR
  ArmSetTCR (TCR);

  // Allocate pages for translation table
  TranslationTable = AllocatePagesSlabMmSt (1);
  if (TranslationTable == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // We set TTBR0 just after allocating the table to retrieve its location from
  // the subsequent functions without needing to pass this value across the
  // functions. The MMU is only enabled after the translation tables are
  // populated.
  //
  ArmSetTTBR0 (TranslationTable);

  if (TranslationTableBase != NULL) {
    *TranslationTableBase = TranslationTable;
  }

  if (TranslationTableSize != NULL) {
    *TranslationTableSize = RootTableEntryCount * sizeof (UINT64);
  }

  //
  // Make sure we are not inadvertently hitting in the caches
  // when populating the page tables.
  //
  InvalidateDataCacheRange (
    TranslationTable,
    RootTableEntryCount * sizeof (UINT64)
    );
  ZeroMem (TranslationTable, RootTableEntryCount * sizeof (UINT64));

  while (MemoryTable->Length != 0) {
    Status = FillTranslationTable (TranslationTable, MemoryTable);
    if (EFI_ERROR (Status)) {
      goto FreeTranslationTable;
    }

    MemoryTable++;
  }

  //
  // EFI_MEMORY_UC ==> MAIR_ATTR_DEVICE_MEMORY
  // EFI_MEMORY_WC ==> MAIR_ATTR_NORMAL_MEMORY_NON_CACHEABLE
  // EFI_MEMORY_WT ==> MAIR_ATTR_NORMAL_MEMORY_WRITE_THROUGH
  // EFI_MEMORY_WB ==> MAIR_ATTR_NORMAL_MEMORY_WRITE_BACK
  //
  ArmSetMAIR (
    MAIR_ATTR (TT_ATTR_INDX_DEVICE_MEMORY, MAIR_ATTR_DEVICE_MEMORY)               |
    MAIR_ATTR (TT_ATTR_INDX_MEMORY_NON_CACHEABLE, MAIR_ATTR_NORMAL_MEMORY_NON_CACHEABLE) |
    MAIR_ATTR (TT_ATTR_INDX_MEMORY_WRITE_THROUGH, MAIR_ATTR_NORMAL_MEMORY_WRITE_THROUGH) |
    MAIR_ATTR (TT_ATTR_INDX_MEMORY_WRITE_BACK, MAIR_ATTR_NORMAL_MEMORY_WRITE_BACK)
    );

  ArmDisableAlignmentCheck ();
  ArmEnableStackAlignmentCheck ();
  ArmEnableInstructionCache ();
  ArmEnableDataCache ();

  ArmEnableMmu ();
  DEBUG ((DEBUG_ERROR, "Done Configuring MMU.\n"));
  return EFI_SUCCESS;

FreeTranslationTable:
  // FreePages (TranslationTable, 1);
  ASSERT (FALSE);
  return Status;
}
