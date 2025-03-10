/**
 initial concept of DSDT patching by mackerintel

 Re-Work by Slice 2011.

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 **/

#include <Platform.h> // Only use angled for Platform, else, xcode project won't compile
#include <Efi.h>

#include "../libeg/BmLib.h"
#include "StateGenerator.h"
#include "AmlGenerator.h"
#include "AcpiPatcher.h"
#include "FixBiosDsdt.h"
#include "platformdata.h"
#include "smbios.h"
#include "cpu.h"
#include "../Settings/Self.h"
#include "../Settings/SelfOem.h"
#include "Settings.h"

#define EBDA_BASE_ADDRESS            0x40E

#define HPET_SIGN        SIGNATURE_32('H','P','E','T')
#define APIC_SIGN        SIGNATURE_32('A','P','I','C')
#define MCFG_SIGN        SIGNATURE_32('M','C','F','G')
#define ECDT_SIGN        SIGNATURE_32('E','C','D','T')
#define DMAR_SIGN        SIGNATURE_32('D','M','A','R')
#define BGRT_SIGN        SIGNATURE_32('B','G','R','T')
#define SLIC_SIGN        SIGNATURE_32('S','L','I','C')
#define APPLE_OEM_ID        { 'A', 'P', 'P', 'L', 'E', ' ' }
#define APPLE_OEM_TABLE_ID  { 'A', 'p', 'p', 'l', 'e', '0', '0', ' ' }
#define APPLE_CREATOR_ID    { 'L', 'o', 'k', 'i' }

#define IGNORE_INDEX    (~((UINTN)0)) // index ignored for matching (not ignored for >= 0)
#define AUTOMERGE_PASS1 1 // load just those that match existing entries
#define AUTOMERGE_PASS2 2 // load the rest

CONST CHAR8  oemID[6]       = APPLE_OEM_ID;
CONST CHAR8  oemTableID[8]  = APPLE_OEM_TABLE_ID;
CONST CHAR8  creatorID[4]   = APPLE_CREATOR_ID;

//Global pointers
RSDT_TABLE    *Rsdt = NULL;
XSDT_TABLE    *Xsdt = NULL;
UINTN         *XsdtReplaceSizes = NULL;

#define IndexFromEntryPtr(xsdt_or_rsdt, entry_ptr) \
((UINT32)(((CHAR8*)(entry_ptr) - (CHAR8*)&(xsdt_or_rsdt)->Entry)/sizeof((xsdt_or_rsdt)->Entry)))
#define IndexFromRsdtEntryPtr(entry_ptr) IndexFromEntryPtr(Rsdt, entry_ptr)
#define IndexFromXsdtEntryPtr(entry_ptr) IndexFromEntryPtr(Xsdt, entry_ptr)
#define TableCount(xsdt_or_rsdt) \
(((xsdt_or_rsdt)->Header.Length - sizeof(EFI_ACPI_DESCRIPTION_HEADER)) / sizeof((xsdt_or_rsdt)->Entry))
#define RsdtTableCount() TableCount(Rsdt)
#define XsdtTableCount() TableCount(Xsdt)
#define RsdtEntryPtrFromIndex(index) (&Rsdt->Entry + index)
#define XsdtEntryPtrFromIndex(index) ((UINT64*)((CHAR8*)&Xsdt->Entry + sizeof(UINT64)*(index)))
#define RsdtEntryFromIndex(index) (EFI_ACPI_DESCRIPTION_HEADER*)(UINTN)*RsdtEntryPtrFromIndex(index)
#define XsdtEntryFromIndex(index) (EFI_ACPI_DESCRIPTION_HEADER*)(UINTN)ReadUnaligned64(XsdtEntryPtrFromIndex(index))

UINT64      BiosDsdt;
UINT32      BiosDsdtLen;
UINT8       acpi_cpu_count;
CHAR8*      acpi_cpu_name[acpi_cpu_max];
UINT8       acpi_cpu_processor_id[acpi_cpu_max];
CHAR8*      acpi_cpu_score;

UINT64      machineSignature;

extern OPER_REGION *gRegions;
//-----------------------------------


UINT8 pmBlock[] = {

  /*0070: 0xA5, 0x84, 0x00, 0x00,*/ 0x01, 0x08, 0x00, 0x01, 0xF9, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  /*0080:*/ 0x06, 0x00, 0x00, 0x00, 0x00, 0xA0, 0x67, 0xBF, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x6F, 0xBF,
  /*0090:*/ 0x00, 0x00, 0x00, 0x00, 0x01, 0x20, 0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  /*00A0:*/ 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x02,
  /*00B0:*/ 0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  /*00C0:*/ 0x00, 0x00, 0x00, 0x00, 0x01, 0x08, 0x00, 0x00, 0x50, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  /*00D0:*/ 0x01, 0x20, 0x00, 0x03, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x80, 0x00, 0x01,
  /*00E0:*/ 0x20, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
  /*00F0:*/ 0x00, 0x00, 0x00, 0x00

};

// RehabMan: for stripping trailing spaces
static void stripTrailingSpaces(CHAR8* sgn)
{
  CHAR8* lastNonSpace = sgn-1;
  for (; *sgn; sgn++) {
    if (*sgn != ' ') {
      lastNonSpace = sgn;
    }
  }
  lastNonSpace[1] = 0;
}

void* FindAcpiRsdPtr()
{
  UINTN                           Address;
  UINTN                           Index;
  //
  // First Seach 0x0e0000 - 0x0fffff for RSD Ptr
  //
  for (Address = 0xe0000; Address < 0xfffff; Address += 0x10) {
    if (*(UINT64 *)(Address) == EFI_ACPI_3_0_ROOT_SYSTEM_DESCRIPTION_POINTER_SIGNATURE) {
      return (void *)Address;
    }
  }
  //
  // Search EBDA
  //
  Address = (*(UINT16 *)(UINTN)(EBDA_BASE_ADDRESS)) << 4;
  if ( Address == 0 ) return 0; // Jief : if Address==0, the first access at *(UINT64 *)(Address + Index) is at address 0. It's supposed to crash.
  for (Index = 0; Index < 0x400 ; Index += 16) {
    if (*(UINT64 *)(Address + Index) == EFI_ACPI_3_0_ROOT_SYSTEM_DESCRIPTION_POINTER_SIGNATURE) {
      return (void *)Address;
    }
  }
  return NULL;
}

UINT8 Checksum8(void* startPtr, UINT32 len)
{
  UINT8 Value = 0;
  UINT8 *ptr = (UINT8*)startPtr;
  UINT8 *endPtr = ptr + len;
  while (ptr < endPtr)
    Value += *ptr++;
  return Value;
}

void FixChecksum(EFI_ACPI_DESCRIPTION_HEADER* Table)
{
  Table->Checksum = 0;
  Table->Checksum = (UINT8)(256-Checksum8(Table, Table->Length));
}

void SaveMergedXsdtEntrySize(UINT32 Index, UINTN Size)
{
  // manage XsdtReplaceSizes (free existing, store new)
  if (XsdtReplaceSizes) {
    if (XsdtReplaceSizes[Index]) {
      // came from patched table in ACPI/patched, so free original pages
      gBS->FreePages((EFI_PHYSICAL_ADDRESS)(UINTN)XsdtEntryFromIndex(Index), XsdtReplaceSizes[Index]);
      XsdtReplaceSizes[Index] = 0;
    }
    XsdtReplaceSizes[Index] = EFI_SIZE_TO_PAGES(Size);
  }
}

XBool IsXsdtEntryMerged(UINT32 Index)
{
  if (!XsdtReplaceSizes) {
    return false;
  }
  return 0 != XsdtReplaceSizes[Index];
}

UINT32* ScanRSDT2(UINT32 Signature, UINT64 TableId, UINTN MatchIndex)
{
  if (!Rsdt || (0 == Signature && 0 == TableId)) {
    return NULL;
  }

  UINT32 Count = RsdtTableCount();
  UINTN MatchingCount = 0;
  UINT32* Ptr = RsdtEntryPtrFromIndex(0);
  UINT32* EndPtr = RsdtEntryPtrFromIndex(Count);
  for (; Ptr < EndPtr; Ptr++) {
    EFI_ACPI_DESCRIPTION_HEADER* Table = (EFI_ACPI_DESCRIPTION_HEADER*)(UINTN)*Ptr;
    if (!Table) {
      // skip NULL entry
      continue;
    }
    if (0 == Signature || Table->Signature == Signature) {
      if ((0 == TableId || Table->OemTableId == TableId) && (IGNORE_INDEX == MatchIndex || MatchingCount == MatchIndex)) {
        return Ptr; // pointer to the matching entry
      }
      ++MatchingCount;
    }
  }
  return NULL;
}


UINT32* ScanRSDT(UINT32 Signature, UINT64 TableId)
{
  return ScanRSDT2(Signature, TableId, IGNORE_INDEX);
}

UINT64* ScanXSDT2(UINT32 Signature, UINT64 TableId, UINTN MatchIndex)
{
  if (!Xsdt || (0 == Signature && 0 == TableId)) {
    return NULL;
  }

  UINT32 Count = XsdtTableCount();
  UINTN MatchingCount = 0;
  UINT64* Ptr = XsdtEntryPtrFromIndex(0);
  UINT64* EndPtr = XsdtEntryPtrFromIndex(Count);
  for (; Ptr < EndPtr; Ptr++) {
    EFI_ACPI_DESCRIPTION_HEADER* Table = (EFI_ACPI_DESCRIPTION_HEADER*)(UINTN)ReadUnaligned64(Ptr);
    if (!Table) {
      // skip NULL entry
      continue;
    }
    if (0 == Signature || Table->Signature == Signature) {
      if ((0 == TableId || Table->OemTableId == TableId) && (IGNORE_INDEX == MatchIndex || MatchingCount == MatchIndex)) {
        return Ptr; // pointer to the matching entry
      }
      ++MatchingCount;
    }
  }
  return NULL;
}

UINT64* ScanXSDT(UINT32 Signature, UINT64 TableId)
{
  return ScanXSDT2(Signature, TableId, IGNORE_INDEX);
}


void AddDropTable(EFI_ACPI_DESCRIPTION_HEADER* Table, UINT32 Index)
{
  CHAR8 sign[5], OTID[9];
  sign[4] = 0;
  OTID[8] = 0;
  CopyMem(&sign[0], &Table->Signature, 4);
  CopyMem(&OTID[0], &Table->OemTableId, 8);
  //DBG(" Found table: %s  %s len=%d\n", sign, OTID, (INT32)Table->Length);
  DBG(" - [%02d]: %s  %s len=%d\n", Index, sign, OTID, (INT32)Table->Length);
  ACPI_DROP_TABLE* DropTable = new ACPI_DROP_TABLE;
  DropTable->Signature = Table->Signature;
  DropTable->TableId = Table->OemTableId;
  DropTable->Length = Table->Length;
  DropTable->MenuItem.BValue = false;
  DropTable->Next = GlobalConfig.ACPIDropTables;
  GlobalConfig.ACPIDropTables = DropTable;
}


/*
 * There is the case when OemTableId ended by space like "TableID ".
 * We will not see the space but comparison will fail.
 */
UINT64 OemTableId_NoSpace(UINT64 origin)
{
  UINT64 mask = 0xffULL << 56;
  UINT64 space = 0x20ULL << 56;
  do {
    if ((mask & origin) == space) {
      origin &= ~mask;
    }
    mask >>= 8;
    space >>= 8;
  } while (mask != 0 && ((mask & origin) == 0 || (mask & origin) == space));
  return origin;
}



void GetAcpiTablesList()
{
  DbgHeader("GetAcpiTablesList");

  GetFadt(); //this is a first call to acpi, we need it to make a pointer to Xsdt
  GlobalConfig.ACPIDropTables = NULL;

  DBG("Get Acpi Tables List ");
/*
  //for test
  CHAR8 OTID[9];
  OTID[8] = 0;
  UINT64 TestTableId = 0x204449656c626154ULL; // <54 61 62 6c 65 49 44 20>
  CopyMem(&OTID[0], &TestTableId, 8);
  DBG("\n test          id=0x%08llx as str=%s\n", TestTableId, OTID);
  TestTableId = OemTableId_NoSpace(TestTableId);
  DBG("after convert id=0x%08llx as str=%s\n", TestTableId, OTID);

  result:
      test        id=0x204449656c626154 as str=TableID
      after convert id=0x4449656c626154 as str=TableID

*/

  if (Xsdt) {
    UINT32 Count = XsdtTableCount();
    UINT64* Ptr = XsdtEntryPtrFromIndex(0);
    UINT64* EndPtr = XsdtEntryPtrFromIndex(Count);
    DBG("from XSDT:\n");
    for (; Ptr < EndPtr; Ptr++) {
      EFI_ACPI_DESCRIPTION_HEADER* Table = (EFI_ACPI_DESCRIPTION_HEADER*)(UINTN)ReadUnaligned64(Ptr);
      if (!Table) {
        // skip NULL entry
        continue;
      }
      AddDropTable(Table, IndexFromXsdtEntryPtr(Ptr));
    }
  } else if (Rsdt) {
    DBG("from RSDT:\n");
    UINT32 Count = RsdtTableCount();
    UINT32* Ptr = RsdtEntryPtrFromIndex(0);
    UINT32* EndPtr = RsdtEntryPtrFromIndex(Count);
    for (; Ptr < EndPtr; Ptr++) {
      EFI_ACPI_DESCRIPTION_HEADER* Table = (EFI_ACPI_DESCRIPTION_HEADER*)(UINTN)*Ptr;
      if (!Table) {
        // skip NULL entry
        continue;
      }
      AddDropTable(Table, IndexFromRsdtEntryPtr(Ptr));
    }
  } else {
    DBG(": [!] Error! ACPI not found:\n");
  }
}

void DropTableFromRSDT(UINT32 Signature, UINT64 TableId, UINT32 Length)
{
  if (!Rsdt || (0 == Signature && 0 == TableId)) {
    return;
  }

  CHAR8 sign[5], OTID[9];
  sign[4] = 0;
  OTID[8] = 0;
  CopyMem(&sign[0], &Signature, 4);
  CopyMem(&OTID[0], &TableId, 8);
  DBG("Drop tables from RSDT, SIGN=%s TableID=%s Length=%d\n", sign, OTID, (INT32)Length);

  UINT32 Count = RsdtTableCount();
  //DBG(" Rsdt has tables count=%d\n", Count);
  UINT32* Ptr = RsdtEntryPtrFromIndex(0);
  UINT32* EndPtr = RsdtEntryPtrFromIndex(Count);
  for (; Ptr < EndPtr; Ptr++) {
    EFI_ACPI_DESCRIPTION_HEADER* Table = (EFI_ACPI_DESCRIPTION_HEADER*)(UINTN)*Ptr;
    if (!Table) {
      // skip NULL entry
      continue;
    }
    CopyMem(&sign[0], &Table->Signature, 4);
    CopyMem(&OTID[0], &Table->OemTableId, 8);
    //DBG(" Found table: %s  %s\n", sign, OTID);
    if (!((Signature && Table->Signature == Signature) &&
          (!TableId || OemTableId_NoSpace(Table->OemTableId) == TableId) &&
          (!Length || Table->Length == Length))) {
      continue;
    }
    if (IsXsdtEntryMerged(IndexFromXsdtEntryPtr(Ptr))) {
      DBG(" attempt to drop already merged table[%d]: %s  %s  %d ignored\n", IndexFromXsdtEntryPtr(Ptr), sign, OTID, (INT32)Table->Length);
      continue;
    }
    // drop matching table by simply replacing entry with NULL
    *Ptr = 0;
    DBG(" Table[%d]: %s  %s  %d dropped\n", IndexFromXsdtEntryPtr(Ptr), sign, OTID, (INT32)Table->Length);
  }
}

void DropTableFromXSDT(UINT32 Signature, UINT64 TableId, UINT32 Length)
{
  if (!Xsdt || (0 == Signature && 0 == TableId)) {
    return;
  }

  CHAR8 sign[5], OTID[9];
  sign[4] = 0;
  OTID[8] = 0;
  CopyMem(&sign[0], &Signature, 4);
  CopyMem(&OTID[0], &TableId, 8);
  DBG("Drop tables from XSDT, SIGN=%s TableID=%s Length=%d\n", sign, OTID, (INT32)Length);

  UINT32 Count = XsdtTableCount();
  //DBG(" Xsdt has tables count=%d\n", Count);
  UINT64* Ptr = XsdtEntryPtrFromIndex(0);
  UINT64* EndPtr = XsdtEntryPtrFromIndex(Count);
  for (; Ptr < EndPtr; Ptr++) {
    EFI_ACPI_DESCRIPTION_HEADER* Table = (EFI_ACPI_DESCRIPTION_HEADER*)(UINTN)ReadUnaligned64(Ptr);
    if (!Table) {
      // skip NULL entry
      continue;
    }
    CopyMem(&sign[0], &Table->Signature, 4);
    CopyMem(&OTID[0], &Table->OemTableId, 8);
    //DBG(" Found table: %s  %s\n", sign, OTID);
    if (!((Signature && Table->Signature == Signature) &&
          (!TableId || OemTableId_NoSpace(Table->OemTableId) == TableId) &&
          (!Length || Table->Length == Length))) {
      continue;
    }
    if (IsXsdtEntryMerged(IndexFromXsdtEntryPtr(Ptr))) {
      DBG(" attempt to drop already merged table[%d]: %s  %s  %d ignored\n", IndexFromXsdtEntryPtr(Ptr), sign, OTID, (INT32)Table->Length);
      continue;
    }
    // drop matching table by simply replacing entry with NULL
    WriteUnaligned64(Ptr, 0);
    DBG(" Table[%d]: %s  %s  %d dropped\n", IndexFromXsdtEntryPtr(Ptr), sign, OTID, (INT32)Table->Length);
  }
}


// by cecekpawon, edited by Slice, further edits by RehabMan
XBool FixAsciiTableHeader(UINT8 *Str, UINTN Len)
{
  XBool NonAscii = false;
  UINT8* StrEnd = Str + Len;
  for (; Str < StrEnd; Str++) {
    if (!*Str) continue; // NUL is allowed
    if (*Str < ' ') {
      *Str = ' ';
      NonAscii = true;
    }
    else if (*Str > 0x7e) {
      *Str = '_';
      NonAscii = true;
    }
  }
  return NonAscii;
}

XBool PatchTableHeader(EFI_ACPI_DESCRIPTION_HEADER *Header)
{
  XBool Ret1, Ret2, Ret3;
  if ( !gSettings.ACPI.FixHeaders ) {
    return false;
  }
  Ret1 = FixAsciiTableHeader((UINT8*)&Header->CreatorId, 4);
  Ret2 = FixAsciiTableHeader((UINT8*)&Header->OemTableId, 8);
  Ret3 = FixAsciiTableHeader((UINT8*)&Header->OemId, 6);
  return (Ret1 || Ret2 || Ret3);
}

void PatchAllTables()
{
  UINT32 Count = XsdtTableCount();
  UINT64* Ptr = XsdtEntryPtrFromIndex(0);
  UINT64* EndPtr = XsdtEntryPtrFromIndex(Count);
  for (; Ptr < EndPtr; Ptr++) {
    XBool Patched = false;
    EFI_ACPI_DESCRIPTION_HEADER* Table = (EFI_ACPI_DESCRIPTION_HEADER*)(UINTN)ReadUnaligned64(Ptr);
    if (!Table) {
      // skip NULL entry
      continue;
    }
    if (EFI_ACPI_3_0_FIXED_ACPI_DESCRIPTION_TABLE_SIGNATURE == Table->Signature) {
      // may be also EFI_ACPI_4_0_MULTIPLE_APIC_DESCRIPTION_TABLE_SIGNATURE?
      continue; // will be patched elsewhere
    }

    //do new table with patched header
    UINT32 Len = Table->Length;
    EFI_PHYSICAL_ADDRESS BufferPtr = EFI_SYSTEM_TABLE_MAX_ADDRESS;
    EFI_STATUS Status = gBS->AllocatePages(AllocateMaxAddress,
                                           EfiACPIReclaimMemory,
                                           EFI_SIZE_TO_PAGES(Len + 4096),
                                           &BufferPtr);
    if(EFI_ERROR(Status)) {
      //DBG(" ... not patched\n");
      continue;
    }
    EFI_ACPI_DESCRIPTION_HEADER* NewTable = (EFI_ACPI_DESCRIPTION_HEADER*)(UINTN)BufferPtr;
    CopyMem(NewTable, Table, Len);
    if ( gSettings.ACPI.FixHeaders ) {
      // Merged tables already have the header patched, so no need to do it again
      if (!IsXsdtEntryMerged(IndexFromXsdtEntryPtr(Ptr))) {
        // table header NOT already patched
        Patched = PatchTableHeader(NewTable);
      }
    }
    if (NewTable->Signature == EFI_ACPI_4_0_SECONDARY_SYSTEM_DESCRIPTION_TABLE_SIGNATURE) {
      if (gSettings.ACPI.DSDT.DSDTPatchArray.size() > 0) {
        DBG("Patching SSDTs: %zu patches each\n", gSettings.ACPI.DSDT.DSDTPatchArray.size());

//        CHAR8  OTID[9];
//        OTID[8] = 0;
//        CopyMem(OTID, &NewTable->OemTableId, 8);
//        DBG("Patching SSDT %s Length=%d\n",  OTID, (INT32)Len);

        for (UINT32 i = 0; i < gSettings.ACPI.DSDT.DSDTPatchArray.size(); i++) {
          if ( gSettings.ACPI.DSDT.DSDTPatchArray[i].PatchDsdtFind.isEmpty() ) {
            continue;
          }
//          DBG("%d. [%s]:", i, gSettings.PatchDsdtLabel[i]);
          if (!gSettings.ACPI.DSDT.DSDTPatchArray[i].PatchDsdtMenuItem.BValue) {
//            DBG(" disabled\n");
            continue;
          }
          if ( gSettings.ACPI.DSDT.DSDTPatchArray[i].PatchDsdtTgt.isEmpty() ) {
            Len = FixAny((UINT8*)NewTable, Len,
                         gSettings.ACPI.DSDT.DSDTPatchArray[i].PatchDsdtFind,
                         gSettings.ACPI.DSDT.DSDTPatchArray[i].PatchDsdtReplace,
                         gSettings.ACPI.DSDT.DSDTPatchArray[i].Skip);
            //DBG(" OK\n");
          }else{
            //DBG("Patching: renaming in bridge\n");
            Len = FixRenameByBridge2((UINT8*)NewTable, Len, gSettings.ACPI.DSDT.DSDTPatchArray[i].PatchDsdtTgt,
                                                            gSettings.ACPI.DSDT.DSDTPatchArray[i].PatchDsdtFind,
                                                            gSettings.ACPI.DSDT.DSDTPatchArray[i].PatchDsdtReplace,
                                                            gSettings.ACPI.DSDT.DSDTPatchArray[i].Skip);
          }
        }
      }
      // fixup length and checksum
      NewTable->Length = Len;
      RenameDevices((UINT8*)NewTable);
      GetBiosRegions((UINT8*)NewTable);  //take Regions from SSDT even if they will be dropped
      Patched = true;
    }
    if (NewTable->Signature == MCFG_SIGN && gSettings.ACPI.FixMCFG) {
      INTN Len1 = ((Len + 4 - 1) / 16 + 1) * 16 - 4;
      CopyMem(NewTable, Table, Len1); //Len increased but less than EFI_PAGE
      NewTable->Length = (UINT32)(UINTN)Len1;      Patched = true;
    }
    if (Patched) {
      WriteUnaligned64(Ptr, BufferPtr);
      FixChecksum(NewTable);
    }
    else {
      gBS->FreePages(BufferPtr, EFI_SIZE_TO_PAGES(Len + 4096));
    }
  }
}

EFI_STATUS InsertTable(void* TableEntry, UINTN Length)
{
  if (!TableEntry) {
    return EFI_NOT_FOUND;
  }

  EFI_PHYSICAL_ADDRESS BufferPtr = EFI_SYSTEM_TABLE_MAX_ADDRESS;
  EFI_STATUS Status = gBS->AllocatePages (
                                          AllocateMaxAddress,
                                          EfiACPIReclaimMemory,
                                          EFI_SIZE_TO_PAGES(Length),
                                          &BufferPtr
                                          );
  //if success insert table pointer into ACPI tables
  if(!EFI_ERROR(Status)) {
    //      DBG("page is allocated, write SSDT into\n");
    EFI_ACPI_DESCRIPTION_HEADER* TableHeader = (EFI_ACPI_DESCRIPTION_HEADER*)(UINTN)BufferPtr;
    CopyMem(TableHeader, TableEntry, Length);
    // Now is a good time to fix the table header and checksum
    PatchTableHeader(TableHeader);
    FixChecksum(TableHeader);

    //insert into RSDT
    if (Rsdt) {
      UINT32* Ptr = RsdtEntryPtrFromIndex(RsdtTableCount());
      *Ptr = (UINT32)(UINTN)BufferPtr;
      Rsdt->Header.Length += sizeof(UINT32);
      //DBG("Rsdt->Length = %d\n", Rsdt->Header.Length);
    }
    //insert into XSDT
    if (Xsdt) {
      UINT64* Ptr = XsdtEntryPtrFromIndex(XsdtTableCount());
      WriteUnaligned64(Ptr, BufferPtr);
      Xsdt->Header.Length += sizeof(UINT64);
      //DBG("Xsdt->Length = %d\n", Xsdt->Header.Length);
    }
  }
  return Status;
}

UINTN IndexFromFileName(CONST CHAR16* FileName)
{
  // FileName must be as "XXXX-number-..." or "XXXX-number.aml", such as "SSDT-9.aml", or "SSDT-11-SaSsdt.aml"
  // But just checking for '-' or '.' following the number.

  // search for '-'
  UINTN Result = IGNORE_INDEX;
  CONST CHAR16* temp = FileName;
  for (; *temp != 0 && *temp != '-'; temp++);
  if ('-' == *temp && 4 == temp-FileName) {
    ++temp;
    if (*temp >= '0' && *temp <= '9') {
      Result = 0;
      for (; *temp >= '0' && *temp <= '9'; temp++) {
        Result *= 10;
        Result += *temp - '0';
      }
      // a FileName such as "SSDT-4x30s.aml" is not considered as "SSDT-4.aml"
      if ('.' != *temp && '-' != *temp)
        Result = IGNORE_INDEX;
    }
  }
  return Result;
}

EFI_STATUS ReplaceOrInsertTable(void* TableEntry, UINTN Length, UINTN MatchIndex, INTN Pass)
{
  if (!TableEntry) {
    return EFI_NOT_FOUND;
  }

  EFI_PHYSICAL_ADDRESS BufferPtr  = EFI_SYSTEM_TABLE_MAX_ADDRESS;
  EFI_STATUS Status = gBS->AllocatePages (
                                          AllocateMaxAddress,
                                          EfiACPIReclaimMemory,
                                          EFI_SIZE_TO_PAGES(Length),
                                          &BufferPtr
                                          );
  EFI_ACPI_DESCRIPTION_HEADER* hdr = (EFI_ACPI_DESCRIPTION_HEADER*)TableEntry;

  //if success insert or replace table pointer into ACPI tables
  if(!EFI_ERROR(Status)) {
    Status = EFI_ABORTED;
    //DBG("page is allocated, write SSDT into\n");
    EFI_ACPI_DESCRIPTION_HEADER* TableHeader = (EFI_ACPI_DESCRIPTION_HEADER*)(UINTN)BufferPtr;
    CopyMem(TableHeader, TableEntry, Length);
#if 0 //REVIEW: seems as if Rsdt is always NULL for ReplaceOrInsertTable scenarios (macOS/OS X)
    //insert/modify into RSDT
    if (Rsdt) {
      UINT32* Ptr = NULL;
      if (hdr->Signature != EFI_ACPI_4_0_SECONDARY_SYSTEM_DESCRIPTION_TABLE_SIGNATURE || MatchIndex != IGNORE_INDEX) {
        // SSDT with target index or non-SSDT, try to find matching entry
        Ptr = ScanRSDT2(hdr->Signature, hdr->OemTableId, MatchIndex);
      }
      if (Ptr) {
        *Ptr = (UINT32)(UINTN)BufferPtr;
        Status = EFI_SUCCESS;
      } else if (AUTOMERGE_PASS2 == Pass) {
        Ptr = RsdtEntryPtrFromIndex(RsdtTableCount());
        *Ptr = (UINT32)(UINTN)BufferPtr;
        Rsdt->Header.Length += sizeof(UINT32);
        //DBG("Rsdt->Length = %d\n", Rsdt->Header.Length);
        Status = EFI_SUCCESS;
      }
    }
#endif
    //insert/modify into XSDT
    if (Xsdt) {
      UINT64* Ptr = NULL;
      if (hdr->Signature != EFI_ACPI_4_0_SECONDARY_SYSTEM_DESCRIPTION_TABLE_SIGNATURE || MatchIndex != IGNORE_INDEX) {
        // SSDT with target index or non-SSDT, try to find matching entry
        Ptr = ScanXSDT2(hdr->Signature, hdr->OemTableId, MatchIndex);
      }
      // Now is a good time to fix the table header and checksum (*MUST* be done after matching)
      PatchTableHeader(TableHeader);
      FixChecksum(TableHeader);
      if (Ptr) {
        UINT32 Index = IndexFromXsdtEntryPtr(Ptr);
		  DBG("@%llu ", (UINT64)Index);
        // keep track of new table size in case it needs to be freed later
        SaveMergedXsdtEntrySize(Index, Length);
        WriteUnaligned64(Ptr, BufferPtr);
        Status = EFI_SUCCESS;
      } else if (AUTOMERGE_PASS2 == Pass) {
        Ptr = XsdtEntryPtrFromIndex(XsdtTableCount());
        WriteUnaligned64(Ptr, BufferPtr);
        Xsdt->Header.Length += sizeof(UINT64);
        //DBG("Xsdt->Length = %d\n", Xsdt->Header.Length);
        Status = EFI_SUCCESS;
      }
    }
  }
  return Status;
}

void PreCleanupRSDT()
{
  if (!Rsdt) {
    return;
  }

  //REVIEW: really?
  // Если адрес RSDT < адреса XSDT и хвост RSDT наползает на XSDT, то подрезаем хвост RSDT до начала XSDT
  // English: If the RSDT address of the XSDT address and the tail of the RSDT crawls onto the XSDT, then we
  // trim the RSDT tail before the XSDT starts
  if ((UINTN)Rsdt < (UINTN)Xsdt && (UINTN)Rsdt + Rsdt->Header.Length > (UINTN)Xsdt) {
    UINTN v = ((UINTN)Xsdt - (UINTN)Rsdt) & ~3;
//    if ( v > MAX_UINT32 ) panic("((UINTN)Xsdt - (UINTN)Rsdt) & ~3 > MAX_UINT32");
    Rsdt->Header.Length = (UINT32)v;
    DBG("Cropped Rsdt->Header.Length=%d\n", (UINT32)Rsdt->Header.Length);
  }

  //REVIEW: why?
  // terminate RSDT table at first double zero, if present
  UINT32 Count = RsdtTableCount();
  if (Count <= 2) {
    return;
  }

  DBG("PreCleanup RSDT: count=%d, length=%d\n", Count, (UINT32)Rsdt->Header.Length);
  UINT32* Ptr = RsdtEntryPtrFromIndex(0);
  UINT32* EndPtr = RsdtEntryPtrFromIndex(Count-1);
  for (; Ptr < EndPtr; Ptr++) {
    if (0 == Ptr[0] && 0 == Ptr[1]) {
      // double zero found, terminate RSDT entry table here
      DBG("DoubleZero in RSDT table\n");
      Rsdt->Header.Length = (UINT32)((CHAR8*)Ptr - (CHAR8*)Rsdt);
      break;
    }
  }
  DBG("PreCleanup RSDT, corrected RSDT: count=%d, length=%d\n", Count, (UINT32)Rsdt->Header.Length);
}

void PostCleanupRSDT()
{
  if (!Rsdt) {
    return;
  }

  // remove NULL entries from RSDT table
  UINT32 Count = RsdtTableCount();
  DBG("Cleanup RSDT: count=%d, length=%d\n", Count, (UINT32)Rsdt->Header.Length);
  UINT32* Source = RsdtEntryPtrFromIndex(0);
  UINT32* Dest = Source;
  UINT32* EndPtr = RsdtEntryPtrFromIndex(Count);
  while (Source < EndPtr) {
    if (0 == *Source) {
      // skip NULL entry
      Source++;
      continue;
    }
    *Dest++ = *Source++;
  }
  // fix header length
  Rsdt->Header.Length = (UINT32)((CHAR8*)Dest - (CHAR8*)Rsdt);
  Count = RsdtTableCount();
  DBG("corrected RSDT: count=%d, length=%d\n", Count, (UINT32)Rsdt->Header.Length);
  FixChecksum(&Rsdt->Header);
}

void PreCleanupXSDT()
{
  UINT64 *Ptr, *EndPtr;
  if (!Xsdt) {
    return;
  }

  //REVIEW: why?
  // terminate RSDT table at first double zero, if present
  UINT32 Count = XsdtTableCount();
  if (Count <= 2) {
    return;
  }
  DBG("PreCleanup XSDT: count=%d, length=%d\n", Count, (UINT32)Xsdt->Header.Length);
  Ptr = XsdtEntryPtrFromIndex(0);
  EndPtr = XsdtEntryPtrFromIndex(Count-1);
  for (; Ptr < EndPtr; Ptr++) {
    if (0 == ReadUnaligned64(Ptr+0) && 0 == ReadUnaligned64(Ptr+1)) {
      // double zero found, terminate XSDT entry table here
      DBG("DoubleZero in XSDT table\n");
      Xsdt->Header.Length = (UINT32)((CHAR8*)Ptr - (CHAR8*)Xsdt);
      break;
    }
  }
  DBG("PreCleanup XSDT, corrected XSDT: count=%d, length=%d\n", Count, (UINT32)Xsdt->Header.Length);
}

void PostCleanupXSDT()
{
  UINT64 *Dest, *EndPtr, *Source;
  if (!Xsdt) {
    return;
  }

  // remove NULL entries from XSDT table
  UINT32 Count = XsdtTableCount();
  DBG("Cleanup XSDT: count=%d, length=%d\n", Count, (UINT32)Xsdt->Header.Length);
  Source = XsdtEntryPtrFromIndex(0);
  Dest = Source;
  EndPtr = XsdtEntryPtrFromIndex(Count);
  while (Source < EndPtr) {
    if (0 == *Source) {
      // skip NULL entry
      Source++;
      continue;
    }
    WriteUnaligned64(Dest++, ReadUnaligned64(Source++));
  }
  // fix header length
  Xsdt->Header.Length = (UINT32)((CHAR8*)Dest - (CHAR8*)Xsdt);
  Count = XsdtTableCount();
  DBG("corrected XSDT count=%d, length=%d\n", Count, (UINT32)Xsdt->Header.Length);
  FixChecksum(&Xsdt->Header);
}


/** Saves Buffer of Length to disk as OemDir\\DirName\\FileName. */
EFI_STATUS SaveBufferToDisk(void *Buffer, UINTN Length, CONST CHAR16 *DirName, CONST CHAR16 *FileName)
{
  if (DirName == NULL || FileName == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  XStringW PathName = SWPrintf("%ls\\%ls", DirName, FileName);

  EFI_STATUS Status = egSaveFile(&selfOem.getConfigDir(), PathName.wc_str(), Buffer, Length);
  // Do not write outside OemDir
//  if (EFI_ERROR(Status)) {
//    Status = egSaveFile(NULL, PathName.wc_str(), Buffer, Length);
//  }
  return Status;
}

//
// Remembering saved tables
//
#define SAVED_TABLES_ALLOC_ENTRIES  64
void   **mSavedTables = NULL;
UINTN   mSavedTablesEntries = 0;
UINTN   mSavedTablesNum = 0;

/** Returns true is TableEntry is already saved. */
XBool IsTableSaved(void *TableEntry)
{
  UINTN   Index;

  if (mSavedTables != NULL) {
    for (Index = 0; Index < mSavedTablesNum; Index++) {
      if (mSavedTables[Index] == TableEntry) {
        return true;
      }
    }
  }
  return false;
}

/** Adds TableEntry to mSavedTables if not already there. */
void MarkTableAsSaved(void *TableEntry)
{
  //
  // If mSavedTables does not exists yet - allocate it
  //
  if (mSavedTables == NULL) {
    //DBG(" Allocaing mSavedTables");
    mSavedTablesEntries = SAVED_TABLES_ALLOC_ENTRIES;
    mSavedTablesNum = 0;
    mSavedTables = (__typeof__(mSavedTables))AllocateZeroPool(sizeof(*mSavedTables) * mSavedTablesEntries);
    if (mSavedTables == NULL) {
      return;
    }
  }

  //
  // If TableEntry is not in mSavedTables - add it
  //
  //DBG(" MarkTableAsSaved %llx", TableEntry);
  if (IsTableSaved(TableEntry)) {
    // already saved
    //DBG(" - already saved\n");
    return;
  }

  //
  // If mSavedTables is full - extend it
  //
  if (mSavedTablesNum + 1 >= mSavedTablesEntries) {
    // not enough space
    //DBG(" - extending mSavedTables from %d", mSavedTablesEntries);
    mSavedTables = (__typeof__(mSavedTables))ReallocatePool(
                                  sizeof(*mSavedTables) * mSavedTablesEntries,
                                  sizeof(*mSavedTables) * (mSavedTablesEntries + SAVED_TABLES_ALLOC_ENTRIES),
                                  mSavedTables
                                  );
    if (mSavedTables == NULL) {
      return;
    }
    mSavedTablesEntries = mSavedTablesEntries + SAVED_TABLES_ALLOC_ENTRIES;
    //DBG(" to %d", mSavedTablesEntries);
  }

  //
  // Add TableEntry to mSavedTables
  //
  mSavedTables[mSavedTablesNum] = TableEntry;
  //DBG(" - added to index %d\n", mSavedTablesNum);
  mSavedTablesNum++;
}

#define AML_OP_NAME    0x08
#define AML_OP_PACKAGE 0x12

STATIC CHAR8 NameSSDT[] = {AML_OP_NAME, 'S', 'S', 'D', 'T', AML_OP_PACKAGE};
STATIC CHAR8 NameCSDT[] = {AML_OP_NAME, 'C', 'S', 'D', 'T', AML_OP_PACKAGE};
STATIC CHAR8 NameTSDT[] = {AML_OP_NAME, 'T', 'S', 'D', 'T', AML_OP_PACKAGE};

// OperationRegion (SSDT, SystemMemory, 0xDF5DAC18, 0x038C)
STATIC UINT8 NameSSDT2[] = {0x80, 0x53, 0x53, 0x44, 0x54};
// OperationRegion (CSDT, SystemMemory, 0xDF5DBE18, 0x84)
STATIC UINT8 NameCSDT2[] = {0x80, 0x43, 0x53, 0x44, 0x54};

//UINT32 get_size(UINT8 * An, UINT32 ); // Let borrow from FixBiosDsdt.

static XStringW GenerateFileName(CONST CHAR16* FileNamePrefix, UINTN SsdtCount, UINTN ChildCount, CHAR8 OemTableId[9])
// ChildCount == IGNORE_INDEX indicates normal SSDT
// SsdtCount == IGNORE_INDEX indicates dynamic SSDT in DSDT
// otherwise is child SSDT from normal SSDT
{
  XStringW FileName;
  CHAR8 Suffix[10]; // "-" + OemTableId + NUL
  if (gSettings.ACPI.SSDT.NoOemTableId || 0 == OemTableId[0]) {
    Suffix[0] = 0;
  } else {
    Suffix[0] = '-';
    CopyMem(Suffix+1, OemTableId, 9);
  }
  if (IGNORE_INDEX == ChildCount) {
    // normal SSDT
    FileName = SWPrintf("%lsSSDT-%llu%s.aml", FileNamePrefix, SsdtCount, Suffix);
  } else if (IGNORE_INDEX == SsdtCount) {
    // dynamic SSDT in DSDT
    FileName = SWPrintf("%lsSSDT-xDSDT_%llu%s.aml", FileNamePrefix, ChildCount, Suffix);
  } else {
    // dynamic SSDT in static SSDT
    FileName = SWPrintf("%lsSSDT-x%llu_%llu%s.aml", FileNamePrefix, SsdtCount, ChildCount, Suffix);
  }
  return FileName;
}

void DumpChildSsdt(EFI_ACPI_DESCRIPTION_HEADER *TableEntry, CONST CHAR16 *DirName, CONST CHAR16 *FileNamePrefix, UINTN SsdtCount)
{
  EFI_STATUS    Status = EFI_SUCCESS;
  INTN          j, k, pacLen, pacCount;
  CHAR8         Signature[5];
  CHAR8         OemTableId[9];
  UINTN         adr, len;
  UINT8         *Entry;
  UINT8         *End;
  UINT8         *pacBody;
  INTN          ChildCount = 0;

  if (gSettings.ACPI.SSDT.NoDynamicExtract) {
    return;
  }

  Entry = (UINT8*)TableEntry;  //first entry is parent SSDT
  End = Entry + TableEntry->Length;
  while (Entry < End) {

    if ((CompareMem(Entry, NameSSDT, sizeof (NameSSDT)) == 0) ||
        (CompareMem(Entry, NameCSDT, sizeof (NameCSDT)) == 0) ||
        (CompareMem(Entry, NameTSDT, sizeof (NameTSDT)) == 0)) {
      pacLen = get_size(Entry, sizeof (NameSSDT));

      pacBody = Entry + sizeof (NameSSDT) + (pacLen > 63 ? 2 : 1); // Our packages are not huge
      pacCount = *pacBody++;

      if (pacCount > 0 && pacCount % 3 == 0) {
        pacCount /= 3;
		    DBG(" (Found hidden SSDT %lld pcs)\n", pacCount);

        while (pacCount-- > 0) {
          // Skip text marker and addr type tag
          pacBody += 1 + 8 + 1 + 1;

          adr = ReadUnaligned32((UINT32*)(pacBody));
          len = 0;
          pacBody += 4;

          if (*pacBody == AML_CHUNK_DWORD) {
            len = ReadUnaligned32((UINT32*)(pacBody + 1));
            pacBody += 5;
          } else if (*pacBody == AML_CHUNK_WORD) {
            len = ReadUnaligned16((UINT16*)(pacBody + 1));
            pacBody += 3;
          }

          // Take Signature and OemId for printing
          CopyMem(&Signature[0], &((EFI_ACPI_DESCRIPTION_HEADER *)adr)->Signature, 4);
          Signature[4] = 0;
          CopyMem(&OemTableId[0], &((EFI_ACPI_DESCRIPTION_HEADER *)adr)->OemTableId, 8);
          OemTableId[8] = 0;
          stripTrailingSpaces(OemTableId);
          int innLen = ((EFI_ACPI_DESCRIPTION_HEADER *)adr)->Length;
          if (innLen < 0 || innLen > 0x20000) break;
			    DBG("      * %llu: '%s', '%s', Rev: %d, Len: %d  ", adr, Signature, OemTableId,
              ((EFI_ACPI_DESCRIPTION_HEADER *)adr)->Revision, innLen);
          for (k = 0; k < 16; k++) {
            DBG("%02hhX ", ((UINT8*)adr)[k]);
          }
          if ((AsciiStrCmp(Signature, "SSDT") == 0) && (len < 0x20000) && DirName != NULL && !IsTableSaved((void*)adr)) {
            XStringW FileName = GenerateFileName(FileNamePrefix, SsdtCount, ChildCount, OemTableId);
            len = ((UINT16*)adr)[2];
			      DBG("Internal length = %llu", len);
            Status = SaveBufferToDisk((void*)adr, len, DirName, FileName.wc_str());
            if (!EFI_ERROR(Status)) {
              DBG(" -> %ls", FileName.wc_str());
              MarkTableAsSaved((void*)adr);
              ChildCount++;
            } else {
              DBG(" -> %s", efiStrError(Status));
            }
          }
          DBG("\n");
        }
      }
      Entry += sizeof (NameSSDT) + pacLen;
    } else if (CompareMem(Entry, NameSSDT2, 5) == 0 ||
               CompareMem(Entry, NameCSDT2, 5) == 0) {

      adr = ReadUnaligned32((UINT32*)(Entry + 7));
      len = 0;
      j = *(Entry + 11);
      if (j == 0x0b) {
        len = ReadUnaligned16((UINT16*)(Entry + 12));
      } else if (j == 0x0a) {
        len = *(Entry + 12);
      } else {
        //not a number so skip for security
        Entry += 5;
        continue;
      }

      if (len > 0) {
        // Take Signature and OemId for printing
        CopyMem(&Signature, &((EFI_ACPI_DESCRIPTION_HEADER *)adr)->Signature, 4);
        Signature[4] = 0;
        CopyMem(&OemTableId, &((EFI_ACPI_DESCRIPTION_HEADER *)adr)->OemTableId, 8);
        OemTableId[8] = 0;
        stripTrailingSpaces(OemTableId);
		  DBG("      * %llu: '%s', '%s', Rev: %d, Len: %d  ", adr, Signature, OemTableId,
            ((EFI_ACPI_DESCRIPTION_HEADER *)adr)->Revision, ((EFI_ACPI_DESCRIPTION_HEADER *)adr)->Length);
        for(k=0; k<16; k++){
          DBG("%02hhX ", ((UINT8*)adr)[k]);
        }
        if ((AsciiStrCmp(Signature, "SSDT") == 0) && (len < 0x20000) && DirName != NULL && !IsTableSaved((void*)adr)) {
          XStringW FileName = GenerateFileName(FileNamePrefix, SsdtCount, ChildCount, OemTableId);
          Status = SaveBufferToDisk((void*)adr, len, DirName, FileName.wc_str());
          if (!EFI_ERROR(Status)) {
            DBG(" -> %ls", FileName.wc_str());
            MarkTableAsSaved((void*)adr);
            ChildCount++;
          } else {
            DBG(" -> %s", efiStrError(Status));
          }
        }
        DBG("\n");
      }
      Entry += 5;
    } else {
      Entry++;
    }
  }
}

/** Saves Table to disk as DirName\\FileName (DirName != NULL)
 *  or just prints basic table data to log (DirName == NULL).
 */
EFI_STATUS DumpTable(EFI_ACPI_DESCRIPTION_HEADER *TableEntry, CONST CHAR8 *CheckSignature, CONST CHAR16 *DirName, const XStringW& FileName, CONST CHAR16 *FileNamePrefix, INTN *SsdtCount)
{
  EFI_STATUS    Status;
  CHAR8         Signature[5];
  CHAR8         OemTableId[9];

  // Take Signature and OemId for printing
  CopyMem(&Signature[0], &TableEntry->Signature, 4);
  Signature[4] = 0;
  CopyMem(&OemTableId[0], &TableEntry->OemTableId, 8);
  OemTableId[8] = 0;
  stripTrailingSpaces(OemTableId);

  DBG(" %llx: '%s', '%s', Rev: %d, Len: %d", (uintptr_t)TableEntry, Signature, OemTableId, TableEntry->Revision, TableEntry->Length);

  //
  // Additional checks
  //
  if (CheckSignature != NULL && AsciiStrCmp(Signature, CheckSignature) != 0) {
    DBG(" -> invalid signature, expecting %s\n", CheckSignature);
    return EFI_INVALID_PARAMETER;
  }
  // XSDT checks
  if (TableEntry->Signature == EFI_ACPI_2_0_EXTENDED_SYSTEM_DESCRIPTION_TABLE_SIGNATURE) {
    if (TableEntry->Length < sizeof(XSDT_TABLE)) {
      DBG(" -> invalid length\n");
      return EFI_INVALID_PARAMETER;
    }
  }
  // RSDT checks
  if (TableEntry->Signature == EFI_ACPI_1_0_ROOT_SYSTEM_DESCRIPTION_TABLE_SIGNATURE) {
    if (TableEntry->Length < sizeof(RSDT_TABLE)) {
      DBG(" -> invalid length\n");
      return EFI_INVALID_PARAMETER;
    }
  }
  // FADT/FACP checks
  if (TableEntry->Signature == EFI_ACPI_1_0_FIXED_ACPI_DESCRIPTION_TABLE_SIGNATURE) {
    if (TableEntry->Length < sizeof(EFI_ACPI_1_0_FIXED_ACPI_DESCRIPTION_TABLE)) {
      DBG(" -> invalid length\n");
      return EFI_INVALID_PARAMETER;
    }
  }
  // DSDT checks
  if (TableEntry->Signature == EFI_ACPI_1_0_DIFFERENTIATED_SYSTEM_DESCRIPTION_TABLE_SIGNATURE) {
    if (TableEntry->Length < sizeof(EFI_ACPI_DESCRIPTION_HEADER)) {
      DBG(" -> invalid length\n");
      return EFI_INVALID_PARAMETER;
    }
  }
  // SSDT checks
  if (TableEntry->Signature == EFI_ACPI_1_0_SECONDARY_SYSTEM_DESCRIPTION_TABLE_SIGNATURE) {
    if (TableEntry->Length < sizeof(EFI_ACPI_DESCRIPTION_HEADER)) {
      DBG(" -> invalid length\n");
      return EFI_INVALID_PARAMETER;
    }
  }

  if (DirName == NULL || IsTableSaved(TableEntry)) {
    // just debug log dump
    return EFI_SUCCESS;
  }

  if (FileNamePrefix == NULL) {
    FileNamePrefix = L"";
  }

  XStringW ReleaseFileName = FileName;
  if (ReleaseFileName.isEmpty()) {
    // take the name from the signature
    if (TableEntry->Signature == EFI_ACPI_1_0_SECONDARY_SYSTEM_DESCRIPTION_TABLE_SIGNATURE && SsdtCount != NULL) {
      ReleaseFileName = GenerateFileName(FileNamePrefix, *SsdtCount, IGNORE_INDEX, OemTableId);
    } else {
      ReleaseFileName = SWPrintf("%ls%s.aml", FileNamePrefix, Signature);
    }
  }
  DBG(" -> %ls", ReleaseFileName.wc_str());

  // Save it
  Status = SaveBufferToDisk(TableEntry, TableEntry->Length, DirName, ReleaseFileName.wc_str());
  MarkTableAsSaved(TableEntry);

  if (TableEntry->Signature == EFI_ACPI_1_0_SECONDARY_SYSTEM_DESCRIPTION_TABLE_SIGNATURE && SsdtCount != NULL) {
    DumpChildSsdt(TableEntry, DirName, FileNamePrefix, *SsdtCount);
    *SsdtCount += 1;
  }

  return Status;
}

/** Saves to disk (DirName != NULL) or prints to log (DirName == NULL) Fadt tables: Dsdt and Facs. */
EFI_STATUS DumpFadtTables(EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *Fadt, CONST CHAR16 *DirName, CONST CHAR16 *FileNamePrefix, INTN *SsdtCount)
{
  EFI_ACPI_DESCRIPTION_HEADER                   *TableEntry;
  EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE  *Facs;
  EFI_STATUS    Status  = EFI_SUCCESS;
  UINT64        DsdtAdr;
  UINT64        FacsAdr;
  CHAR8         Signature[5];

  //
  // if Fadt->Revision < 3 (EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE_REVISION), then it is Acpi 1.0
  // and fields after Flags are not available
  //
  DBG("      (Dsdt: %X, Facs: %X", Fadt->Dsdt, Fadt->FirmwareCtrl);
  // for Acpi 1.0
  DsdtAdr = Fadt->Dsdt;
  FacsAdr = Fadt->FirmwareCtrl;

  if (Fadt->Header.Revision >= EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE_REVISION) {
    // Acpi 2.0 or up
    // may have it in XDsdt or XFirmwareCtrl
	  DBG(", XDsdt: %llx, XFacs: %llx", Fadt->XDsdt, Fadt->XFirmwareCtrl);
    if ((Fadt->XDsdt != 0)  && (Fadt->XDsdt < 0xFFFFFFF0ull)){
      DsdtAdr = Fadt->XDsdt;
    }
    if (Fadt->XFirmwareCtrl != 0) {
      FacsAdr = Fadt->XFirmwareCtrl;
    }
  }
  DBG(")\n");

  //
  // Save Dsdt
  //
  if (DsdtAdr != 0) {
    DBG("     ");
    TableEntry = (EFI_ACPI_DESCRIPTION_HEADER*)(UINTN)DsdtAdr;
    Status = DumpTable(TableEntry, "DSDT", DirName,  L""_XSW, FileNamePrefix, NULL);
    if (EFI_ERROR(Status)) {
      DBG(" - %s\n", efiStrError(Status));
      return Status;
    }
    DBG("\n");
    DumpChildSsdt(TableEntry, DirName, FileNamePrefix, IGNORE_INDEX);
  }
  //
  // Save Facs
  //
  if (FacsAdr != 0) {
    // Taking it as structure from Acpi 2.0 just to get Version (it's reserved field in Acpi 1.0 and == 0)
    Facs = (EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE*)(UINTN)FacsAdr;
    // Take Signature for printing
    CopyMem(&Signature[0], &Facs->Signature, 4);
    Signature[4] = 0;
    DBG("      %llx: '%s', Ver: %d, Len: %d", (uintptr_t)Facs, Signature, Facs->Version, Facs->Length);

    // FACS checks
    if (Facs->Signature != EFI_ACPI_1_0_FIRMWARE_ACPI_CONTROL_STRUCTURE_SIGNATURE) {
      DBG(" -> invalid signature, expecting FACS\n");
      return EFI_INVALID_PARAMETER;
    }
    if (Facs->Length < sizeof(EFI_ACPI_1_0_FIRMWARE_ACPI_CONTROL_STRUCTURE)) {
      DBG(" -> invalid length\n");
      return EFI_INVALID_PARAMETER;
    }

    if (DirName != NULL && !IsTableSaved(Facs)) {
      XStringW FileName = SWPrintf("%lsFACS.aml", FileNamePrefix);
      DBG(" -> %ls", FileName.wc_str());
      Status = SaveBufferToDisk(Facs, Facs->Length, DirName, FileName.wc_str());
      MarkTableAsSaved(Facs);
      if (EFI_ERROR(Status)) {
        DBG(" - %s\n", efiStrError(Status));
        return Status;
      }
    }
    DBG("\n");
  }

  return Status;
}

EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE* GetFadt()
{
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  *RsdPtr;
  EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE     *FadtPointer = NULL;

  //  EFI_STATUS      Status;

  RsdPtr = (__typeof__(RsdPtr))FindAcpiRsdPtr();
  if (RsdPtr == NULL) {
    /*Status = */EfiGetSystemConfigurationTable (&gEfiAcpi20TableGuid, (void **)&RsdPtr);
    if (RsdPtr == NULL) {
      /*Status = */EfiGetSystemConfigurationTable (&gEfiAcpi10TableGuid, (void **)&RsdPtr);
      if (RsdPtr == NULL) {
        return NULL;
      }
    }
  }
  Rsdt = (RSDT_TABLE*)(UINTN)(RsdPtr->RsdtAddress);
  if (RsdPtr->Revision > 0) {
    if (Rsdt == NULL || Rsdt->Header.Signature != EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_TABLE_SIGNATURE) {
      Xsdt = (XSDT_TABLE *)(UINTN)(RsdPtr->XsdtAddress);
    }
  }
  if (Rsdt == NULL && Xsdt == NULL) {
    //
    // Search Acpi 2.0 or newer in UEFI Sys.Tables
    //
    RsdPtr = NULL;
    /*Status = */EfiGetSystemConfigurationTable (&gEfiAcpi20TableGuid, (void**)&RsdPtr);
    if (RsdPtr != NULL) {
      DBG("Found UEFI Acpi 2.0 RSDP at %llx\n", (uintptr_t)RsdPtr);
      Rsdt = (RSDT_TABLE*)(UINTN)(RsdPtr->RsdtAddress);
      if (RsdPtr->Revision > 0) {
        if (Rsdt == NULL || Rsdt->Header.Signature != EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_TABLE_SIGNATURE) {
          Xsdt = (XSDT_TABLE *)(UINTN)(RsdPtr->XsdtAddress);
        }
      }
    }
    if (Rsdt == NULL && Xsdt == NULL) {
      DBG("No RSDT or XSDT found!\n");
      return NULL;
    }
  }

  if (Rsdt) {
    FadtPointer = (EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE*)(UINTN)(Rsdt->Entry);
  }
  if (Xsdt) {
    //overwrite previous find as xsdt priority
    FadtPointer = (EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE*)(UINTN)(Xsdt->Entry);
  }
  return FadtPointer;
}

/** Saves to disk (DirName != NULL)
 *  or prints to debug log (DirName == NULL)
 *  ACPI tables given by RsdPtr.
 *  Takes tables from Xsdt if present or from Rsdt if Xsdt is not present.
 */
void DumpTables(void *RsdPtrVoid, CONST CHAR16 *DirName)
{
  EFI_STATUS      Status;
  UINTN           Length;
  INTN            SsdtCount;
  CONST CHAR16          *FileNamePrefix;
  //
  // RSDP
  // Take it as Acpi 2.0, but take care that if RsdPtr->Revision == 0
  // then it is actually Acpi 1.0 and fields after RsdtAddress (like XsdtAddress)
  // are not available
  //
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER* RsdPtr = (EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER*)RsdPtrVoid;
  if (DirName != NULL) {
    DBG("Saving ACPI tables from RSDP %llx to %ls ...\n", (uintptr_t)RsdPtr, DirName);
  } else {
    DBG("Printing ACPI tables from RSDP %llx ...\n", (uintptr_t)RsdPtr);
  }

  if (RsdPtr->Signature != EFI_ACPI_1_0_ROOT_SYSTEM_DESCRIPTION_POINTER_SIGNATURE) {
	  DBG(" RsdPrt at %llx has invaid signature 0x%llx - exiting.\n", (uintptr_t)RsdPtr, RsdPtr->Signature);
    return;
  }

  // Take Signature for printing
  CHAR8 Signature[9];
  CopyMem(&Signature[0], &RsdPtr->Signature, 8);
  Signature[8] = 0;

  // Take Rsdt and Xsdt
  Rsdt = NULL;
  Xsdt = NULL;

  DBG(" %llx: '%s', Rev: %d", (uintptr_t)RsdPtr, Signature, RsdPtr->Revision);
  if (RsdPtr->Revision == 0) {
    // Acpi 1.0
    Length = sizeof(EFI_ACPI_1_0_ROOT_SYSTEM_DESCRIPTION_POINTER);
    DBG(" (Acpi 1.0)");
  } else {
    // Acpi 2.0 or newer
    Length = RsdPtr->Length;
    DBG(" (Acpi 2.0 or newer)");
  }
	DBG(", Len: %llu", Length);

  //
  // Save RsdPtr
  //
  if (DirName != NULL && !IsTableSaved(RsdPtr)) {
    DBG(" -> RSDP.aml");
    Status = SaveBufferToDisk(RsdPtr, Length, DirName, L"RSDP.aml");
    MarkTableAsSaved(RsdPtr);
    if (EFI_ERROR(Status)) {
      DBG(" - %s\n", efiStrError(Status));
      return;
    }
  }
  DBG("\n");

  if (RsdPtr->Revision == 0) {
    // Acpi 1.0 - no Xsdt
    Rsdt = (RSDT_TABLE*)(UINTN)(RsdPtr->RsdtAddress);
    DBG("  (Rsdt: %llx)\n", (UINTN)Rsdt);
  } else {
    // Acpi 2.0 or newer - may have Xsdt and/or Rsdt
    Rsdt = (RSDT_TABLE*)(UINTN)(RsdPtr->RsdtAddress);
    Xsdt = (XSDT_TABLE *)(UINTN)(RsdPtr->XsdtAddress);
    DBG("  (Xsdt: %llx, Rsdt: %llx)\n", (UINTN)Xsdt, (UINTN)Rsdt);
  }

  if (Rsdt == NULL && Xsdt == NULL) {
    DBG(" No Rsdt and Xsdt - exiting.\n");
    return;
  }

  FileNamePrefix = L"";
  //
  // Save Xsdt
  //
  if (Xsdt != NULL) {
    DBG(" ");
    Status = DumpTable((EFI_ACPI_DESCRIPTION_HEADER *)Xsdt, "XSDT", DirName,  L"XSDT.aml"_XSW, FileNamePrefix, NULL);
    if (EFI_ERROR(Status)) {
      DBG(" - %s", efiStrError(Status));
      Xsdt = NULL;
    }
    DBG("\n");
  }
  //
  // Save Rsdt
  //
  if (Rsdt != NULL) {
    DBG(" ");
    Status = DumpTable((EFI_ACPI_DESCRIPTION_HEADER *)Rsdt, "RSDT", DirName,  L"RSDT.aml"_XSW, FileNamePrefix, NULL);
    if (EFI_ERROR(Status)) {
      DBG(" - %s", efiStrError(Status));
      Rsdt = NULL;
    }
    DBG("\n");
  }
  //
  // Check once more since they might be invalid
  //
  if (Rsdt == NULL && Xsdt == NULL) {
    DBG(" No Rsdt and Xsdt - exiting.\n");
    return;
  }
  UINT32 Count = 0;
  if (Xsdt) {
    UINT64 *Ptr, *EndPtr;
    Count = XsdtTableCount();
    DBG("  Tables in Xsdt: %d\n", Count);
    if (Count > 100) Count = 100; //it's enough

    Ptr = XsdtEntryPtrFromIndex(0);
    EndPtr = XsdtEntryPtrFromIndex(Count);
    SsdtCount = 0;
    for (; Ptr < EndPtr; Ptr++) {
      DBG("  %d.", IndexFromXsdtEntryPtr(Ptr));
      EFI_ACPI_DESCRIPTION_HEADER* Table = (EFI_ACPI_DESCRIPTION_HEADER*)(UINTN)ReadUnaligned64(Ptr);
      if (!Table) {
        DBG(" = 0\n");
        // skip NULL entry
        continue;
      }
      if (EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE_SIGNATURE == Table->Signature) {
        // Fadt - save Dsdt and Facs
        Status = DumpTable(Table, NULL, DirName,  L""_XSW, FileNamePrefix, &SsdtCount);
        if (EFI_ERROR(Status)) {
          DBG(" - %s\n", efiStrError(Status));
          return;
        }
        DBG("\n");
        EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE* Fadt = (EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE*)Table;
        Status = DumpFadtTables(Fadt, DirName, FileNamePrefix, &SsdtCount);
        if (EFI_ERROR(Status)) {
          return;
        }
      } else {
        Status = DumpTable(Table, NULL, DirName,  L""_XSW, FileNamePrefix, &SsdtCount);
        if (EFI_ERROR(Status)) {
          DBG(" - %s\n", efiStrError(Status));
          return;
        }
        DBG("\n");
      }
    }
  } // if Xsdt

  if (!Count && Rsdt) {
    UINT32 *Ptr, *EndPtr;
    // additional Rsdt tables which are not present in Xsdt will have "RSDT-" prefix, like RSDT-FACS.aml
    FileNamePrefix = L"RSDT-";
    // Take tables from Rsdt
    // if saved from Xsdt already, then just print debug
    Count = RsdtTableCount();
    DBG("  Tables in Rsdt: %d\n", Count);
    if (Count > 100) Count = 100; //it's enough
    Ptr = RsdtEntryPtrFromIndex(0);
    EndPtr = RsdtEntryPtrFromIndex(Count);
    for (; Ptr < EndPtr; Ptr++) {
      DBG("  %d.", IndexFromRsdtEntryPtr(Ptr));
      EFI_ACPI_DESCRIPTION_HEADER* Table = (EFI_ACPI_DESCRIPTION_HEADER*)(UINTN)*Ptr;
      if (!Table) {
        DBG(" = 0\n");
        // skip NULL entry
        continue;
      }
      if (EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE_SIGNATURE == Table->Signature) {
        // Fadt - save Dsdt and Facs
        Status = DumpTable(Table, NULL, DirName,  L""_XSW, FileNamePrefix, &SsdtCount);
        if (EFI_ERROR(Status)) {
          DBG(" - %s\n", efiStrError(Status));
          return;
        }
        DBG("\n");
        EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE* Fadt = (EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE*)Table;
        Status = DumpFadtTables(Fadt, DirName, FileNamePrefix, &SsdtCount);
        if (EFI_ERROR(Status)) {
          return;
        }
      } else {
        Status = DumpTable(Table, NULL, DirName,  L""_XSW, FileNamePrefix, &SsdtCount);
        if (EFI_ERROR(Status)) {
          DBG(" - %s\n", efiStrError(Status));
          return;
        }
        DBG("\n");
      }
    }
  } // if Rsdt
}

/** Saves OEM ACPI tables to disk.
 *  Searches BIOS, then UEFI Sys.Tables for Acpi 2.0 or newer tables, then for Acpi 1.0 tables
 *  CloverEFI:
 *   - saves first one found, dump others to log
 *  UEFI:
 *   - saves first one found in UEFI Sys.Tables, dump others to log
 *
 * Dumping of other tables to log can be removed if it turns out that there is no value in doing it.
 */
void SaveOemTables()
{
//  EFI_STATUS              Status;
  void                   *RsdPtr;
  XStringW                AcpiOriginPath = L"ACPI\\origin"_XSW;
  XBool                   Saved = false;
  CHAR8                  *MemLogStart;
  UINTN                   MemLogStartLen;

  MemLogStartLen = GetMemLogLen();
  MemLogStart = GetMemLogBuffer() + MemLogStartLen;
  //
  // Search in BIOS
  // CloverEFI - Save
  // UEFI - just print to log
  //
  //  RsdPtr = NULL;
  RsdPtr = FindAcpiRsdPtr();
  if (RsdPtr != NULL) {
    DBG("Found BIOS RSDP at %llx\n", (UINTN)RsdPtr);
    if (gFirmwareClover) {
      // Save it
      DumpTables(RsdPtr, AcpiOriginPath.wc_str());
      Saved = true;
    } else {
      // just print to log
      DumpTables(RsdPtr, NULL);
    }
  }
  //
  // Search Acpi 2.0 or newer in UEFI Sys.Tables
  //
  RsdPtr = NULL;
  /*Status = */EfiGetSystemConfigurationTable (&gEfiAcpi20TableGuid, &RsdPtr);
  if (RsdPtr != NULL) { //it may be EFI_SUCCESS but null pointer
    DBG("Found UEFI Acpi 2.0 RSDP at %llx\n", (UINTN)RsdPtr);
    // if tables already saved, then just print to log
    DumpTables(RsdPtr, Saved ? NULL : AcpiOriginPath.wc_str());
    Saved = true;
  }
  //
  // Then search Acpi 1.0 UEFI Sys.Tables
  //
  RsdPtr = NULL;
  /*Status = */EfiGetSystemConfigurationTable (&gEfiAcpi10TableGuid, &RsdPtr);
  if (RsdPtr != NULL) {
    DBG("Found UEFI Acpi 1.0 RSDP at %llx\n", (UINTN)RsdPtr);
    // if tables already saved, then just print to log
    DumpTables(RsdPtr, Saved ? NULL : AcpiOriginPath.wc_str());
    //    Saved = true;
  }
  SaveBufferToDisk(MemLogStart, GetMemLogLen() - MemLogStartLen, AcpiOriginPath.wc_str(), L"DumpLog.txt");
  FreePool(mSavedTables);
}

void SaveOemDsdt(XBool FullPatch)
{
  EFI_STATUS                                    Status = EFI_NOT_FOUND;
  EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE     *FadtPointer = NULL;
  EFI_PHYSICAL_ADDRESS                          dsdt = EFI_SYSTEM_TABLE_MAX_ADDRESS;

  UINTN              Pages;
  UINT8             *buffer = NULL;
  UINTN              DsdtLen = 0;
  XStringW           OriginDsdt      = SWPrintf("ACPI\\origin\\DSDT.aml");
  XStringW           OriginDsdtFixed = SWPrintf("ACPI\\origin\\DSDT-%x.aml", gSettings.ACPI.DSDT.FixDsdt);
//  constexpr LStringW PathPatched     = L"\\EFI\\CL OVER\\ACPI\\patched";
//  XStringW           PathDsdt;
//  XStringW           AcpiOemPath     = SWPrintf("ACPI\\patched");

//  PathDsdt.SWPrintf("\\%ls", gSettings.ACPI.DSDT.FixDsdt.wc_str());

  if (FileExists(selfOem.getConfigDir(), SWPrintf("ACPI\\patched\\%ls", gSettings.ACPI.DSDT.DsdtName.wc_str()))) {
    DBG("SaveOemDsdt: DSDT found in Clover volume OEM folder: \\%ls\\ACPI\\patched\\%ls\n", selfOem.getConfigDirFullPath().wc_str(), gSettings.ACPI.DSDT.DsdtName.wc_str());
    Status = egLoadFile(&selfOem.getConfigDir(), SWPrintf("ACPI\\patched\\%ls", gSettings.ACPI.DSDT.DsdtName.wc_str()).wc_str(), &buffer, &DsdtLen);
  }

//  Jief : Do not write outside OemPath
//  if (EFI_ERROR(Status) && FileExists(&self.getSelfRootDir(), SWPrintf("%ls%ls", PathPatched.wc_str(), PathDsdt.wc_str()))) {
//    DBG("SaveOemDsdt: DSDT found in Clover volume common folder: %ls%ls\n", PathPatched.wc_str(), PathDsdt.wc_str());
//    Status = egLoadFile(&self.getSelfRootDir(), SWPrintf("%ls%ls", PathPatched.wc_str(), PathDsdt.wc_str()).wc_str(), &buffer, &DsdtLen);
//  }

  if (EFI_ERROR(Status)) {
    FadtPointer = GetFadt();
    if (FadtPointer == NULL) {
      DBG("Cannot found FADT in BIOS or in UEFI!\n"); //really?!
      return;
    }
    BiosDsdt = FadtPointer->Dsdt;
    if (FadtPointer->Header.Revision >= EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE_REVISION &&
        FadtPointer->XDsdt != 0) {
      BiosDsdt = FadtPointer->XDsdt;
    }
    buffer = (UINT8*)(UINTN)BiosDsdt;
  }

  if (!buffer) {
    DBG("Cannot found DSDT in BIOS or in files!\n");
    return;
  }

  DsdtLen = ((EFI_ACPI_DESCRIPTION_HEADER*)buffer)->Length;
  Pages = EFI_SIZE_TO_PAGES(DsdtLen + DsdtLen / 8); // take some extra space for patches
  Status = gBS->AllocatePages (
                               AllocateMaxAddress,
                               EfiBootServicesData,
                               Pages,
                               &dsdt
                               );
  //if success insert dsdt pointer into ACPI tables
  if(!EFI_ERROR(Status))
  {
    CopyMem((void*)(UINTN)dsdt, buffer, DsdtLen);
    buffer = (UINT8*)(UINTN)dsdt;
    if (FullPatch) {
      FixBiosDsdt(buffer, FadtPointer, NullXString8);
      DsdtLen = ((EFI_ACPI_DESCRIPTION_HEADER*)buffer)->Length;
      OriginDsdt = OriginDsdtFixed;
    }
    Status = egSaveFile(&selfOem.getConfigDir(), OriginDsdt.wc_str(), buffer, DsdtLen);
// Jief : do not write outside of OemDir
//    if (EFI_ERROR(Status)) {
//      Status = egSaveFile(NULL, OriginDsdt.wc_str(), buffer, DsdtLen);
//    }
    if (!EFI_ERROR(Status)) {
      MsgLog("DSDT saved to %ls\\%ls\n", selfOem.getConfigDirFullPath().wc_str(), OriginDsdt.wc_str());
    } else {
      MsgLog("Saving DSDT to %ls\\%ls failed - %s\n", selfOem.getConfigDirFullPath().wc_str(), OriginDsdt.wc_str(), efiStrError(Status));
    }
    gBS->FreePages(dsdt, Pages);
  }
}

XBool LoadPatchedAML(const EFI_FILE& dir, const XStringW& acpiOemPath, CONST CHAR16* PartName, UINTN Pass)
{
  // pass1 prefilter based on file names (optimization that avoids loading same files twice)
  UINTN Index = IGNORE_INDEX;
  if (AUTOMERGE_PASS1 == Pass) {
    Index = IndexFromFileName(PartName);
    // gSettings.ACPI.AutoMerge always true in this case
    // file names such as: ECDT.aml, SSDT-0.aml, SSDT-1-CpuPm.aml, attempt merge on pass1
    // others: no attempt for merge
    // special case for SSDT.aml: no attempt to merge
    if (0 == StriCmp(PartName, L"SSDT.aml") || (8 != StrLen(PartName) && IGNORE_INDEX == Index)) {
      DBG("ignore on pass 1\n");
      return false;
    }
  }
  UINT8 *buffer = NULL;
  UINTN bufferLen = 0;
  EFI_STATUS Status = egLoadFile(&dir, SWPrintf("%ls\\%ls", acpiOemPath.wc_str(), PartName).wc_str(), &buffer, &bufferLen);
  if (!EFI_ERROR(Status)) {
    if (buffer) {
      EFI_ACPI_DESCRIPTION_HEADER* TableHeader = (EFI_ACPI_DESCRIPTION_HEADER*)buffer;
      if (TableHeader->Length > 500 * Kilo) {
        DBG("wrong table\n");
        return false;
      }
    }
    DBG("size=%lld ", (UINT64)bufferLen);
    if (!gSettings.ACPI.AutoMerge) {
      // Note: with AutoMerge=false, Pass is only AUTOMERGE_PASS2 here
      Status = InsertTable(buffer, bufferLen);
    } else {
      Status = ReplaceOrInsertTable(buffer, bufferLen, Index, Pass);
    }
    FreePool(buffer);
  }
  DBG("... %s\n", efiStrError(Status));
  return !EFI_ERROR(Status);
}

#define BVALUE_ATTEMPTED 2  // special value for MenuItem.BValue to avoid excessive log output

void LoadAllPatchedAML(const XStringW& acpiPathUnderOem, UINTN Pass)
{
  if (!gSettings.ACPI.AutoMerge && AUTOMERGE_PASS1 == Pass) {
    // nothing to do in this case, since AutoMerge=false -> no tables ever merged
    return;
  }
  if ( ACPIPatchedAML.notEmpty() ) {
    DbgHeader("ACPIPatchedAML");
    if (gSettings.ACPI.AutoMerge) {
		DBG("AutoMerge pass %llu\n", Pass);
    }
    //DBG("Start: Processing Patched AML(s): ");
    if (gSettings.ACPI.SortedACPI.size()) {
      UINTN Index;
      DBG("Sorted\n");
      for (Index = 0; Index < gSettings.ACPI.SortedACPI.size(); Index++) {
        size_t idx;
        for ( idx = 0 ; idx < ACPIPatchedAML.size() ; ++idx) {
          ACPI_PATCHED_AML& ACPIPatchedAMLTmp = ACPIPatchedAML[idx];
          if ( ACPIPatchedAMLTmp.FileName == gSettings.ACPI.SortedACPI[Index] && ACPIPatchedAMLTmp.MenuItem.BValue) {
            if (BVALUE_ATTEMPTED != ACPIPatchedAMLTmp.MenuItem.BValue)
              DBG("Disabled: %s, skip\n", ACPIPatchedAMLTmp.FileName.c_str());
            ACPIPatchedAMLTmp.MenuItem.BValue = BVALUE_ATTEMPTED;
            break;
          }
        }
        if ( idx == ACPIPatchedAML.size() ) { // NULL when not disabled
          DBG("Inserting table[%llu]:%s from %ls\\%ls: ", Index, gSettings.ACPI.SortedACPI[Index].c_str(), selfOem.getConfigDirFullPath().wc_str(), acpiPathUnderOem.wc_str());
          if (LoadPatchedAML(selfOem.getConfigDir(), acpiPathUnderOem, XStringW(gSettings.ACPI.SortedACPI[Index]).wc_str(), Pass)) {
            // avoid inserting table again on second pass
            for ( idx = 0 ; idx < ACPIPatchedAML.size() ; ++idx) {
              ACPI_PATCHED_AML& temp2 = ACPIPatchedAML[idx];
              if ( temp2.FileName == gSettings.ACPI.SortedACPI[Index] ) {
                temp2.MenuItem.BValue = BVALUE_ATTEMPTED;
                break;
              }
            }
          }
        }
      }
    } else {
      DBG("Unsorted\n");
      for ( size_t idx = 0 ; idx < ACPIPatchedAML.size() ; ++idx) {
        ACPI_PATCHED_AML& ACPIPatchedAMLTmp = ACPIPatchedAML[idx];
        if (!ACPIPatchedAMLTmp.MenuItem.BValue) {
          DBG("Inserting %s from %ls\\%ls: ", ACPIPatchedAMLTmp.FileName.c_str(), selfOem.getConfigDirFullPath().wc_str(), acpiPathUnderOem.wc_str());
          if (LoadPatchedAML(selfOem.getConfigDir(), acpiPathUnderOem, XStringW(ACPIPatchedAMLTmp.FileName).wc_str(), Pass)) {
            // avoid inserting table again on second pass
            ACPIPatchedAMLTmp.MenuItem.BValue = BVALUE_ATTEMPTED;
          }
        } else {
          if (BVALUE_ATTEMPTED != ACPIPatchedAMLTmp.MenuItem.BValue)
            DBG("Disabled: %s, skip\n", ACPIPatchedAMLTmp.FileName.c_str());
          ACPIPatchedAMLTmp.MenuItem.BValue = BVALUE_ATTEMPTED;
        }
      }
    }
    //DBG("End: Processing Patched AML(s)\n");
  }
}

EFI_STATUS PatchACPI(IN REFIT_VOLUME *Volume, const MacOsVersion& OSVersion)
{
  EFI_STATUS                    Status = EFI_SUCCESS;
  UINTN                         Index;
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  *RsdPointer = NULL;
  EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE    *FadtPointer = NULL;
  EFI_ACPI_4_0_FIXED_ACPI_DESCRIPTION_TABLE    *newFadt   = NULL;
  //  EFI_ACPI_HIGH_PRECISION_EVENT_TIMER_TABLE_HEADER  *Hpet    = NULL;
  EFI_ACPI_4_0_FIRMWARE_ACPI_CONTROL_STRUCTURE  *Facs = NULL;
  EFI_PHYSICAL_ADDRESS    dsdt = EFI_SYSTEM_TABLE_MAX_ADDRESS; //0xFE000000;
  EFI_PHYSICAL_ADDRESS    BufferPtr;
  SSDT_TABLE              *Ssdt = NULL;
  UINT8                   *buffer = NULL;
  UINTN                   bufferLen = 0;
//  constexpr LStringW      PathPatched   = L"\\EFI\\CL OVER\\ACPI\\patched";
//  XStringW                PathDsdt;    //  = L"\\DSDT.aml";
//  CHAR16*                 PatchedAPIC = L"\\EFI\\CL OVER\\ACPI\\origin\\APIC-p.aml";
  UINT32*                 rf = NULL;
  UINT64*                 xf = NULL;
  UINT64                  XDsdt; //save values if present
  UINT64                  XFirmwareCtrl;
//  EFI_FILE                *RootDir;
  UINT32                  eCntR; //, eCntX;
  UINT32                  *pEntryR;
  CHAR8                   *pEntry;
  EFI_ACPI_DESCRIPTION_HEADER *TableHeader;
  // -===== APIC =====-
  EFI_ACPI_DESCRIPTION_HEADER                           *ApicTable;
  //  EFI_ACPI_2_0_MULTIPLE_APIC_DESCRIPTION_TABLE_HEADER   *ApicHeader;
  EFI_ACPI_2_0_PROCESSOR_LOCAL_APIC_STRUCTURE           *ProcLocalApic;
  EFI_ACPI_2_0_LOCAL_APIC_NMI_STRUCTURE                 *LocalApicNMI;
  //  UINTN             ApicLen;
  UINTN              ApicCPUNum;
  UINT8             *SubTable;
  XBool              DsdtLoaded = false;
  XBool              NeedUpdate = false;
  OPER_REGION       *tmpRegion;
//  XStringW           AcpiOemPath = SWPrintf("%ls\\ACPI\\patched", OEMPath.wc_str());

  DbgHeader("PatchACPI");

  //try to find in SystemTable
  for(Index = 0; Index < gST->NumberOfTableEntries; Index++) {
    if( gST->ConfigurationTable[Index].VendorGuid == gEfiAcpi20TableGuid ) {
      // Acpi 2.0
      RsdPointer = (EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER*)gST->ConfigurationTable[Index].VendorTable;
      break;
    }
    else if( gST->ConfigurationTable[Index].VendorGuid == gEfiAcpi10TableGuid ) {
      // Acpi 1.0 - RSDT only
      RsdPointer = (EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER*)gST->ConfigurationTable[Index].VendorTable;
      continue;
    }
  }

  if (!RsdPointer) {
    return EFI_UNSUPPORTED;
  }
  Rsdt = (RSDT_TABLE*)(UINTN)RsdPointer->RsdtAddress;
  // DBG("RSDT 0x%llx\n", Rsdt);
  rf = ScanRSDT(EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE_SIGNATURE, 0);
  if(rf) {
    FadtPointer = (EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE*)(UINTN)(*rf);
    //   DBG("FADT from RSDT: 0x%llx\n", FadtPointer);
  }

  Xsdt = NULL;
  if (RsdPointer->Revision >=2 && (RsdPointer->XsdtAddress < (UINT64)(UINTN)-1)) {
    Xsdt = (XSDT_TABLE*)(UINTN)RsdPointer->XsdtAddress;
    //   DBG("XSDT 0x%llx\n", Xsdt);
    xf = ScanXSDT(EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE_SIGNATURE, 0);
    if(xf) {
      //Slice - change priority. First Xsdt, second Rsdt
      if (*xf) {
        FadtPointer = (EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE*)(UINTN)(*xf);
        //       DBG("FADT from XSDT: 0x%llx\n", FadtPointer);
      } else {
        *xf =  (UINT64)(UINTN)FadtPointer;
        //       DBG("reuse FADT\n");  //never happens
      }
    }
  }

  if(!xf && Rsdt) {
    DBG("Xsdt is not found! Creating new one\n");
    //We should make here ACPI20 RSDP with all needed subtables based on ACPI10
    BufferPtr = EFI_SYSTEM_TABLE_MAX_ADDRESS;
    Status = gBS->AllocatePages(AllocateMaxAddress, EfiACPIReclaimMemory, 1, &BufferPtr);
    if(!EFI_ERROR(Status)) {
      if (RsdPointer->Revision == 0) {
        // Acpi 1.0 RsdPtr, but we need Acpi 2.0
        EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  *NewRsdPointer;
        DBG("RsdPointer is Acpi 1.0 - creating new one Acpi 2.0\n");

        // add new pointer to the beginning of a new buffer
        NewRsdPointer = (EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER*)(UINTN)BufferPtr;

        // and Xsdt will come after it
        BufferPtr += 0x30;
        //     DBG("::pointers %llx %llx\n", NewRsdPointer, RsdPointer);
        // Signature, Checksum, OemId, Reserved/Revision, RsdtAddress
        CopyMem(NewRsdPointer, RsdPointer, sizeof(EFI_ACPI_1_0_ROOT_SYSTEM_DESCRIPTION_POINTER));
        NewRsdPointer->Revision = 2;
        NewRsdPointer->Length = sizeof(EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER);
        RsdPointer = NewRsdPointer;
        NeedUpdate = true;
        //        gBS->InstallConfigurationTable(&gEfiAcpiTableGuid, RsdPointer);
        //        DBG("first install success\n");
        //        gBS->InstallConfigurationTable(&gEfiAcpi10TableGuid, RsdPointer);
        DBG("RsdPointer Acpi 2.0 installed\n");
      }
      Xsdt = (XSDT_TABLE*)(UINTN)BufferPtr;
      //      DBG("XSDT = 0x%llx\n", uintptr_t(Xsdt));
      Xsdt->Header.Signature = 0x54445358; //EFI_ACPI_2_0_EXTENDED_SYSTEM_DESCRIPTION_TABLE_SIGNATURE
      eCntR = (Rsdt->Header.Length - sizeof (EFI_ACPI_DESCRIPTION_HEADER)) / sizeof(UINT32);
      Xsdt->Header.Length = eCntR * sizeof(UINT64) + sizeof (EFI_ACPI_DESCRIPTION_HEADER);
      Xsdt->Header.Revision = 1;
      CopyMem(&Xsdt->Header.OemId, &FadtPointer->Header.OemId, 6);
      //     Xsdt->Header.OemTableId = Rsdt->Header.OemTableId;
      CopyMem(&Xsdt->Header.OemTableId, &Rsdt->Header.OemTableId, 8);
      Xsdt->Header.OemRevision = Rsdt->Header.OemRevision;
      Xsdt->Header.CreatorId = Rsdt->Header.CreatorId;
      Xsdt->Header.CreatorRevision = Rsdt->Header.CreatorRevision;
      pEntryR = (UINT32*)(&(Rsdt->Entry));
      pEntry = (CHAR8*)(&(Xsdt->Entry));
      DBG("RSDT entries = %d\n", eCntR);
      for (Index = 0; Index < eCntR; Index ++)
      {
        UINT64  *pEntryX = (UINT64 *)pEntry;
        //        DBG("RSDT entry = 0x%X\n", *pEntryR);
        if (*pEntryR != 0) {
          *pEntryX = 0;
          CopyMem(pEntryX, pEntryR, sizeof(UINT32));
          pEntryR++;
          pEntry += sizeof(UINT64);
        } else {
          DBG("RSDT entry %llu = 0 ... skip it\n", Index);
          Xsdt->Header.Length -= sizeof(UINT64);
          pEntryR++;
        }
      }
    }
  }
  if (Xsdt) {
    //Now we need no more Rsdt
    Rsdt =  NULL;
    RsdPointer->RsdtAddress = 0;
    //and we want to reallocate Xsdt
    BufferPtr = EFI_SYSTEM_TABLE_MAX_ADDRESS;
    Status = gBS->AllocatePages(AllocateMaxAddress, EfiACPIReclaimMemory, 1, &BufferPtr);
    if(!EFI_ERROR(Status))
    {
      CopyMem((void*)(UINTN)BufferPtr, Xsdt, Xsdt->Header.Length);
      Xsdt = (XSDT_TABLE*)(UINTN)BufferPtr;
    }
    //   DBG("Finishing RsdPointer\n");
    RsdPointer->XsdtAddress = (UINT64)(UINTN)Xsdt;
    RsdPointer->Checksum = 0;
    RsdPointer->Checksum = (UINT8)(256-Checksum8((CHAR8*)RsdPointer, 20));
    RsdPointer->ExtendedChecksum = 0;
    RsdPointer->ExtendedChecksum = (UINT8)(256-Checksum8((CHAR8*)RsdPointer, RsdPointer->Length));
    DBG("Xsdt reallocation done\n");
  }
  //  DBG("FADT pointer = %X\n", (UINTN)FadtPointer);
  if(!FadtPointer) {
    return EFI_NOT_FOUND;
  }
  //Slice - then we do FADT patch no matter if we don't have DSDT.aml
  BufferPtr = EFI_SYSTEM_TABLE_MAX_ADDRESS;
  Status = gBS->AllocatePages(AllocateMaxAddress, EfiACPIReclaimMemory, 1, &BufferPtr);
  if(!EFI_ERROR(Status))
  {
    UINT32 oldLength = ((EFI_ACPI_DESCRIPTION_HEADER*)FadtPointer)->Length;
    newFadt = (EFI_ACPI_4_0_FIXED_ACPI_DESCRIPTION_TABLE*)(UINTN)BufferPtr;
    DBG("old FADT length=%X\n", oldLength);
    CopyMem(newFadt, FadtPointer, oldLength); //old data
    newFadt->Header.Length = 0xF4;
    CopyMem(newFadt->Header.OemId, AppleBiosVendor.c_str(), 6);
    if (newFadt->Header.Revision < EFI_ACPI_4_0_FIXED_ACPI_DESCRIPTION_TABLE_REVISION) {
      newFadt->Header.Revision = EFI_ACPI_4_0_FIXED_ACPI_DESCRIPTION_TABLE_REVISION;
    }
    newFadt->Reserved0 = 0; //ACPIspec said it should be 0, while 1 is possible, but no more

    //should correct headers if needed and if asked
    PatchTableHeader((EFI_ACPI_DESCRIPTION_HEADER*)newFadt);

    if (gSettings.ACPI.smartUPS==true) {
      newFadt->PreferredPmProfile = 3;
    } else {
      newFadt->PreferredPmProfile = gMobile?2:1; //as calculated before
    }
    if (GlobalConfig.EnableC6 || gSettings.ACPI.SSDT.EnableISS) {
      newFadt->CstCnt = 0x85; //as in Mac
    }
    if (GlobalConfig.EnableC2) newFadt->PLvl2Lat = 0x65;
    if (GlobalConfig.C3Latency > 0) {
      newFadt->PLvl3Lat = GlobalConfig.C3Latency;
    } else if (GlobalConfig.EnableC4) {
      newFadt->PLvl3Lat = 0x3E9;
    }
    if (GlobalConfig.C3Latency == 0) {
      GlobalConfig.C3Latency = newFadt->PLvl3Lat;
    }
    
    newFadt->IaPcBootArch = 0x3;
    if (gSettings.ACPI.NoASPM) {
      newFadt->IaPcBootArch |= 0x10;  // disable ASPM
    }
    newFadt->Flags |= 0x420; //Reset Register Supported and SleepButton active
    newFadt->Flags &= ~0x10010; //RTC_STS not valid and PowerButton disable
    XDsdt = newFadt->XDsdt; //save values if present
    XFirmwareCtrl = newFadt->XFirmwareCtrl;
    CopyMem(&newFadt->ResetReg, pmBlock, 0x80);
    //but these common values are not specific, so adjust
    //ACPIspec said that if Xdsdt !=0 then Dsdt must be =0. But real Mac no! Both values present
    if (BiosDsdt) {
      newFadt->XDsdt = BiosDsdt;
      newFadt->Dsdt = (UINT32)BiosDsdt;
    } else if (newFadt->Dsdt) {
      newFadt->XDsdt = (UINT64)(newFadt->Dsdt);
    } else if (XDsdt) {
      newFadt->Dsdt = (UINT32)XDsdt;
    }
    if (newFadt->FirmwareCtrl) {
      Facs = (EFI_ACPI_4_0_FIRMWARE_ACPI_CONTROL_STRUCTURE*)(UINTN)newFadt->FirmwareCtrl;
      newFadt->XFirmwareCtrl = (UINT64)(UINTN)(Facs);
    } else if (newFadt->XFirmwareCtrl) {
      newFadt->FirmwareCtrl = (UINT32)XFirmwareCtrl;
      Facs = (EFI_ACPI_4_0_FIRMWARE_ACPI_CONTROL_STRUCTURE*)(UINTN)XFirmwareCtrl;
    }

    //patch for FACS included here
    Facs->Version = EFI_ACPI_4_0_FIRMWARE_ACPI_CONTROL_STRUCTURE_VERSION;
    if (gSettings.Boot.SignatureFixup) {
		DBG(" SignatureFixup: 0x%X -> 0x%llX\n", Facs->HardwareSignature, machineSignature);
      Facs->HardwareSignature = (UINT32)machineSignature;
    } else {
      DBG(" SignatureFixup: 0x%X -> 0x0\n", Facs->HardwareSignature);
      Facs->HardwareSignature = 0x0;
    }
    Facs->Flags = 0; //dont' support S4BIOS, as well as 64bit wake
    //

    if ((gSettings.ACPI.ResetAddr == 0) && ((oldLength < 0x80) || (newFadt->ResetReg.Address == 0))) {
      newFadt->ResetReg.Address   = 0x64;
      newFadt->ResetValue         = 0xFE;
      gSettings.ACPI.ResetAddr         = 0x64;
      gSettings.ACPI.ResetVal          = 0xFE;
    } else if (gSettings.ACPI.ResetAddr != 0) {
      newFadt->ResetReg.Address    = gSettings.ACPI.ResetAddr;
      newFadt->ResetValue          = gSettings.ACPI.ResetVal;
    }
    newFadt->XPm1aEvtBlk.Address = (UINT64)(newFadt->Pm1aEvtBlk);
    newFadt->XPm1bEvtBlk.Address = (UINT64)(newFadt->Pm1bEvtBlk);
    newFadt->XPm1aCntBlk.Address = (UINT64)(newFadt->Pm1aCntBlk);
    newFadt->XPm1bCntBlk.Address = (UINT64)(newFadt->Pm1bCntBlk);
    newFadt->XPm2CntBlk.Address  = (UINT64)(newFadt->Pm2CntBlk);
    newFadt->XPmTmrBlk.Address   = (UINT64)(newFadt->PmTmrBlk);
    newFadt->XGpe0Blk.Address    = (UINT64)(newFadt->Gpe0Blk);
    newFadt->XGpe1Blk.Address    = (UINT64)(newFadt->Gpe1Blk);
    FadtPointer = (EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE*)newFadt;
    //We are sure that Fadt is the first entry in RSDT/XSDT table
    if (Rsdt!=NULL) {
      Rsdt->Entry = (UINT32)(UINTN)newFadt;
    }
    if (Xsdt!=NULL) {
      Xsdt->Entry = (UINT64)((UINT32)(UINTN)newFadt);
    }
    FixChecksum(&FadtPointer->Header);
    if (gSettings.ACPI.SlpSmiEnable) {
      UINT32 *SlpSmiEn = (UINT32*)((UINTN)(newFadt->Pm1aEvtBlk) + 0x30);
      UINT32 Value = *SlpSmiEn;
      Value &= ~ bit(4);
      *SlpSmiEn = Value;
    }
  }

  //Get regions from BIOS DSDT
  if ((gSettings.ACPI.DSDT.FixDsdt & FIX_REGIONS) != 0) {
    GetBiosRegions((UINT8*)(UINTN)(newFadt->Dsdt));
  }
  //  DBG("DSDT finding\n");
  if (!Volume) {
    DBG("Volume not found!\n");
    return EFI_NOT_FOUND;
  }
//  RootDir = Volume->RootDir;
  Status = EFI_NOT_FOUND;


  XStringW acpiPath = SWPrintf("ACPI\\patched\\%ls", gSettings.ACPI.DSDT.DsdtName.wc_str());
  
  if ( selfOem.oemDirExists() ) {
    if ( FileExists(&selfOem.getOemDir(), acpiPath) ) {
      DBG("DSDT found in Clover volume OEM folder: %ls\\%ls\n", selfOem.getOemFullPath().wc_str(), acpiPath.wc_str());
      Status = egLoadFile(&selfOem.getOemDir(), acpiPath.wc_str(), &buffer, &bufferLen);
      //REVIEW: memory leak... buffer
    }
  }

  //Slice: the idea was from past
  // first priority DSDT.aml from the root of booted volume. It allows to keep different DSDT for different systems
  // second priority is DSDT from OEM folder
  // third priority is {Clover folder}/ACPI/patched/DSDT*.aml choosen from GUI.
  
  XStringW PathDsdt = SWPrintf("\\%ls", gSettings.ACPI.DSDT.DsdtName.wc_str());
  if (EFI_ERROR(Status) && FileExists(Volume->RootDir, PathDsdt)) {
    DBG("DSDT found in booted volume\n");
    Status = egLoadFile(Volume->RootDir, PathDsdt.wc_str(), &buffer, &bufferLen);
  }

  //  Jief : may I suggest to remove that. Loading from outside of OemPath might be confusing
  if ( EFI_ERROR(Status)  &&  FileExists(&self.getCloverDir(), acpiPath) ) {
    DBG("DSDT found in Clover volume: %ls\\%ls\n", self.getCloverDirFullPath().wc_str(), acpiPath.wc_str());
    Status = egLoadFile(&self.getCloverDir(), acpiPath.wc_str(), &buffer, &bufferLen);
  }
  //
  //apply DSDT loaded from a file into buffer
  //else FADT will contain old BIOS DSDT
  //
  DsdtLoaded = false;
  if (!EFI_ERROR(Status)) {
    // if we will apply fixes, allocate additional space
    bufferLen = bufferLen + bufferLen / 8;
    dsdt = EFI_SYSTEM_TABLE_MAX_ADDRESS;
    Status = gBS->AllocatePages (
                                 AllocateMaxAddress,
                                 EfiACPIReclaimMemory,
                                 EFI_SIZE_TO_PAGES(bufferLen),
                                 &dsdt
                                 );

    //if success insert dsdt pointer into ACPI tables
    if(!EFI_ERROR(Status)) {
      //      DBG("page is allocated, write DSDT into\n");
      CopyMem((void*)(UINTN)dsdt, buffer, bufferLen);
      //once we copied buffer to other place we can free the buffer
      FadtPointer->Dsdt  = (UINT32)dsdt;
      FadtPointer->XDsdt = dsdt;
      FixChecksum(&FadtPointer->Header);
      DsdtLoaded = true;
    }
  }
  if(buffer) FreePool(buffer); //the buffer is allocated if egLoadFile() is success. Else the pointer must be nullptr

  if (!DsdtLoaded) {
    // allocate space for fixes
    TableHeader = (EFI_ACPI_DESCRIPTION_HEADER*)(UINTN)FadtPointer->Dsdt;
    bufferLen = TableHeader->Length;
    //    DBG("DSDT len = 0x%X", bufferLen);
    //    bufferLen = bufferLen + bufferLen / 8;
    //    DBG(" new len = 0x%X\n", bufferLen);

    //Slice: new buffer is greater then origin by 12.5%. It is dirty hack but we live with it
    // there will be the sense reallocate buffer if one patch requires increasing the buffer
    // this is headache to predict how many new bytes we need.
    dsdt = EFI_SYSTEM_TABLE_MAX_ADDRESS;
    Status = gBS->AllocatePages(AllocateMaxAddress,
                                EfiACPIReclaimMemory,
                                EFI_SIZE_TO_PAGES(bufferLen + bufferLen / 8),
                                &dsdt);

    //if success insert dsdt pointer into ACPI tables
    if(!EFI_ERROR(Status)) {
      CopyMem((void*)(UINTN)dsdt, TableHeader, bufferLen); //can't free TableHeader because we are not allocate it

      FadtPointer->Dsdt  = (UINT32)dsdt;
      FadtPointer->XDsdt = dsdt;
      FixChecksum(&FadtPointer->Header);
    }
  }
//  dropDSM = 0xFFFF; //by default we drop all OEM _DSM. They have no sense for us.
//  if (defDSM) {
//    dropDSM = gSettings.DropOEM_DSM;   //if set by user
//  }

  if (gSettings.ACPI.DSDT.DebugDSDT) {
    TableHeader = (EFI_ACPI_DESCRIPTION_HEADER*)(UINTN)FadtPointer->XDsdt;
    bufferLen = TableHeader->Length;

    DBG("Output DSDT before patch to %ls\\ACPI\\origin\\DSDT-or.aml\n", selfOem.getConfigDirFullPath().wc_str());
    Status = egSaveFile(&selfOem.getConfigDir(), L"ACPI\\origin\\DSDT-or.aml", (UINT8*)(UINTN)FadtPointer->XDsdt, bufferLen);
  }
  //native DSDT or loaded we want to apply autoFix to this
  //  if (gSettings.ACPI.DSDT.FixDsdt) { //fix even with zero mask because we want to know PCIRootUID and count(?)
  DBG("Apply DsdtFixMask=0x%08X\n", gSettings.ACPI.DSDT.FixDsdt);
//  DBG("   drop _DSM mask=0x%04hX\n", dropDSM);
  FixBiosDsdt((UINT8*)(UINTN)FadtPointer->XDsdt, FadtPointer, OSVersion);
  if (gSettings.ACPI.DSDT.DebugDSDT) {
    for (Index=0; Index < 60; Index++) {
      XStringW DsdtPatchedName = SWPrintf("ACPI\\origin\\DSDT-pa%llu.aml", Index);
      if(!FileExists(&selfOem.getConfigDir(), DsdtPatchedName)){
        Status = egSaveFile(&selfOem.getConfigDir(), DsdtPatchedName.wc_str(), (UINT8*)(UINTN)FadtPointer->XDsdt, bufferLen);
        if (!EFI_ERROR(Status)) {
          break;
        }
      }
    }
    if (EFI_ERROR(Status)) {
      DBG("...saving DSDT failed with status=%s\n", efiStrError(Status));
    }
  }

  // handle unusual situations with XSDT and RSDT
  PreCleanupRSDT();
  PreCleanupXSDT();

  // XsdtReplaceSizes array is used to keep track of allocations for the merged tables,
  //  as those tables may need to be freed if patched later.
  XsdtReplaceSizes = (__typeof__(XsdtReplaceSizes))AllocateZeroPool(XsdtTableCount() * sizeof(*XsdtReplaceSizes));

  // Load merged ACPI files from ACPI/patched
  LoadAllPatchedAML(L"ACPI\\patched"_XSW, AUTOMERGE_PASS1);

  // Drop tables
  if (GlobalConfig.ACPIDropTables) {
    ACPI_DROP_TABLE *DropTable;
    DbgHeader("ACPIDropTables");
    for (DropTable = GlobalConfig.ACPIDropTables; DropTable; DropTable = DropTable->Next) {
      if (DropTable->MenuItem.BValue) {
        //DBG("Attempting to drop \"%4.4a\" (%8.8X) \"%8.8a\" (%16.16lX) L=%d\n", &(DropTable->Signature), DropTable->Signature, &(DropTable->TableId), DropTable->TableId, DropTable->Length);
        DropTableFromXSDT(DropTable->Signature, DropTable->TableId, DropTable->Length);
        DropTableFromRSDT(DropTable->Signature, DropTable->TableId, DropTable->Length);
      }
    }
  }

  if (GlobalConfig.DropSSDT) {
    DbgHeader("DropSSDT");
    //special case if we set into menu drop all SSDT
    DropTableFromXSDT(EFI_ACPI_4_0_SECONDARY_SYSTEM_DESCRIPTION_TABLE_SIGNATURE, 0, 0);
    DropTableFromRSDT(EFI_ACPI_4_0_SECONDARY_SYSTEM_DESCRIPTION_TABLE_SIGNATURE, 0, 0);
  }
  //It's time to fix headers of all remaining ACPI tables.
  // The bug reported by TheRacerMaster and https://alextjam.es/debugging-appleacpiplatform/
  // Workaround proposed by cecekpawon, revised by Slice
  PatchAllTables();

  // Load add-on ACPI files from ACPI/patched
  LoadAllPatchedAML(L"ACPI\\patched"_XSW, AUTOMERGE_PASS2);

  if (XsdtReplaceSizes) {
    FreePool(XsdtReplaceSizes);
    XsdtReplaceSizes = NULL;
  }

  //Slice - this is a time to patch MADT table.
  //  DBG("Fool proof: size of APIC NMI  = %d\n", sizeof(EFI_ACPI_2_0_LOCAL_APIC_NMI_STRUCTURE));
  //  DBG("----------- size of APIC DESC = %d\n", sizeof(EFI_ACPI_2_0_MULTIPLE_APIC_DESCRIPTION_TABLE_HEADER));
  //  DBG("----------- size of APIC PROC = %d\n", sizeof(EFI_ACPI_2_0_PROCESSOR_LOCAL_APIC_STRUCTURE));

  ApicCPUNum = 0;
  // 2. For absent NMI subtable
  xf = ScanXSDT(APIC_SIGN, 0);
  if (xf) {
    ApicTable = (EFI_ACPI_DESCRIPTION_HEADER*)(UINTN)(*xf);
    //      ApicLen = ApicTable->Length;
    ProcLocalApic = (EFI_ACPI_2_0_PROCESSOR_LOCAL_APIC_STRUCTURE *)(UINTN)(*xf + sizeof(EFI_ACPI_2_0_MULTIPLE_APIC_DESCRIPTION_TABLE_HEADER));

    while ((ProcLocalApic->Type == EFI_ACPI_4_0_PROCESSOR_LOCAL_APIC) && (ProcLocalApic->Length == 8)) {
      if (ProcLocalApic->Flags & EFI_ACPI_4_0_LOCAL_APIC_ENABLED) {
        ApicCPUNum++;
      }
      ProcLocalApic++;
      if (ApicCPUNum > 16) {
        DBG("Out of control with CPU numbers\n");
        break;
      }
    }
    //fool proof
    if ((ApicCPUNum == 0) || (ApicCPUNum > 16)) {
      ApicCPUNum = gCPUStructure.Threads;
    }

	  DBG("ApicCPUNum=%llu\n", ApicCPUNum);
    //reallocate table
    if (gSettings.ACPI.PatchNMI) {
      BufferPtr = EFI_SYSTEM_TABLE_MAX_ADDRESS;
      Status=gBS->AllocatePages(AllocateMaxAddress, EfiACPIReclaimMemory, 1, &BufferPtr);
      if(!EFI_ERROR(Status)) {
        //save old table and drop it from XSDT
        CopyMem((void*)(UINTN)BufferPtr, ApicTable, ApicTable->Length);
        DropTableFromXSDT(APIC_SIGN, 0, 0);
        ApicTable = (EFI_ACPI_DESCRIPTION_HEADER*)(UINTN)BufferPtr;
        ApicTable->Revision = EFI_ACPI_4_0_MULTIPLE_APIC_DESCRIPTION_TABLE_REVISION;
        CopyMem(&ApicTable->OemId, oemID, 6);
        CopyMem(&ApicTable->OemTableId, oemTableID, 8);
        ApicTable->OemRevision = 0x00000001;
        CopyMem(&ApicTable->CreatorId, creatorID, 4);

        SubTable = (UINT8*)((UINTN)BufferPtr + sizeof(EFI_ACPI_2_0_MULTIPLE_APIC_DESCRIPTION_TABLE_HEADER));
        Index = 0;
        while (*SubTable != EFI_ACPI_4_0_LOCAL_APIC_NMI) {
          DBG("Found subtable in MADT: type=%d\n", *SubTable);
          if (*SubTable == EFI_ACPI_4_0_PROCESSOR_LOCAL_APIC) {
            ProcLocalApic = (EFI_ACPI_2_0_PROCESSOR_LOCAL_APIC_STRUCTURE *)SubTable;
            // macOS assumes that the first processor from DSDT is always enabled, without checking MADT table
            // here we're trying to assign first IDs found in DSDT to enabled processors in MADT, such that macOS assumption to be true
            if (ProcLocalApic->Flags & EFI_ACPI_4_0_LOCAL_APIC_ENABLED) {
              if (ProcLocalApic->AcpiProcessorId != acpi_cpu_processor_id[Index]) {
                DBG("AcpiProcessorId changed: 0x%02hhX to 0x%02hhX\n", ProcLocalApic->AcpiProcessorId, acpi_cpu_processor_id[Index]);
                ProcLocalApic->AcpiProcessorId = acpi_cpu_processor_id[Index];
              } else {
                DBG("AcpiProcessorId: 0x%02hhX\n", ProcLocalApic->AcpiProcessorId);
              }
              Index++;
            }
          }
          bufferLen = (UINTN)SubTable[1];
          SubTable += bufferLen;
          if (((UINTN)SubTable - (UINTN)BufferPtr) >= ApicTable->Length) {
            break;
          }
        }

        if (*SubTable == EFI_ACPI_4_0_LOCAL_APIC_NMI) {
          DBG("LocalApicNMI is already present, no patch needed\n");
        } else {
          LocalApicNMI = (EFI_ACPI_2_0_LOCAL_APIC_NMI_STRUCTURE*)((UINTN)ApicTable + ApicTable->Length);
          for (Index = 0; Index < ApicCPUNum; Index++) {
            LocalApicNMI->Type = EFI_ACPI_4_0_LOCAL_APIC_NMI;
            LocalApicNMI->Length = sizeof(EFI_ACPI_4_0_LOCAL_APIC_NMI_STRUCTURE);
            LocalApicNMI->AcpiProcessorId = acpi_cpu_processor_id[Index];
            LocalApicNMI->Flags = 5;
            LocalApicNMI->LocalApicLint = 1;
            LocalApicNMI++;
            ApicTable->Length += sizeof(EFI_ACPI_4_0_LOCAL_APIC_NMI_STRUCTURE);
          }
          DBG("ApicTable new Length=%d\n", ApicTable->Length);
          // insert corrected MADT
        }

        Status = InsertTable(ApicTable, ApicTable->Length);
        if (!EFI_ERROR(Status)) {
          DBG("New APIC table successfully inserted\n");
        }
        /*
        Status = egSaveFile(&self.getSelfRootDir(), PatchedAPIC, (UINT8 *)ApicTable, ApicTable->Length);
        if (EFI_ERROR(Status)) {
          Status = egSaveFile(NULL, PatchedAPIC,  (UINT8 *)ApicTable, ApicTable->Length);
        }
        if (!EFI_ERROR(Status)) {
          DBG("Patched APIC table saved into efi/clover/acpi/origin/APIC-p.aml \n");
        }
         */
      }
    }
  }
  else {
    DBG("No APIC table Found !!!\n");
  }

  if (gCPUStructure.Threads >= gCPUStructure.Cores) {
    ApicCPUNum = gCPUStructure.Threads;
  } else {
    ApicCPUNum = gCPUStructure.Cores;
  }
  //  }
  /*
   At this moment we have CPU numbers from DSDT - acpi_cpu_num
   and from CPU characteristics gCPUStructure
   Also we had the number from APIC table ApicCPUNum
   What to choose?
   Since rev745 I will return to acpi_cpu_count global variable
   */
  if (acpi_cpu_count) {
    ApicCPUNum = acpi_cpu_count;
  }

  if (gSettings.ACPI.SSDT.Generate.GeneratePStates || gSettings.ACPI.SSDT.Generate.GeneratePluginType) {
    Status = EFI_NOT_FOUND;
    Ssdt = generate_pss_ssdt(ApicCPUNum);
    if (Ssdt) {
      Status = InsertTable(Ssdt, Ssdt->Length);
    }
    if(EFI_ERROR(Status)){
      DBG("GeneratePStates failed: Status=%s\n", efiStrError(Status));
    }
  }

  if (gSettings.ACPI.SSDT.Generate.GenerateCStates) {
    Status = EFI_NOT_FOUND;
    Ssdt = generate_cst_ssdt(FadtPointer, ApicCPUNum);
    if (Ssdt) {
      Status = InsertTable(Ssdt, Ssdt->Length);
    }
    if(EFI_ERROR(Status)){
      DBG("GenerateCStates failed Status=%s\n", efiStrError(Status));
    }
  }

  // remove NULL entries from RSDT and XSDT
  PostCleanupRSDT();
  PostCleanupXSDT();

  if (NeedUpdate) {
    gBS->InstallConfigurationTable(&gEfiAcpiTableGuid, RsdPointer);
    gBS->InstallConfigurationTable(&gEfiAcpi10TableGuid, RsdPointer);
  }

  //free regions?
  while (gRegions) {
    tmpRegion = gRegions->next;
    FreePool(gRegions);
    gRegions = tmpRegion;
  }
  return EFI_SUCCESS;
}

/**
 * Searches for TableName in PathPatched dirs and loads it
 * to Buffer if found. Buffer is allocated here and should be released
 * by caller.
 */
EFI_STATUS LoadAcpiTable (
                          CONST CHAR16           *PathPatched,
                          CONST CHAR16           *TableName,
                          UINT8                 **Buffer,
                          UINTN                  *BufferLen
                          )
{
  EFI_STATUS            Status;

  Status = EFI_NOT_FOUND;

  // checking \EFI\ACPI\patched dir
  XStringW TmpStr = SWPrintf("%ls\\%ls", PathPatched, TableName);
  if (FileExists(&self.getCloverDir(), TmpStr)) {
    DBG("found %ls\n", TmpStr.wc_str());
    Status = egLoadFile(&self.getCloverDir(), TmpStr.wc_str(), Buffer, BufferLen);
  }
  return Status;
}

/**
 * Searches for DSDT in AcpiOemPath or PathPatched dirs and inserts it
 * to FadtPointer if found.
 */
EFI_STATUS LoadAndInjectDSDT(CONST CHAR16 *PathPatched,
                             EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *FadtPointer)
{
  EFI_STATUS            Status;
  UINT8                 *Buffer = NULL;
  UINTN                  BufferLen = 0;
  EFI_PHYSICAL_ADDRESS  Dsdt;

  // load if exists
  Status = LoadAcpiTable(PathPatched, gSettings.ACPI.DSDT.DsdtName.wc_str(), &Buffer, &BufferLen);

  if (!EFI_ERROR(Status)) {
    // loaded - allocate EfiACPIReclaim
    DBG("Loaded DSDT at \\%ls\\%ls\\%ls\n", self.getCloverDirFullPath().wc_str(), PathPatched, gSettings.ACPI.DSDT.DsdtName.wc_str());
    Dsdt = EFI_SYSTEM_TABLE_MAX_ADDRESS; //0xFE000000;
    Status = gBS->AllocatePages (
                                 AllocateMaxAddress,
                                 EfiACPIReclaimMemory,
                                 EFI_SIZE_TO_PAGES(BufferLen),
                                 &Dsdt
                                 );

    if(!EFI_ERROR(Status)) {
      // copy DSDT into EfiACPIReclaim block
      CopyMem((void*)(UINTN)Dsdt, Buffer, BufferLen);

      // update FADT
      FadtPointer->Dsdt  = (UINT32)Dsdt;
      FadtPointer->XDsdt = Dsdt;
      FixChecksum(&FadtPointer->Header);
	    DBG("DSDT at 0x%llX injected to FADT 0x%llx\n", Dsdt, (UINTN)FadtPointer);
    }

    if(Buffer) FreePool(Buffer);
  }

  return Status;
}

/**
 * Searches for TableName in AcpiOemPath or PathPatched dirs and inserts it
 * to Rsdt and/or Xsdt (globals) if found.
 */
EFI_STATUS LoadAndInjectAcpiTable(CONST CHAR16 *PathPatched,
                                  CONST CHAR16 *TableName)
{
  EFI_STATUS                    Status;
  UINT8                         *Buffer = NULL;
  UINTN                         BufferLen = 0;
  EFI_ACPI_DESCRIPTION_HEADER   *TableHeader = NULL;


  // load if exists
  Status = LoadAcpiTable(PathPatched, TableName, &Buffer, &BufferLen);

  if(!EFI_ERROR(Status)) {
    if (Buffer) {
      // if this is SLIC, then remove previous SLIC if it is there
      TableHeader = (EFI_ACPI_DESCRIPTION_HEADER*)Buffer;
      if (TableHeader->Signature == SLIC_SIGN) {
        DropTableFromXSDT(SLIC_SIGN, 0, 0);
        DropTableFromRSDT(SLIC_SIGN, 0, 0);
      }
    }

    // loaded - insert it into XSDT/RSDT
    Status = InsertTable(Buffer, BufferLen);

    if(!EFI_ERROR(Status)) {
      DBG("Table %ls inserted.\n", TableName);

      // if this was SLIC, then update IDs in XSDT/RSDT

      if (TableHeader->Signature == SLIC_SIGN) {
        if (Rsdt) {
			DBG("SLIC: Rsdt OEMid '%6.6s', TabId '%8.8s'", (CHAR8*)&Rsdt->Header.OemId, (CHAR8*)&Rsdt->Header.OemTableId);
          CopyMem(&Rsdt->Header.OemId, &TableHeader->OemId, 6);
          Rsdt->Header.OemTableId = TableHeader->OemTableId;
			DBG(" to OEMid '%6.6s', TabId '%8.8s'\n", (CHAR8*)&Rsdt->Header.OemId, (CHAR8*)&Rsdt->Header.OemTableId);
        }
        if (Xsdt) {
			DBG("SLIC: Xsdt OEMid '%6.6s', TabId '%8.8s'", (CHAR8*)&Xsdt->Header.OemId, (CHAR8*)&Xsdt->Header.OemTableId);
          CopyMem(&Xsdt->Header.OemId, &TableHeader->OemId, 6);
          Xsdt->Header.OemTableId = TableHeader->OemTableId;
			DBG(" to OEMid '%6.6s', TabId '%8.8s'\n", (CHAR8*)&Xsdt->Header.OemId, (CHAR8*)&Xsdt->Header.OemTableId);
        }
      }
    } else {
      DBG("Insert return status %s\n", efiStrError(Status));
    }

    FreePool(Buffer);
  } // if table loaded

  return Status;
}

/**
 * Patches UEFI ACPI tables with tables found in OsSubdir.
 */
EFI_STATUS PatchACPI_OtherOS(CONST CHAR16* OsSubdir, XBool DropSSDT)
{
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER    *RsdPointer;
  EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE       *FadtPointer;

  EFI_STATUS            Status; // = EFI_SUCCESS;
  //  UINTN                 Index;
  REFIT_DIR_ITER        DirIter;
  EFI_FILE_INFO         *DirEntry;

  //
  // Search for RSDP in UEFI SystemTable/ConfigTable (first Acpi 2.0, then 1.0)
  //
  RsdPointer = NULL;

  Status = EfiGetSystemConfigurationTable (&gEfiAcpi20TableGuid, (void **) &RsdPointer);
  if (RsdPointer != NULL) {
	  DBG("OtherOS: Found Acpi 2.0 RSDP 0x%llX\n", (uintptr_t)RsdPointer);
  } else {
    Status = EfiGetSystemConfigurationTable (&gEfiAcpi10TableGuid, (void **) &RsdPointer);
    if (RsdPointer != NULL) {
		DBG("Found Acpi 1.0 RSDP 0x%llX\n", (uintptr_t)RsdPointer);
    }
  }
  // if RSDP not found - quit
  if (!RsdPointer) {
    return Status;
  }

  //
  // Find RSDT and/or XSDT
  //
  Rsdt = (RSDT_TABLE*)(UINTN)(RsdPointer->RsdtAddress);
  if (Rsdt != NULL && Rsdt->Header.Signature != EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_TABLE_SIGNATURE) {
    Rsdt = NULL;
  }
  DBG("RSDT at 0x%llX\n", (UINTN)Rsdt);

  // check for XSDT
  Xsdt = NULL;
  if (RsdPointer->Revision >=2 && (RsdPointer->XsdtAddress < (UINT64)((UINTN)(-1)))) {
    Xsdt = (XSDT_TABLE*)(UINTN)RsdPointer->XsdtAddress;
    if (Xsdt != NULL && Xsdt->Header.Signature != EFI_ACPI_2_0_EXTENDED_SYSTEM_DESCRIPTION_TABLE_SIGNATURE) {
      Xsdt = NULL;
    }
  }
  DBG("XSDT at 0x%llX\n", (UINTN)Xsdt);

  // if RSDT and XSDT not found - quit
  if (Rsdt == NULL && Xsdt == NULL) {
    return EFI_UNSUPPORTED;
  }

  //
  // Take FADT (FACP) from XSDT or RSDT (always first entry)
  //
  FadtPointer = NULL;
  if (Xsdt) {
    FadtPointer = (EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE*)(UINTN)(Xsdt->Entry);
  } else if (Rsdt) {
    FadtPointer = (EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE*)(UINTN)(Rsdt->Entry);
  }
  DBG("FADT pointer = 0x%llX\n", (UINTN)FadtPointer);

  // if not found - quit
  if(FadtPointer == NULL) {
    return EFI_NOT_FOUND;
  }

  //
  // Inject/drop tables
  //

  // prepare dirs that will be searched for custom ACPI tables
  XStringW PathPatched;
  if ( selfOem.oemDirExists() ) {
    PathPatched = SWPrintf("%ls\\ACPI\\%ls", selfOem.getOemPathRelToSelfDir().wc_str(), OsSubdir);
    if ( !FileExists(&self.getCloverDir(), PathPatched) ) {
      PathPatched.setEmpty();
    }
  }
  if ( PathPatched.isEmpty() ) {
    PathPatched = SWPrintf("ACPI\\%ls", OsSubdir);
    if (!FileExists(&self.getCloverDir(), PathPatched)) {
      DBG("Dir '\\%ls\\%ls' not found. No patching will be done.\n", self.getCloverDirFullPath().wc_str(), PathPatched.wc_str());
      return EFI_NOT_FOUND;
    }
  }

  //
  // Inject DSDT
  //
  /* Status = */LoadAndInjectDSDT(PathPatched.wc_str(), FadtPointer);

  //
  // Drop SSDT if requested. Not until now
  //
  /*
   if (DropSSDT) {
   DropTableFromXSDT(EFI_ACPI_4_0_SECONDARY_SYSTEM_DESCRIPTION_TABLE_SIGNATURE, 0, 0);
   DropTableFromRSDT(EFI_ACPI_4_0_SECONDARY_SYSTEM_DESCRIPTION_TABLE_SIGNATURE, 0, 0);
   }
   */
  if (GlobalConfig.ACPIDropTables) {
    ACPI_DROP_TABLE *DropTable;
    DbgHeader("ACPIDropTables");
    for (DropTable = GlobalConfig.ACPIDropTables; DropTable; DropTable = DropTable->Next) {
      // only for tables that have OtherOS true
      if (DropTable->OtherOS && DropTable->MenuItem.BValue) {
        //DBG("Attempting to drop \"%4.4a\" (%8.8X) \"%8.8a\" (%16.16lX) L=%d\n", &(DropTable->Signature), DropTable->Signature, &(DropTable->TableId), DropTable->TableId, DropTable->Length);
        DropTableFromXSDT(DropTable->Signature, DropTable->TableId, DropTable->Length);
        DropTableFromRSDT(DropTable->Signature, DropTable->TableId, DropTable->Length);
      }
    }
  }

  //
  // find and inject other ACPI tables
  //

  DirIterOpen(&self.getCloverDir(), PathPatched.wc_str(), &DirIter);
  while (DirIterNext(&DirIter, 2, L"*.aml", &DirEntry)) {

    if (DirEntry->FileName[0] == L'.') {
      continue;
    }
    if (StrStr(DirEntry->FileName, L"DSDT")) {
      continue;
    }

    LoadAndInjectAcpiTable(PathPatched.wc_str(), DirEntry->FileName);
  }
  /*Status = */DirIterClose(&DirIter);

  // remove NULL entries from RSDT and XSDT
  PostCleanupRSDT();
  PostCleanupXSDT();

  return EFI_SUCCESS;
}

