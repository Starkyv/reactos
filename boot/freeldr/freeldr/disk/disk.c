/*
 *  FreeLoader
 *  Copyright (C) 1998-2003  Brian Palmer  <brianp@sginet.com>
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

#ifndef _M_ARM
#include <freeldr.h>

#include <debug.h>

DBG_DEFAULT_CHANNEL(DISK);

CHAR FrldrBootPath[MAX_PATH] = "";

static BOOLEAN bReportError = TRUE;

/* FUNCTIONS *****************************************************************/

VOID DiskReportError(BOOLEAN bError)
{
    bReportError = bError;
}

VOID DiskError(PCSTR ErrorString, ULONG ErrorCode)
{
    CHAR ErrorCodeString[200];

    if (bReportError == FALSE)
        return;

    sprintf(ErrorCodeString, "%s\n\nError Code: 0x%lx\nError: %s",
            ErrorString, ErrorCode, DiskGetErrorCodeString(ErrorCode));

    TRACE("%s\n", ErrorCodeString);

    UiMessageBox(ErrorCodeString);
}

PCSTR DiskGetErrorCodeString(ULONG ErrorCode)
{
    switch (ErrorCode)
    {
    case 0x00:  return "no error";
    case 0x01:  return "bad command passed to driver";
    case 0x02:  return "address mark not found or bad sector";
    case 0x03:  return "diskette write protect error";
    case 0x04:  return "sector not found";
    case 0x05:  return "fixed disk reset failed";
    case 0x06:  return "diskette changed or removed";
    case 0x07:  return "bad fixed disk parameter table";
    case 0x08:  return "DMA overrun";
    case 0x09:  return "DMA access across 64k boundary";
    case 0x0A:  return "bad fixed disk sector flag";
    case 0x0B:  return "bad fixed disk cylinder";
    case 0x0C:  return "unsupported track/invalid media";
    case 0x0D:  return "invalid number of sectors on fixed disk format";
    case 0x0E:  return "fixed disk controlled data address mark detected";
    case 0x0F:  return "fixed disk DMA arbitration level out of range";
    case 0x10:  return "ECC/CRC error on disk read";
    case 0x11:  return "recoverable fixed disk data error, data fixed by ECC";
    case 0x20:  return "controller error (NEC for floppies)";
    case 0x40:  return "seek failure";
    case 0x80:  return "time out, drive not ready";
    case 0xAA:  return "fixed disk drive not ready";
    case 0xBB:  return "fixed disk undefined error";
    case 0xCC:  return "fixed disk write fault on selected drive";
    case 0xE0:  return "fixed disk status error/Error reg = 0";
    case 0xFF:  return "sense operation failed";

    default:  return "unknown error code";
    }
}

BOOLEAN DiskIsDriveRemovable(UCHAR DriveNumber)
{
    /*
     * Hard disks use drive numbers >= 0x80 . So if the drive number
     * indicates a hard disk then return FALSE.
     * 0x49 is our magic ramdisk drive, so return FALSE for that too.
     */
    if ((DriveNumber >= 0x80) || (DriveNumber == 0x49))
        return FALSE;

    /* The drive is a floppy diskette so return TRUE */
    return TRUE;
}

BOOLEAN DiskGetBootPath(OUT PCHAR BootPath, IN ULONG Size)
{
    if (*FrldrBootPath)
        goto Done;

    if (Size)
        BootPath[0] = ANSI_NULL;

    /* 0x49 is our magic ramdisk drive, so try to detect it first */
    if (FrldrBootDrive == 0x49)
    {
        /* This is the ramdisk. See ArmDiskGetBootPath too... */
        // sprintf(FrldrBootPath, "ramdisk(%u)", 0);
        strcpy(FrldrBootPath, "ramdisk(0)");
    }
    else if (FrldrBootDrive < 0x80)
    {
        /* This is a floppy */
        sprintf(FrldrBootPath, "multi(0)disk(0)fdisk(%u)", FrldrBootDrive);
    }
    else if (FrldrBootPartition == 0xFF)
    {
        /* Boot Partition 0xFF is the magic value that indicates booting from CD-ROM (see isoboot.S) */
        sprintf(FrldrBootPath, "multi(0)disk(0)cdrom(%u)", FrldrBootDrive - 0x80);
    }
    else
    {
        ULONG BootPartition;
        PARTITION_TABLE_ENTRY PartitionEntry;

        /* This is a hard disk */
        if (!DiskGetBootPartitionEntry(FrldrBootDrive, &PartitionEntry, &BootPartition))
        {
            ERR("Failed to get boot partition entry\n");
            return FALSE;
        }

        FrldrBootPartition = BootPartition;

        sprintf(FrldrBootPath, "multi(0)disk(0)rdisk(%u)partition(%lu)",
                FrldrBootDrive - 0x80, FrldrBootPartition);
    }

Done:
    /* Copy back the buffer */
    if (Size < strlen(FrldrBootPath) + 1)
        return FALSE;
    strncpy(BootPath, FrldrBootPath, Size);
    return TRUE;
}

#endif
