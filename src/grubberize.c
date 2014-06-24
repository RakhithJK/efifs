/* grubberize.c - The elastic binding between grub and standalone EFI */
/*
 *  Copyright © 2014 Pete Batard <pete@akeo.ie>
 *  Based on GRUB, glibc and additional software:
 *  Copyright © 2001-2014 Free Software Foundation, Inc.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <efi.h>
#include <efilib.h>

#include <grub/err.h>
#include <grub/misc.h>
#include <grub/disk.h>
#include <grub/fs.h>
#include <grub/dl.h>
#include <grub/charset.h>

#include "fs_driver.h"

void grub_exit(void)
{
	ST->BootServices->Exit(EfiImageHandle, EFI_SUCCESS, 0, NULL);
	for (;;) ;
}

/* Screen I/O */
int grub_term_inputs = 0;

void grub_refresh(void) { }

int grub_getkey(void)
{
	EFI_INPUT_KEY Key;

	while (ST->ConIn->ReadKeyStroke(ST->ConIn, &Key) == EFI_NOT_READY);
	return (int) Key.UnicodeChar;
}

static void grub_xputs_dumb(const char *str)
{
	APrint((CHAR8 *)str);
}

void (*grub_xputs)(const char *str) = grub_xputs_dumb;

/* Read an EFI shell variable */
const char *grub_env_get(const char *var)
{
	EFI_STATUS Status;
	CHAR16 wVar[64], wVal[128];
	UINTN wValSize = sizeof(wVal);	/* EFI uses the size in bytes... */
	static char val[128] = { 0 };

	/* ...whereas GRUB uses the size in characters */
	grub_utf8_to_utf16(wVar, ARRAYSIZE(wVar), var, grub_strlen(var), NULL);

	Status = RT->GetVariable(wVar, &ShellVariable, NULL, &wValSize, wVal);
	if (EFI_ERROR(Status))
		return NULL;

	/* Oh, and GRUB's utf16_to_utf8 does NOT stop at NUL nor does it check the dest size!! */
	grub_utf16_to_utf8(val, wVal, StrLen(wVal)+1);

	return val;
}

/* Memory management */
void *grub_malloc(grub_size_t size)
{
	return AllocatePool(size);
}

void *grub_zalloc(grub_size_t size)
{
	return AllocateZeroPool(size);
}

void grub_free(void *ptr)
{
	FreePool(ptr);
}

/* Don't care about refcounts for a standalone EFI FS driver */
int grub_dl_ref(grub_dl_t mod) {
	return 0;
}

int grub_dl_unref(grub_dl_t mod) {
	return 0;
};


// TODO: add EFI_STATUS <-> grub_err_t (defined in err.h) calls

grub_disk_read_hook_t grub_file_progress_hook = NULL;

grub_err_t grub_disk_read(grub_disk_t disk, grub_disk_addr_t sector,
		grub_off_t offset, grub_size_t size, void *buf)
{
	EFI_STATUS Status;
	const UINT32 MediaAny = 0;
	EFI_FS* FileSystem = (EFI_FS *) disk->data;

	if ((FileSystem == NULL) || (FileSystem->DiskIo == NULL))
		return GRUB_ERR_READ_ERROR;

	/* NB: We could get the actual blocksize through FileSystem->BlockIo->Media.BlockSize
	 * but GRUB uses the fixed GRUB_DISK_SECTOR_SIZE, so we follow suit
	 */
	Status = FileSystem->DiskIo->ReadDisk(FileSystem->DiskIo, MediaAny,
			sector * GRUB_DISK_SECTOR_SIZE + offset, size, buf);

	if (EFI_ERROR(Status)) {
		PrintStatusError(Status, L"Could not read block at address %08x", sector);
		return GRUB_ERR_READ_ERROR;
	}

	return 0;
}

/* We need to instantiate this too */
grub_fs_t grub_fs_list = NULL;

// TODO: btrfs DOES call on grub_device_open() with an actual name => we'll have to handle that!
grub_device_t grub_device_open(const char *name)
{
	struct grub_device* device;

	device = grub_zalloc(sizeof(struct grub_device));
	if (device == NULL)
		return NULL;
	device->disk = grub_zalloc(sizeof(struct grub_disk));
	if (device->disk == NULL) {
		grub_free(device);
		return NULL;
	}
	/* The private disk data is a pointer back to our EFI_FS */
	device->disk->data = (void *) name;
	/* Ideally, we'd fill the other disk data, such as total_sectors, name
	 * and so on, but since we're doing the actual disk access through EFI
	 * DiskIO rather than GRUB's disk.c, this doesn't seem to be needed.
	 */

	return device;
}

grub_err_t grub_device_close(grub_device_t device)
{
	grub_free(device->disk);
	grub_free(device);
	return 0;
}

EFI_STATUS GrubDeviceInit(EFI_FS* This)
{
	This->GrubDevice = (VOID *) grub_device_open((const char *) This);
	if (This->GrubDevice == NULL)
		return EFI_OUT_OF_RESOURCES;

	return EFI_SUCCESS;
}

EFI_STATUS GrubDeviceExit(EFI_FS* This)
{
	grub_device_close((grub_device_t) This->GrubDevice);

	return EFI_SUCCESS;
}

/* Helper for GrubFSProbe.  */
static int
probe_dummy_iter (const char *filename __attribute__ ((unused)),
		const struct grub_dirhook_info *info __attribute__ ((unused)),
		void *data __attribute__ ((unused)))
{
	return 1;
}

BOOLEAN GrubFSProbe(EFI_FS *This)
{
	grub_fs_t p = grub_fs_list;
	grub_device_t device = (grub_device_t) This->GrubDevice;

	if ((p == NULL) || (device->disk == NULL)) {
		PrintError(L"GrubFSProbe: uninitialized variables\n"); 
		return FALSE;
	}

	(p->dir)(device, "/", probe_dummy_iter, NULL);
	return (grub_errno == GRUB_ERR_NONE);
}

CHAR16 *GrubGetUUID(EFI_FS* This)
{
	static CHAR16 UUID[36];
	char* uuid;

	if (grub_fs_list->uuid == NULL) {
		PrintError(L"Grub fs list is empty\n");
		return NULL;
	}

	if (grub_fs_list->uuid((grub_device_t) This->GrubDevice, &uuid) || (uuid == NULL))
		return NULL;

	grub_utf8_to_utf16(UUID, ARRAYSIZE(UUID), uuid, grub_strlen(uuid), NULL);

	return UUID;
}

/* The following is adapted from glibc's (offtime.c, etc.)
 */

/* How many days come before each month (0-12). */
static const unsigned short int __mon_yday[2][13] = {
	/* Normal years.  */
	{ 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365 },
	/* Leap years.  */
	{ 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366 }
};

/* Nonzero if YEAR is a leap year (every 4 years,
   except every 100th isn't, and every 400th is). */
#define __isleap(year) \
	((year) % 4 == 0 && ((year) % 100 != 0 || (year) % 400 == 0))

#define SECS_PER_HOUR         (60 * 60)
#define SECS_PER_DAY          (SECS_PER_HOUR * 24)
#define DIV(a, b)             ((a) / (b) - ((a) % (b) < 0))
#define LEAPS_THRU_END_OF(y)  (DIV (y, 4) - DIV (y, 100) + DIV (y, 400))

/* Compute an EFI_TIME representation of a GRUB's mtime_t */
VOID GrubTimeToEfiTime(const INT32 t, EFI_TIME *tp)
{
	INT32 days, rem, y;
	const unsigned short int *ip;

	days = t / SECS_PER_DAY;
	rem = t % SECS_PER_DAY;
	while (rem < 0) {
		rem += SECS_PER_DAY;
		--days;
	}
	while (rem >= SECS_PER_DAY) {
		rem -= SECS_PER_DAY;
		++days;
	}
	tp->Hour = rem / SECS_PER_HOUR;
	rem %= SECS_PER_HOUR;
	tp->Minute = rem / 60;
	tp->Second = rem % 60;
	y = 1970;

	while (days < 0 || days >= (__isleap (y) ? 366 : 365)) {
		/* Guess a corrected year, assuming 365 days per year. */
		INT32 yg = y + days / 365 - (days % 365 < 0);

		/* Adjust DAYS and Y to match the guessed year. */
		days -= ((yg - y) * 365
			+ LEAPS_THRU_END_OF (yg - 1)
			- LEAPS_THRU_END_OF (y - 1));
		y = yg;
	}
	tp->Year = y;
	ip = __mon_yday[__isleap(y)];
	for (y = 11; days < (long int) ip[y]; --y)
		continue;
	days -= ip[y];
	tp->Month = y + 1;
	tp->Day = days + 1;
}
