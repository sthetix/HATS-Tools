/*
 * HATS Installer - Simplified disk I/O (SD card only)
 * Based on TegraExplorer diskio.c by shchmue
 *
 * This simplified version removes BIS/eMMC support since the
 * HATS installer only needs SD card access.
 */

/*-----------------------------------------------------------------------*/
/* Low level disk I/O module skeleton for FatFs     (C)ChaN, 2016        */
/*-----------------------------------------------------------------------*/

#include <string.h>

#include <libs/fatfs/diskio.h>	/* FatFs lower layer API */
#include <memory_map.h>
#include <storage/nx_sd.h>
#include <storage/sdmmc.h>

/*-----------------------------------------------------------------------*/
/* Get Drive Status                                                      */
/*-----------------------------------------------------------------------*/
DSTATUS disk_status (
	BYTE pdrv /* Physical drive number to identify the drive */
)
{
	return 0;
}

/*-----------------------------------------------------------------------*/
/* Initialize a Drive                                                    */
/*-----------------------------------------------------------------------*/
DSTATUS disk_initialize (
	BYTE pdrv /* Physical drive number to identify the drive */
)
{
	return 0;
}

/*-----------------------------------------------------------------------*/
/* Read Sector(s)                                                        */
/*-----------------------------------------------------------------------*/
DRESULT disk_read (
	BYTE pdrv,		/* Physical drive number to identify the drive */
	BYTE *buff,		/* Data buffer to store read data */
	DWORD sector,	/* Start sector in LBA */
	UINT count		/* Number of sectors to read */
)
{
	if (pdrv == DRIVE_SD)
		return sdmmc_storage_read(&sd_storage, sector, count, buff) ? RES_OK : RES_ERROR;

	return RES_ERROR;
}

/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                       */
/*-----------------------------------------------------------------------*/
DRESULT disk_write (
	BYTE pdrv,			/* Physical drive number to identify the drive */
	const BYTE *buff,	/* Data to be written */
	DWORD sector,		/* Start sector in LBA */
	UINT count			/* Number of sectors to write */
)
{
	if (pdrv == DRIVE_SD)
		return sdmmc_storage_write(&sd_storage, sector, count, (void *)buff) ? RES_OK : RES_ERROR;

	return RES_ERROR;
}

/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/
static u32 part_rsvd_size = 0;
DRESULT disk_ioctl (
	BYTE pdrv,		/* Physical drive number (0..) */
	BYTE cmd,		/* Control code */
	void *buff		/* Buffer to send/receive control data */
)
{
	DWORD *buf = (DWORD *)buff;

	if (pdrv == DRIVE_SD)
	{
		switch (cmd)
		{
		case GET_SECTOR_COUNT:
			*buf = sd_storage.sec_cnt - part_rsvd_size;
			break;
		case GET_BLOCK_SIZE:
			*buf = 32768; // Align to 16MB.
			break;
		}
	}
	else if (pdrv == DRIVE_RAM)
	{
		switch (cmd)
		{
		case GET_SECTOR_COUNT:
			*buf = RAM_DISK_SZ >> 9; // 1GB.
			break;
		case GET_BLOCK_SIZE:
			*buf = 2048; // Align to 1MB.
			break;
		}
	}

	return RES_OK;
}
