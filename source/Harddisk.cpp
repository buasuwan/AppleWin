/*
AppleWin : An Apple //e emulator for Windows

Copyright (C) 1994-1996, Michael O'Brien
Copyright (C) 1999-2001, Oliver Schmidt
Copyright (C) 2002-2005, Tom Charlesworth
Copyright (C) 2006-2015, Tom Charlesworth, Michael Pohoreski

AppleWin is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

AppleWin is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with AppleWin; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/* Description: Hard drive emulation
 *
 * Author: Copyright (c) 2005, Robert Hoem
 */

#include "StdAfx.h"

#include "Harddisk.h"
#include "Core.h"
#include "Interface.h"
#include "CardManager.h"
#include "CPU.h"
#include "DiskImage.h"	// ImageError_e, Disk_Status_e
#include "Memory.h"
#include "Registry.h"
#include "SaveState.h"
#include "YamlHelper.h"

#include "../resource/resource.h"

/*
Memory map:

    C0F0	(r)   EXECUTE AND RETURN STATUS
	C0F1	(r)   STATUS (or ERROR)
	C0F2	(r/w) COMMAND
	C0F3	(r/w) UNIT NUMBER
	C0F4	(r/w) LOW BYTE OF MEMORY BUFFER
	C0F5	(r/w) HIGH BYTE OF MEMORY BUFFER
	C0F6	(r/w) LOW BYTE OF BLOCK NUMBER
	C0F7	(r/w) HIGH BYTE OF BLOCK NUMBER
	C0F8    (r)   NEXT BYTE
*/

/*
Hard drive emulation in Applewin.

Concept
    To emulate a 32mb hard drive connected to an Apple IIe via Applewin.
    Designed to work with Autoboot Rom and Prodos.

Overview
  1. Hard drive image file
      The hard drive image file (.HDV) will be formatted into blocks of 512
      bytes, in a linear fashion. The internal formatting and meaning of each
      block to be decided by the Apple's operating system (ProDos). To create
      an empty .HDV file, just create a 0 byte file (I prefer the debug method).
  
  2. Emulation code
      There are 4 commands Prodos will send to a block device.
      Listed below are each command and how it's handled:

      1. STATUS
          In the emulation's case, returns only a DEVICE OK (0) or DEVICE I/O ERROR (8).
          DEVICE I/O ERROR only returned if no HDV file is selected.

      2. READ
          Loads requested block into a 512 byte buffer by attempting to seek to
            location in HDV file.
          If seek fails, returns a DEVICE I/O ERROR.  Resets hd_buf_ptr used by HD_NEXTBYTE
          Returns a DEVICE OK if read was successful, or a DEVICE I/O ERROR otherwise.

      3. WRITE
          Copies requested block from the Apple's memory to a 512 byte buffer
            then attempts to seek to requested block.
          If the seek fails (usually because the seek is beyond the EOF for the
            HDV file), the Emulation will attempt to "grow" the HDV file to accomodate.
            Once the file can accomodate, or if the seek did not fail, the buffer is
            written to the HDV file.  NOTE: A2PC will grow *AND* shrink the HDV file.
          I didn't see the point in shrinking the file as this behaviour would require
            patching prodos (to detect DELETE FILE calls).

      4. FORMAT
          Ignored.  This would be used for low level formatting of the device
            (as in the case of a tape or SCSI drive, perhaps).

  3. Bugs
      The only thing I've noticed is that Copy II+ 7.1 seems to crash or stall
      occasionally when trying to calculate how many free block are available
      when running a catalog.  This might be due to the great number of blocks
      available.  Also, DDD pro will not optimise the disk correctally (it's
      doing a disk defragment of some sort, and when it requests a block outside
      the range of the image file, it starts getting I/O errors), so don't
      bother.  Any program that preforms a read before write to an "unwritten"
      block (a block that should be located beyond the EOF of the .HDV, which is
      valid for writing but not for reading until written to) will fail with I/O
      errors (although these are few and far between).

      I'm sure there are programs out there that may try to use the I/O ports in
      ways they weren't designed (like telling Ultima 5 that you have a Phasor
      sound card in slot 7 is a generally bad idea) will cause problems.
*/



HarddiskInterfaceCard::HarddiskInterfaceCard(UINT slot) :
	Card(CT_GenericHDD, slot)
{
	m_unitNum = HARDDISK_1 << 7;	// b7=unit

	// The HDD interface has a single Command register for both drives:
	// . ProDOS will write to Command before switching drives
	m_command = 0;

	m_saveDiskImage = true;	// Save the DiskImage name to Registry

	// if created by user in Config->Disk, then MemInitializeIO() won't be called
	if (GetCxRomPeripheral())
		InitializeIO(GetCxRomPeripheral());	// During regular start-up, Initialize() will be called later by MemInitializeIO()
}

HarddiskInterfaceCard::~HarddiskInterfaceCard(void)
{
	CleanupDriveInternal(HARDDISK_1);
	CleanupDriveInternal(HARDDISK_2);

	// if destroyed by user in Config->Disk, then ensure that old object's reference is removed
	UnregisterIoHandler(m_slot);
}

void HarddiskInterfaceCard::Reset(const bool powerCycle)
{
	m_hardDiskDrive[HARDDISK_1].m_error = 0;
	m_hardDiskDrive[HARDDISK_2].m_error = 0;
}

//===========================================================================

void HarddiskInterfaceCard::InitializeIO(const LPBYTE pCxRomPeripheral)
{
	const DWORD HARDDISK_FW_SIZE = APPLE_SLOT_SIZE;

	BYTE* pData = GetFrame().GetResource(IDR_HDDRVR_FW, "FIRMWARE", HARDDISK_FW_SIZE);
	if (pData == NULL)
		return;

	memcpy(pCxRomPeripheral + m_slot * APPLE_SLOT_SIZE, pData, HARDDISK_FW_SIZE);

	RegisterIoHandler(m_slot, IORead, IOWrite, NULL, NULL, this, NULL);
}

//===========================================================================

void HarddiskInterfaceCard::CleanupDriveInternal(const int iDrive)
{
	if (m_hardDiskDrive[iDrive].m_imagehandle)
	{
		ImageClose(m_hardDiskDrive[iDrive].m_imagehandle);
		m_hardDiskDrive[iDrive].m_imagehandle = NULL;
	}

	m_hardDiskDrive[iDrive].m_imageloaded = false;

	m_hardDiskDrive[iDrive].m_imagename.clear();
	m_hardDiskDrive[iDrive].m_fullname.clear();
	m_hardDiskDrive[iDrive].m_strFilenameInZip.clear();
}

void HarddiskInterfaceCard::CleanupDrive(const int iDrive)
{
	CleanupDriveInternal(iDrive);

	SaveLastDiskImage(iDrive);
}

//===========================================================================

void HarddiskInterfaceCard::NotifyInvalidImage(TCHAR* pszImageFilename)
{
	// TC: TO DO
}

//===========================================================================

void HarddiskInterfaceCard::LoadLastDiskImage(const int drive)
{
	_ASSERT(drive == HARDDISK_1 || drive == HARDDISK_2);

	const std::string regKey = (drive == HARDDISK_1)
		? REGVALUE_LAST_HARDDISK_1
		: REGVALUE_LAST_HARDDISK_2;

	char pathname[MAX_PATH];

	std::string& regSection = RegGetConfigSlotSection(m_slot);
	if (RegLoadString(regSection.c_str(), regKey.c_str(), TRUE, pathname, MAX_PATH, TEXT("")))
	{
		m_saveDiskImage = false;
		Insert(drive, pathname);
		m_saveDiskImage = true;
	}
}

//===========================================================================

void HarddiskInterfaceCard::SaveLastDiskImage(const int drive)
{
	_ASSERT(drive == HARDDISK_1 || drive == HARDDISK_2);

	if (!m_saveDiskImage)
		return;

	std::string& regSection = RegGetConfigSlotSection(m_slot);
	RegSaveValue(regSection.c_str(), REGVALUE_CARD_TYPE, TRUE, CT_GenericHDD);

	const std::string regKey = (drive == HARDDISK_1)
		? REGVALUE_LAST_HARDDISK_1
		: REGVALUE_LAST_HARDDISK_2;

	const std::string& pathName = HarddiskGetFullPathName(drive);

	RegSaveString(regSection.c_str(), regKey.c_str(), TRUE, pathName);

	//

	// For now, only update 'HDV Starting Directory' for slot7 & drive1
	// . otherwise you'll get inconsistent results if you set drive1, then drive2 (and the images were in different folders)
	if (m_slot != SLOT7 || drive != HARDDISK_1)
		return;

	TCHAR szPathName[MAX_PATH];
	StringCbCopy(szPathName, MAX_PATH, pathName.c_str());
	TCHAR* slash = _tcsrchr(szPathName, PATH_SEPARATOR);
	if (slash != NULL)
	{
		slash[1] = '\0';
		RegSaveString(REG_PREFS, REGVALUE_PREF_HDV_START_DIR, 1, szPathName);
	}
}

//===========================================================================

const std::string& HarddiskInterfaceCard::GetFullName(const int iDrive)
{
	return m_hardDiskDrive[iDrive].m_fullname;
}

const std::string& HarddiskInterfaceCard::HarddiskGetFullPathName(const int iDrive)
{
	return ImageGetPathname(m_hardDiskDrive[iDrive].m_imagehandle);
}

const std::string& HarddiskInterfaceCard::DiskGetBaseName(const int iDrive)
{
	return m_hardDiskDrive[iDrive].m_imagename;
}

void HarddiskInterfaceCard::GetFilenameAndPathForSaveState(std::string& filename, std::string& path)
{
	filename = "";
	path = "";

	for (UINT i=HARDDISK_1; i<=HARDDISK_2; i++)
	{
		if (!m_hardDiskDrive[i].m_imageloaded)
			continue;

		filename = DiskGetBaseName(i);
		std::string pathname = HarddiskGetFullPathName(i);

		int idx = pathname.find_last_of(PATH_SEPARATOR);
		if (idx >= 0 && idx+1 < (int)pathname.length())	// path exists?
		{
			path = pathname.substr(0, idx+1);
			return;
		}

		_ASSERT(0);
		break;
	}
}

//===========================================================================

void HarddiskInterfaceCard::Destroy(void)
{
	m_saveDiskImage = false;
	CleanupDrive(HARDDISK_1);

	m_saveDiskImage = false;
	CleanupDrive(HARDDISK_2);

	m_saveDiskImage = true;
}

//===========================================================================

// Pre: pathname likely to include path (but can also just be filename)
BOOL HarddiskInterfaceCard::Insert(const int iDrive, const std::string& pathname)
{
	if (pathname.empty())
		return FALSE;

	if (m_hardDiskDrive[iDrive].m_imageloaded)
		Unplug(iDrive);

	// Check if image is being used by the other HDD, and unplug it in order to be swapped
	{
		const std::string & pszOtherPathname = HarddiskGetFullPathName(!iDrive);

		char szCurrentPathname[MAX_PATH]; 
		DWORD uNameLen = GetFullPathName(pathname.c_str(), MAX_PATH, szCurrentPathname, NULL);
		if (uNameLen == 0 || uNameLen >= MAX_PATH)
			strcpy_s(szCurrentPathname, MAX_PATH, pathname.c_str());

		if (!strcmp(pszOtherPathname.c_str(), szCurrentPathname))
		{
			Unplug(!iDrive);
			GetFrame().FrameRefreshStatus(DRAW_LEDS | DRAW_DISK_STATUS);
		}
	}

	const bool bCreateIfNecessary = false;		// NB. Don't allow creation of HDV files
	const bool bExpectFloppy = false;
	const bool bIsHarddisk = true;
	ImageError_e Error = ImageOpen(pathname,
		&m_hardDiskDrive[iDrive].m_imagehandle,
		&m_hardDiskDrive[iDrive].m_bWriteProtected,
		bCreateIfNecessary,
		m_hardDiskDrive[iDrive].m_strFilenameInZip,	// TODO: Use this
		bExpectFloppy);

	m_hardDiskDrive[iDrive].m_imageloaded = (Error == eIMAGE_ERROR_NONE);

	m_hardDiskDrive[iDrive].m_status_next = DISK_STATUS_OFF;
	m_hardDiskDrive[iDrive].m_status_prev = DISK_STATUS_OFF;

	if (Error == eIMAGE_ERROR_NONE)
	{
		GetImageTitle(pathname.c_str(), m_hardDiskDrive[iDrive].m_imagename, m_hardDiskDrive[iDrive].m_fullname);
		Snapshot_UpdatePath();
	}

	SaveLastDiskImage(iDrive);

	return m_hardDiskDrive[iDrive].m_imageloaded;
}

//-----------------------------------------------------------------------------

bool HarddiskInterfaceCard::SelectImage(const int drive, LPCSTR pszFilename)
{
	TCHAR directory[MAX_PATH];
	TCHAR filename[MAX_PATH];
	TCHAR title[40];

	StringCbCopy(filename, MAX_PATH, pszFilename);

	RegLoadString(TEXT(REG_PREFS), TEXT(REGVALUE_PREF_HDV_START_DIR), 1, directory, MAX_PATH, TEXT(""));
	StringCbPrintf(title, 40, TEXT("Select HDV Image For HDD %d"), drive + 1);

	OPENFILENAME ofn;
	memset(&ofn, 0, sizeof(OPENFILENAME));
	ofn.lStructSize     = sizeof(OPENFILENAME);
	ofn.hwndOwner       = GetFrame().g_hFrameWindow;
	ofn.hInstance       = GetFrame().g_hInstance;
	ofn.lpstrFilter     = TEXT("Hard Disk Images (*.hdv,*.po,*.2mg,*.2img,*.gz,*.zip)\0*.hdv;*.po;*.2mg;*.2img;*.gz;*.zip\0")
						  TEXT("All Files\0*.*\0");
	ofn.lpstrFile       = filename;
	ofn.nMaxFile        = MAX_PATH;
	ofn.lpstrInitialDir = directory;
	ofn.Flags           = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;	// Don't allow creation & hide the read-only checkbox
	ofn.lpstrTitle      = title;

	bool bRes = false;

	if (GetOpenFileName(&ofn))
	{
		if ((!ofn.nFileExtension) || !filename[ofn.nFileExtension])
			StringCbCat(filename, MAX_PATH, TEXT(".hdv"));
		
		if (Insert(drive, filename))
		{
			bRes = true;
		}
		else
		{
			NotifyInvalidImage(filename);
		}
	}

	return bRes;
}

bool HarddiskInterfaceCard::Select(const int iDrive)
{
	return SelectImage(iDrive, TEXT(""));
}

//===========================================================================

void HarddiskInterfaceCard::Unplug(const int iDrive)
{
	if (m_hardDiskDrive[iDrive].m_imageloaded)
	{
		CleanupDrive(iDrive);
		Snapshot_UpdatePath();
	}
}

bool HarddiskInterfaceCard::IsDriveUnplugged(const int iDrive)
{
	return m_hardDiskDrive[iDrive].m_imageloaded == false;
}

//===========================================================================

#define DEVICE_OK				0x00
#define DEVICE_UNKNOWN_ERROR	0x28
#define DEVICE_IO_ERROR			0x27

BYTE __stdcall HarddiskInterfaceCard::IORead(WORD pc, WORD addr, BYTE bWrite, BYTE d, ULONG nExecutedCycles)
{
	const UINT slot = ((addr & 0xff) >> 4) - 8;
	HarddiskInterfaceCard* pCard = (HarddiskInterfaceCard*)MemGetSlotParameters(slot);
	HardDiskDrive* pHDD = &(pCard->m_hardDiskDrive[pCard->m_unitNum >> 7]);	// bit7 = drive select

	BYTE r = DEVICE_OK;
	pHDD->m_status_next = DISK_STATUS_READ;

	switch (addr & 0xF)
	{
		case 0x0:
			if (pHDD->m_imageloaded)
			{
				// based on loaded data block request, load block into memory
				// returns status
				switch (pCard->m_command)
				{
					default:
					case 0x00: //status
						if (ImageGetImageSize(pHDD->m_imagehandle) == 0)
						{
							pHDD->m_error = 1;
							r = DEVICE_IO_ERROR;
						}
						break;
					case 0x01: //read
						if ((pHDD->m_diskblock * HD_BLOCK_SIZE) < ImageGetImageSize(pHDD->m_imagehandle))
						{
							bool bRes = ImageReadBlock(pHDD->m_imagehandle, pHDD->m_diskblock, pHDD->m_buf);
							if (bRes)
							{
								pHDD->m_error = 0;
								r = 0;
								pHDD->m_buf_ptr = 0;
							}
							else
							{
								pHDD->m_error = 1;
								r = DEVICE_IO_ERROR;
							}
						}
						else
						{
							pHDD->m_error = 1;
							r = DEVICE_IO_ERROR;
						}
						break;
					case 0x02: //write
						{
							pHDD->m_status_next = DISK_STATUS_WRITE;
							bool bRes = true;
							const bool bAppendBlocks = (pHDD->m_diskblock * HD_BLOCK_SIZE) >= ImageGetImageSize(pHDD->m_imagehandle);

							if (bAppendBlocks)
							{
								memset(pHDD->m_buf, 0, HD_BLOCK_SIZE);

								// Inefficient (especially for gzip/zip files!)
								UINT uBlock = ImageGetImageSize(pHDD->m_imagehandle) / HD_BLOCK_SIZE;
								while (uBlock < pHDD->m_diskblock)
								{
									bRes = ImageWriteBlock(pHDD->m_imagehandle, uBlock++, pHDD->m_buf);
									_ASSERT(bRes);
									if (!bRes)
										break;
								}
							}

							memmove(pHDD->m_buf, mem+pHDD->m_memblock, HD_BLOCK_SIZE);

							if (bRes)
								bRes = ImageWriteBlock(pHDD->m_imagehandle, pHDD->m_diskblock, pHDD->m_buf);

							if (bRes)
							{
								pHDD->m_error = 0;
								r = 0;
							}
							else
							{
								pHDD->m_error = 1;
								r = DEVICE_IO_ERROR;
							}
						}
						break;
					case 0x03: //format
						pHDD->m_status_next = DISK_STATUS_WRITE;
						break;
				}
			}
			else
			{
				pHDD->m_status_next = DISK_STATUS_OFF;
				pHDD->m_error = 1;
				r = DEVICE_UNKNOWN_ERROR;
			}
		break;
	case 0x1: // m_error
		pHDD->m_status_next = DISK_STATUS_OFF; // TODO: FIXME: ??? YELLOW ??? WARNING
		if (pHDD->m_error)
		{
			_ASSERT(pHDD->m_error & 1);
			pHDD->m_error |= 1;	// Firmware requires that b0=1 for an error
		}

		r = pHDD->m_error;
		break;
	case 0x2:
		r = pCard->m_command;
		break;
	case 0x3:
		r = pCard->m_unitNum;
		break;
	case 0x4:
		r = (BYTE)(pHDD->m_memblock & 0x00FF);
		break;
	case 0x5:
		r = (BYTE)(pHDD->m_memblock & 0xFF00 >> 8);
		break;
	case 0x6:
		r = (BYTE)(pHDD->m_diskblock & 0x00FF);
		break;
	case 0x7:
		r = (BYTE)(pHDD->m_diskblock & 0xFF00 >> 8);
		break;
	case 0x8:
		r = pHDD->m_buf[pHDD->m_buf_ptr];
		if (pHDD->m_buf_ptr < sizeof(pHDD->m_buf)-1)
			pHDD->m_buf_ptr++;
		break;
	default:
		pHDD->m_status_next = DISK_STATUS_OFF;
		r = IO_Null(pc, addr, bWrite, d, nExecutedCycles);
	}

	pCard->UpdateLightStatus(pHDD);
	return r;
}

//-----------------------------------------------------------------------------

BYTE __stdcall HarddiskInterfaceCard::IOWrite(WORD pc, WORD addr, BYTE bWrite, BYTE d, ULONG nExecutedCycles)
{
	const UINT slot = ((addr & 0xff) >> 4) - 8;
	HarddiskInterfaceCard* pCard = (HarddiskInterfaceCard*)MemGetSlotParameters(slot);
	HardDiskDrive* pHDD = &(pCard->m_hardDiskDrive[pCard->m_unitNum >> 7]);	// bit7 = drive select

	BYTE r = DEVICE_OK;
	pHDD->m_status_next = DISK_STATUS_PROT; // TODO: FIXME: If we ever enable write-protect on HD then need to change to something else ...

	switch (addr & 0xF)
	{
	case 0x2:
		pCard->m_command = d;
		break;
	case 0x3:
		// b7    = drive#
		// b6..4 = slot#
		// b3..0 = ?
		pCard->m_unitNum = d;
		break;
	case 0x4:
		pHDD->m_memblock = (pHDD->m_memblock & 0xFF00) | d;
		break;
	case 0x5:
		pHDD->m_memblock = (pHDD->m_memblock & 0x00FF) | (d << 8);
		break;
	case 0x6:
		pHDD->m_diskblock = (pHDD->m_diskblock & 0xFF00) | d;
		break;
	case 0x7:
		pHDD->m_diskblock = (pHDD->m_diskblock & 0x00FF) | (d << 8);
		break;
	default:
		pHDD->m_status_next = DISK_STATUS_OFF;
		r = IO_Null(pc, addr, bWrite, d, nExecutedCycles);
	}

	pCard->UpdateLightStatus(pHDD);
	return r;
}

//===========================================================================

void HarddiskInterfaceCard::UpdateLightStatus(HardDiskDrive* pHDD)
{
	if (pHDD->m_status_prev != pHDD->m_status_next) // Update LEDs if state changes
	{
		pHDD->m_status_prev = pHDD->m_status_next;
		GetFrame().FrameRefreshStatus(DRAW_LEDS | DRAW_DISK_STATUS);
	}
}

void HarddiskInterfaceCard::GetLightStatus(Disk_Status_e *pDisk1Status)
{
	HardDiskDrive* pHDD = &m_hardDiskDrive[m_unitNum >> 7];	// bit7 = drive select
	*pDisk1Status = pHDD->m_status_prev;
}

//===========================================================================

bool HarddiskInterfaceCard::ImageSwap(void)
{
	std::swap(m_hardDiskDrive[HARDDISK_1], m_hardDiskDrive[HARDDISK_2]);

	SaveLastDiskImage(HARDDISK_1);
	SaveLastDiskImage(HARDDISK_2);

	GetFrame().FrameRefreshStatus(DRAW_LEDS);

	return true;
}

//===========================================================================

// Unit version history:
// 2: Updated $Csnn firmware to fix GH#319
static const UINT kUNIT_VERSION = 2;

#define SS_YAML_VALUE_CARD_HDD "Generic HDD"

#define SS_YAML_KEY_CURRENT_UNIT "Current Unit"
#define SS_YAML_KEY_COMMAND "Command"

#define SS_YAML_KEY_HDDUNIT "Unit"
#define SS_YAML_KEY_FILENAME "Filename"
#define SS_YAML_KEY_ERROR "Error"
#define SS_YAML_KEY_MEMBLOCK "MemBlock"
#define SS_YAML_KEY_DISKBLOCK "DiskBlock"
#define SS_YAML_KEY_IMAGELOADED "ImageLoaded"
#define SS_YAML_KEY_STATUS_NEXT "Status Next"
#define SS_YAML_KEY_STATUS_PREV "Status Prev"
#define SS_YAML_KEY_BUF_PTR "Buffer Offset"
#define SS_YAML_KEY_BUF "Buffer"

std::string HarddiskInterfaceCard::GetSnapshotCardName(void)
{
	static const std::string name(SS_YAML_VALUE_CARD_HDD);
	return name;
}

void HarddiskInterfaceCard::SaveSnapshotHDDUnit(YamlSaveHelper& yamlSaveHelper, UINT unit)
{
	YamlSaveHelper::Label label(yamlSaveHelper, "%s%d:\n", SS_YAML_KEY_HDDUNIT, unit);
	yamlSaveHelper.SaveString(SS_YAML_KEY_FILENAME, m_hardDiskDrive[unit].m_fullname);
	yamlSaveHelper.SaveHexUint8(SS_YAML_KEY_ERROR, m_hardDiskDrive[unit].m_error);
	yamlSaveHelper.SaveHexUint16(SS_YAML_KEY_MEMBLOCK, m_hardDiskDrive[unit].m_memblock);
	yamlSaveHelper.SaveHexUint32(SS_YAML_KEY_DISKBLOCK, m_hardDiskDrive[unit].m_diskblock);
	yamlSaveHelper.SaveBool(SS_YAML_KEY_IMAGELOADED, m_hardDiskDrive[unit].m_imageloaded);
	yamlSaveHelper.SaveUint(SS_YAML_KEY_STATUS_NEXT, m_hardDiskDrive[unit].m_status_next);
	yamlSaveHelper.SaveUint(SS_YAML_KEY_STATUS_PREV, m_hardDiskDrive[unit].m_status_prev);
	yamlSaveHelper.SaveHexUint16(SS_YAML_KEY_BUF_PTR, m_hardDiskDrive[unit].m_buf_ptr);

	// New label
	{
		YamlSaveHelper::Label buffer(yamlSaveHelper, "%s:\n", SS_YAML_KEY_BUF);
		yamlSaveHelper.SaveMemory(m_hardDiskDrive[unit].m_buf, HD_BLOCK_SIZE);
	}
}

void HarddiskInterfaceCard::SaveSnapshot(YamlSaveHelper& yamlSaveHelper)
{
	YamlSaveHelper::Slot slot(yamlSaveHelper, GetSnapshotCardName(), m_slot, kUNIT_VERSION);

	YamlSaveHelper::Label state(yamlSaveHelper, "%s:\n", SS_YAML_KEY_STATE);
	yamlSaveHelper.Save("%s: %d # b7=unit\n", SS_YAML_KEY_CURRENT_UNIT, m_unitNum);
	yamlSaveHelper.SaveHexUint8(SS_YAML_KEY_COMMAND, m_command);

	SaveSnapshotHDDUnit(yamlSaveHelper, HARDDISK_1);
	SaveSnapshotHDDUnit(yamlSaveHelper, HARDDISK_2);
}

bool HarddiskInterfaceCard::LoadSnapshotHDDUnit(YamlLoadHelper& yamlLoadHelper, UINT unit)
{
	std::string hddUnitName = std::string(SS_YAML_KEY_HDDUNIT) + (unit == HARDDISK_1 ? std::string("0") : std::string("1"));
	if (!yamlLoadHelper.GetSubMap(hddUnitName))
		throw std::string("Card: Expected key: ") + hddUnitName;

	m_hardDiskDrive[unit].m_fullname.clear();
	m_hardDiskDrive[unit].m_imagename.clear();
	m_hardDiskDrive[unit].m_imageloaded = false;	// Default to false (until image is successfully loaded below)
	m_hardDiskDrive[unit].m_status_next = DISK_STATUS_OFF;
	m_hardDiskDrive[unit].m_status_prev = DISK_STATUS_OFF;

	std::string filename = yamlLoadHelper.LoadString(SS_YAML_KEY_FILENAME);
	m_hardDiskDrive[unit].m_error = yamlLoadHelper.LoadUint(SS_YAML_KEY_ERROR);
	m_hardDiskDrive[unit].m_memblock = yamlLoadHelper.LoadUint(SS_YAML_KEY_MEMBLOCK);
	m_hardDiskDrive[unit].m_diskblock = yamlLoadHelper.LoadUint(SS_YAML_KEY_DISKBLOCK);
	yamlLoadHelper.LoadBool(SS_YAML_KEY_IMAGELOADED);	// Consume
	Disk_Status_e diskStatusNext = (Disk_Status_e) yamlLoadHelper.LoadUint(SS_YAML_KEY_STATUS_NEXT);
	Disk_Status_e diskStatusPrev = (Disk_Status_e) yamlLoadHelper.LoadUint(SS_YAML_KEY_STATUS_PREV);
	m_hardDiskDrive[unit].m_buf_ptr = yamlLoadHelper.LoadUint(SS_YAML_KEY_BUF_PTR);

	if (!yamlLoadHelper.GetSubMap(SS_YAML_KEY_BUF))
		throw hddUnitName + std::string(": Missing: ") + std::string(SS_YAML_KEY_BUF);
	yamlLoadHelper.LoadMemory(m_hardDiskDrive[unit].m_buf, HD_BLOCK_SIZE);

	yamlLoadHelper.PopMap();
	yamlLoadHelper.PopMap();

	//

	bool bResSelectImage = false;

	if (!filename.empty())
	{
		DWORD dwAttributes = GetFileAttributes(filename.c_str());
		if (dwAttributes == INVALID_FILE_ATTRIBUTES)
		{
			// Get user to browse for file
			bResSelectImage = SelectImage(unit, filename.c_str());

			dwAttributes = GetFileAttributes(filename.c_str());
		}

		bool bImageError = (dwAttributes == INVALID_FILE_ATTRIBUTES);
		if (!bImageError)
		{
			if (!Insert(unit, filename.c_str()))
				bImageError = true;

			// HD_Insert() sets up:
			// . m_imagename
			// . m_fullname
			// . m_imageloaded
			// . hd_status_next = DISK_STATUS_OFF
			// . hd_status_prev = DISK_STATUS_OFF

			m_hardDiskDrive[unit].m_status_next = diskStatusNext;
			m_hardDiskDrive[unit].m_status_prev = diskStatusPrev;
		}
	}

	return bResSelectImage;
}

bool HarddiskInterfaceCard::LoadSnapshot(YamlLoadHelper& yamlLoadHelper, UINT slot, UINT version, const std::string& strSaveStatePath)
{
	if (slot != SLOT7)	// fixme
		throw std::string("Card: wrong slot");

	if (version < 1 || version > kUNIT_VERSION)
		throw std::string("Card: wrong version");

	if (version == 1 && (regs.pc >> 8) == (0xC0|slot))
		throw std::string("HDD card: 6502 is running old HDD firmware");

	m_unitNum = yamlLoadHelper.LoadUint(SS_YAML_KEY_CURRENT_UNIT);	// b7=unit
	m_command = yamlLoadHelper.LoadUint(SS_YAML_KEY_COMMAND);

	// Unplug all HDDs first in case HDD-2 is to be plugged in as HDD-1
	for (UINT i=0; i<NUM_HARDDISKS; i++)
	{
		Unplug(i);
		m_hardDiskDrive[i].clear();
	}

	bool bResSelectImage1 = LoadSnapshotHDDUnit(yamlLoadHelper, HARDDISK_1);
	bool bResSelectImage2 = LoadSnapshotHDDUnit(yamlLoadHelper, HARDDISK_2);

	if (!bResSelectImage1 && !bResSelectImage2)
		RegSaveString(TEXT(REG_PREFS), TEXT(REGVALUE_PREF_HDV_START_DIR), 1, strSaveStatePath);

	GetFrame().FrameRefreshStatus(DRAW_LEDS | DRAW_DISK_STATUS);

	return true;
}
