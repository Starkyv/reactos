/*
 *  FreeLoader
 *
 *  Copyright (C) 2003, 2004  Eric Kohl
 *  Copyright (C) 2009  Herv� Poussineau
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <freeldr.h>

#include <debug.h>

DBG_DEFAULT_CHANNEL(HWDETECT);

/*
 * This is the common code for harddisk for both the PC and the XBOX.
 */

typedef struct tagDISKCONTEXT
{
    UCHAR DriveNumber;
    ULONG SectorSize;
    ULONGLONG SectorOffset;
    ULONGLONG SectorCount;
    ULONGLONG SectorNumber;
} DISKCONTEXT;

static const CHAR Hex[] = "0123456789abcdef";

/* Data cache for BIOS disks pre-enumeration */
UCHAR PcBiosDiskCount = 0;
static CHAR PcDiskIdentifier[32][20];

PVOID DiskReadBuffer;
SIZE_T DiskReadBufferSize;


/* FUNCTIONS *****************************************************************/

static ARC_STATUS
DiskClose(ULONG FileId)
{
    DISKCONTEXT* Context = FsGetDeviceSpecific(FileId);

    FrLdrTempFree(Context, TAG_HW_DISK_CONTEXT);
    return ESUCCESS;
}

static ARC_STATUS
DiskGetFileInformation(ULONG FileId, FILEINFORMATION* Information)
{
    DISKCONTEXT* Context = FsGetDeviceSpecific(FileId);

    RtlZeroMemory(Information, sizeof(*Information));
    Information->EndingAddress.QuadPart = Context->SectorCount * Context->SectorSize;
    Information->CurrentAddress.QuadPart = Context->SectorNumber * Context->SectorSize;

    return ESUCCESS;
}

static ARC_STATUS
DiskOpen(CHAR* Path, OPENMODE OpenMode, ULONG* FileId)
{
    DISKCONTEXT* Context;
    UCHAR DriveNumber;
    ULONG DrivePartition, SectorSize;
    ULONGLONG SectorOffset = 0;
    ULONGLONG SectorCount = 0;
    PARTITION_TABLE_ENTRY PartitionTableEntry;
    CHAR FileName[1];

    if (DiskReadBufferSize == 0)
    {
        ERR("DiskOpen(): DiskReadBufferSize is 0, something is wrong.\n");
        ASSERT(FALSE);
        return ENOMEM;
    }

    if (!DissectArcPath(Path, FileName, &DriveNumber, &DrivePartition))
        return EINVAL;

    if (DrivePartition == 0xff)
    {
        /* This is a CD-ROM device */
        SectorSize = 2048;
    }
    else
    {
        /*
         * This is either a floppy disk device (DrivePartition == 0) or
         * a hard disk device (DrivePartition != 0 && DrivePartition != 0xFF)
         * but it doesn't matter which one because they both have 512 bytes
         * per sector.
         */
        SectorSize = 512;
    }

    if (DrivePartition != 0xff && DrivePartition != 0)
    {
        if (!DiskGetPartitionEntry(DriveNumber, DrivePartition, &PartitionTableEntry))
            return EINVAL;

        SectorOffset = PartitionTableEntry.SectorCountBeforePartition;
        SectorCount = PartitionTableEntry.PartitionSectorCount;
    }
#if 0 // FIXME: Investigate
    else
    {
        SectorCount = 0; /* FIXME */
    }
#endif

    Context = FrLdrTempAlloc(sizeof(DISKCONTEXT), TAG_HW_DISK_CONTEXT);
    if (!Context)
        return ENOMEM;

    Context->DriveNumber = DriveNumber;
    Context->SectorSize = SectorSize;
    Context->SectorOffset = SectorOffset;
    Context->SectorCount = SectorCount;
    Context->SectorNumber = 0;
    FsSetDeviceSpecific(*FileId, Context);

    return ESUCCESS;
}

static ARC_STATUS
DiskRead(ULONG FileId, VOID* Buffer, ULONG N, ULONG* Count)
{
    DISKCONTEXT* Context = FsGetDeviceSpecific(FileId);
    UCHAR* Ptr = (UCHAR*)Buffer;
    ULONG Length, TotalSectors, MaxSectors, ReadSectors;
    BOOLEAN ret;
    ULONGLONG SectorOffset;

    ASSERT(DiskReadBufferSize > 0);

    TotalSectors = (N + Context->SectorSize - 1) / Context->SectorSize;
    MaxSectors   = DiskReadBufferSize / Context->SectorSize;
    SectorOffset = Context->SectorNumber + Context->SectorOffset;

    // If MaxSectors is 0, this will lead to infinite loop
    // In release builds assertions are disabled, however we also have sanity checks in DiskOpen()
    ASSERT(MaxSectors > 0);

    ret = TRUE;

    while (TotalSectors)
    {
        ReadSectors = TotalSectors;
        if (ReadSectors > MaxSectors)
            ReadSectors = MaxSectors;

        ret = MachDiskReadLogicalSectors(Context->DriveNumber,
                                         SectorOffset,
                                         ReadSectors,
                                         DiskReadBuffer);
        if (!ret)
            break;

        Length = ReadSectors * Context->SectorSize;
        if (Length > N)
            Length = N;

        RtlCopyMemory(Ptr, DiskReadBuffer, Length);

        Ptr += Length;
        N -= Length;
        SectorOffset += ReadSectors;
        TotalSectors -= ReadSectors;
    }

    *Count = (ULONG)(Ptr - (UCHAR*)Buffer);

    return (!ret) ? EIO : ESUCCESS;
}

static ARC_STATUS
DiskSeek(ULONG FileId, LARGE_INTEGER* Position, SEEKMODE SeekMode)
{
    DISKCONTEXT* Context = FsGetDeviceSpecific(FileId);

    if (SeekMode != SeekAbsolute)
        return EINVAL;
    if (Position->LowPart & (Context->SectorSize - 1))
        return EINVAL;

    Context->SectorNumber = Position->QuadPart / Context->SectorSize;
    return ESUCCESS;
}

static const DEVVTBL DiskVtbl =
{
    DiskClose,
    DiskGetFileInformation,
    DiskOpen,
    DiskRead,
    DiskSeek,
};


PCHAR
GetHarddiskIdentifier(UCHAR DriveNumber)
{
    return PcDiskIdentifier[DriveNumber - 0x80];
}

static VOID
GetHarddiskInformation(UCHAR DriveNumber)
{
    PMASTER_BOOT_RECORD Mbr;
    PULONG Buffer;
    ULONG i;
    ULONG Checksum;
    ULONG Signature;
    BOOLEAN ValidPartitionTable;
    CHAR ArcName[MAX_PATH];
    PARTITION_TABLE_ENTRY PartitionTableEntry;
    PCHAR Identifier = PcDiskIdentifier[DriveNumber - 0x80];

    /* Detect disk partition type */
    DiskDetectPartitionType(DriveNumber);

    /* Read the MBR */
    if (!MachDiskReadLogicalSectors(DriveNumber, 0ULL, 1, DiskReadBuffer))
    {
        ERR("Reading MBR failed\n");
        /* We failed, use a default identifier */
        sprintf(Identifier, "BIOSDISK%d", DriveNumber - 0x80 + 1);
        return;
    }

    Buffer = (ULONG*)DiskReadBuffer;
    Mbr = (PMASTER_BOOT_RECORD)DiskReadBuffer;

    Signature = Mbr->Signature;
    TRACE("Signature: %x\n", Signature);

    /* Calculate the MBR checksum */
    Checksum = 0;
    for (i = 0; i < 512 / sizeof(ULONG); i++)
    {
        Checksum += Buffer[i];
    }
    Checksum = ~Checksum + 1;
    TRACE("Checksum: %x\n", Checksum);

    ValidPartitionTable = (Mbr->MasterBootRecordMagic == 0xAA55);

    /* Fill out the ARC disk block */
    sprintf(ArcName, "multi(0)disk(0)rdisk(%u)", DriveNumber - 0x80);
    AddReactOSArcDiskInfo(ArcName, Signature, Checksum, ValidPartitionTable);

    sprintf(ArcName, "multi(0)disk(0)rdisk(%u)partition(0)", DriveNumber - 0x80);
    FsRegisterDevice(ArcName, &DiskVtbl);

    /* Add partitions */
    i = 1;
    DiskReportError(FALSE);
    while (DiskGetPartitionEntry(DriveNumber, i, &PartitionTableEntry))
    {
        if (PartitionTableEntry.SystemIndicator != PARTITION_ENTRY_UNUSED)
        {
            sprintf(ArcName, "multi(0)disk(0)rdisk(%u)partition(%lu)", DriveNumber - 0x80, i);
            FsRegisterDevice(ArcName, &DiskVtbl);
        }
        i++;
    }
    DiskReportError(TRUE);

    /* Convert checksum and signature to identifier string */
    Identifier[0] = Hex[(Checksum >> 28) & 0x0F];
    Identifier[1] = Hex[(Checksum >> 24) & 0x0F];
    Identifier[2] = Hex[(Checksum >> 20) & 0x0F];
    Identifier[3] = Hex[(Checksum >> 16) & 0x0F];
    Identifier[4] = Hex[(Checksum >> 12) & 0x0F];
    Identifier[5] = Hex[(Checksum >> 8) & 0x0F];
    Identifier[6] = Hex[(Checksum >> 4) & 0x0F];
    Identifier[7] = Hex[Checksum & 0x0F];
    Identifier[8] = '-';
    Identifier[9] = Hex[(Signature >> 28) & 0x0F];
    Identifier[10] = Hex[(Signature >> 24) & 0x0F];
    Identifier[11] = Hex[(Signature >> 20) & 0x0F];
    Identifier[12] = Hex[(Signature >> 16) & 0x0F];
    Identifier[13] = Hex[(Signature >> 12) & 0x0F];
    Identifier[14] = Hex[(Signature >> 8) & 0x0F];
    Identifier[15] = Hex[(Signature >> 4) & 0x0F];
    Identifier[16] = Hex[Signature & 0x0F];
    Identifier[17] = '-';
    Identifier[18] = (ValidPartitionTable ? 'A' : 'X');
    Identifier[19] = 0;
    TRACE("Identifier: %s\n", Identifier);
}

static UCHAR
EnumerateHarddisks(OUT PBOOLEAN BootDriveReported)
{
    UCHAR DiskCount, DriveNumber;
    ULONG i;
    BOOLEAN Changed;

    *BootDriveReported = FALSE;

    /* Count the number of visible harddisk drives */
    DiskReportError(FALSE);
    DiskCount = 0;
    DriveNumber = 0x80;

    /*
     * There are some really broken BIOSes out there. There are even BIOSes
     * that happily report success when you ask them to read from non-existent
     * harddisks. So, we set the buffer to known contents first, then try to
     * read. If the BIOS reports success but the buffer contents haven't
     * changed then we fail anyway.
     */
    memset(DiskReadBuffer, 0xcd, DiskReadBufferSize);
    while (MachDiskReadLogicalSectors(DriveNumber, 0ULL, 1, DiskReadBuffer))
    {
        Changed = FALSE;
        for (i = 0; !Changed && i < DiskReadBufferSize; i++)
        {
            Changed = ((PUCHAR)DiskReadBuffer)[i] != 0xcd;
        }
        if (!Changed)
        {
            TRACE("BIOS reports success for disk %d (0x%02X) but data didn't change\n",
                  (int)DiskCount, DriveNumber);
            break;
        }

        /* Cache the BIOS hard disk information for later use */
        GetHarddiskInformation(DriveNumber);

        /* Check if we have seen the boot drive */
        if (FrldrBootDrive == DriveNumber)
            *BootDriveReported = TRUE;

        DiskCount++;
        DriveNumber++;
        memset(DiskReadBuffer, 0xcd, DiskReadBufferSize);
    }
    DiskReportError(TRUE);

    PcBiosDiskCount = DiskCount;
    TRACE("BIOS reports %d harddisk%s\n",
          (int)DiskCount, (DiskCount == 1) ? "" : "s");

    return DiskCount;
}

BOOLEAN
PcInitializeBootDevices(VOID)
{
    UCHAR DiskCount;
    BOOLEAN BootDriveReported = FALSE;
    ULONG i;
    CHAR BootPath[MAX_PATH];

    DiskCount = EnumerateHarddisks(&BootDriveReported);

    /* Get the drive we're booting from */
    MachDiskGetBootPath(BootPath, sizeof(BootPath));

    /* Add it, if it's a floppy or cdrom */
    if ((FrldrBootDrive >= 0x80 && !BootDriveReported) ||
        DiskIsDriveRemovable(FrldrBootDrive))
    {
        /* TODO: Check if it's really a CDROM drive */

        PMASTER_BOOT_RECORD Mbr;
        PULONG Buffer;
        ULONG Checksum = 0;
        ULONG Signature;

        /* Read the MBR */
        if (!MachDiskReadLogicalSectors(FrldrBootDrive, 16ULL, 1, DiskReadBuffer))
        {
            ERR("Reading MBR failed\n");
            return FALSE;
        }

        Buffer = (ULONG*)DiskReadBuffer;
        Mbr = (PMASTER_BOOT_RECORD)DiskReadBuffer;

        Signature = Mbr->Signature;
        TRACE("Signature: %x\n", Signature);

        /* Calculate the MBR checksum */
        for (i = 0; i < 2048 / sizeof(ULONG); i++)
        {
            Checksum += Buffer[i];
        }
        Checksum = ~Checksum + 1;
        TRACE("Checksum: %x\n", Checksum);

        /* Fill out the ARC disk block */
        AddReactOSArcDiskInfo(BootPath, Signature, Checksum, TRUE);

        FsRegisterDevice(BootPath, &DiskVtbl);
        DiskCount++; // This is not accounted for in the number of pre-enumerated BIOS drives!
        TRACE("Additional boot drive detected: 0x%02X\n", (int)FrldrBootDrive);
    }

    return (DiskCount != 0);
}
