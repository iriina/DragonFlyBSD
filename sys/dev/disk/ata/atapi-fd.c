/*-
 * Copyright (c) 1998,1999,2000,2001,2002 S�ren Schmidt <sos@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer,
 *    without modification, immediately at the beginning of the file.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * $FreeBSD: src/sys/dev/ata/atapi-fd.c,v 1.44.2.9 2002/07/31 11:19:26 sos Exp $
 * $DragonFly: src/sys/dev/disk/ata/atapi-fd.c,v 1.22 2008/01/06 16:55:49 swildner Exp $
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/ata.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/buf.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/devicestat.h>
#include <sys/cdio.h>
#include <sys/proc.h>
#include <sys/buf2.h>
#include <sys/thread2.h>

#include "ata-all.h"
#include "atapi-all.h"
#include "atapi-fd.h"

/* device structures */
static	d_open_t	afdopen;
static	d_close_t	afdclose;
static	d_ioctl_t	afdioctl;
static	d_strategy_t	afdstrategy;

static struct dev_ops afd_ops = {
	{ "afd", 118, D_DISK | D_TRACKCLOSE },
	.d_open =	afdopen,
	.d_close =	afdclose,
	.d_read =	physread,
	.d_write =	physwrite,
	.d_ioctl =	afdioctl,
	.d_strategy =	afdstrategy,
};

/* prototypes */
static int afd_sense(struct afd_softc *);
static void afd_describe(struct afd_softc *);
static int afd_done(struct atapi_request *);
static int afd_eject(struct afd_softc *, int);
static int afd_start_stop(struct afd_softc *, int);
static int afd_prevent_allow(struct afd_softc *, int);

/* internal vars */
static u_int32_t afd_lun_map = 0;
static MALLOC_DEFINE(M_AFD, "AFD driver", "ATAPI floppy driver buffers");

int 
afdattach(struct ata_device *atadev)
{
    struct afd_softc *fdp;
    cdev_t dev;

    fdp = kmalloc(sizeof(struct afd_softc), M_AFD, M_WAITOK | M_ZERO);

    fdp->device = atadev;
    fdp->lun = ata_get_lun(&afd_lun_map);
    ata_set_name(atadev, "afd", fdp->lun);
    bioq_init(&fdp->bio_queue);

    if (afd_sense(fdp)) {
	kfree(fdp, M_AFD);
	return 0;
    }

    devstat_add_entry(&fdp->stats, "afd", fdp->lun, DEV_BSIZE,
		      DEVSTAT_NO_ORDERED_TAGS,
		      DEVSTAT_TYPE_DIRECT | DEVSTAT_TYPE_IF_IDE,
		      DEVSTAT_PRIORITY_WFD);
    dev = disk_create(fdp->lun, &fdp->disk, &afd_ops);
    dev->si_drv1 = fdp;
    fdp->dev = dev;

    if (!strncmp(atadev->param->model, "IOMEGA ZIP", 10) ||
	!strncmp(atadev->param->model, "IOMEGA Clik!", 12))
	fdp->dev->si_iosize_max = 64 * DEV_BSIZE;
    else
	fdp->dev->si_iosize_max = 252 * DEV_BSIZE;

    afd_describe(fdp);
    atadev->flags |= ATA_D_MEDIA_CHANGED;
    atadev->driver = fdp;
    return 1;
}

void
afddetach(struct ata_device *atadev)
{   
    struct afd_softc *fdp = atadev->driver;
    struct bio *bio;
    struct buf *bp;
    
    while ((bio = bioq_first(&fdp->bio_queue))) {
	bioq_remove(&fdp->bio_queue, bio);
	bp = bio->bio_buf;
	bp->b_flags |= B_ERROR;
	bp->b_error = ENXIO;
	biodone(bio);
    }
    disk_invalidate(&fdp->disk);
    disk_destroy(&fdp->disk);
    devstat_remove_entry(&fdp->stats);
    ata_free_name(atadev);
    ata_free_lun(&afd_lun_map, fdp->lun);
    kfree(fdp, M_AFD);
    atadev->driver = NULL;
}   

static int 
afd_sense(struct afd_softc *fdp)
{
    int8_t ccb[16] = { ATAPI_MODE_SENSE_BIG, 0, ATAPI_REWRITEABLE_CAP_PAGE,
		       0, 0, 0, 0, sizeof(struct afd_cappage) >> 8,
		       sizeof(struct afd_cappage) & 0xff, 0, 0, 0, 0, 0, 0, 0 };
    int count, error = 0;

    /* The IOMEGA Clik! doesn't support reading the cap page, fake it */
    if (!strncmp(fdp->device->param->model, "IOMEGA Clik!", 12)) {
	fdp->cap.transfer_rate = 500;
	fdp->cap.heads = 1;
	fdp->cap.sectors = 2;
	fdp->cap.cylinders = 39441;
	fdp->cap.sector_size = 512;
	atapi_test_ready(fdp->device);
	return 0;
    }

    /* get drive capabilities, some drives needs this repeated */
    for (count = 0 ; count < 5 ; count++) {
	if (!(error = atapi_queue_cmd(fdp->device, ccb, (caddr_t)&fdp->cap,
				      sizeof(struct afd_cappage),
				      ATPR_F_READ, 30, NULL, NULL)))
	    break;
    }
    if (error || fdp->cap.page_code != ATAPI_REWRITEABLE_CAP_PAGE)
	return 1;   
    fdp->cap.cylinders = ntohs(fdp->cap.cylinders);
    fdp->cap.sector_size = ntohs(fdp->cap.sector_size);
    fdp->cap.transfer_rate = ntohs(fdp->cap.transfer_rate);
    return 0;
}

static void 
afd_describe(struct afd_softc *fdp)
{
    if (bootverbose) {
	ata_prtdev(fdp->device,
		   "<%.40s/%.8s> rewriteable drive at ata%d as %s\n",
		   fdp->device->param->model, fdp->device->param->revision,
		   device_get_unit(fdp->device->channel->dev),
		   (fdp->device->unit == ATA_MASTER) ? "master" : "slave");
	ata_prtdev(fdp->device,
		   "%luMB (%u sectors), %u cyls, %u heads, %u S/T, %u B/S\n",
		   (fdp->cap.cylinders * fdp->cap.heads * fdp->cap.sectors) / 
		   ((1024L * 1024L) / fdp->cap.sector_size),
		   fdp->cap.cylinders * fdp->cap.heads * fdp->cap.sectors,
		   fdp->cap.cylinders, fdp->cap.heads, fdp->cap.sectors,
		   fdp->cap.sector_size);
	ata_prtdev(fdp->device, "%dKB/s,", fdp->cap.transfer_rate / 8);
	kprintf(" %s\n", ata_mode2str(fdp->device->mode));
	if (fdp->cap.medium_type) {
	    ata_prtdev(fdp->device, "Medium: ");
	    switch (fdp->cap.medium_type) {
	    case MFD_2DD:
		kprintf("720KB DD disk"); break;

	    case MFD_HD_12:
		kprintf("1.2MB HD disk"); break;

	    case MFD_HD_144:
		kprintf("1.44MB HD disk"); break;

	    case MFD_UHD: 
		kprintf("120MB UHD disk"); break;

	    default:
		kprintf("Unknown (0x%x)", fdp->cap.medium_type);
	    }
	    if (fdp->cap.wp) kprintf(", writeprotected");
	}
	kprintf("\n");
    }
    else {
	ata_prtdev(fdp->device, "%luMB <%.40s> [%d/%d/%d] at ata%d-%s %s\n",
		   (fdp->cap.cylinders * fdp->cap.heads * fdp->cap.sectors) /
		   ((1024L * 1024L) / fdp->cap.sector_size),	
		   fdp->device->param->model,
		   fdp->cap.cylinders, fdp->cap.heads, fdp->cap.sectors,
		   device_get_unit(fdp->device->channel->dev),
		   (fdp->device->unit == ATA_MASTER) ? "master" : "slave",
		   ata_mode2str(fdp->device->mode));
    }
}

static int
afdopen(struct dev_open_args *ap)
{
    cdev_t dev = ap->a_head.a_dev;
    struct afd_softc *fdp = dev->si_drv1;
    struct disk_info info;

    atapi_test_ready(fdp->device);

    if (count_dev(dev) == 1)
	afd_prevent_allow(fdp, 1);

    if (afd_sense(fdp))
	ata_prtdev(fdp->device, "sense media type failed\n");

    fdp->device->flags &= ~ATA_D_MEDIA_CHANGED;

    bzero(&info, sizeof(info));
    info.d_media_blksize = fdp->cap.sector_size;	/* mandatory */
    info.d_media_blocks = fdp->cap.sectors * fdp->cap.heads * 
			  fdp->cap.cylinders;

    info.d_secpertrack = fdp->cap.sectors; 	 	/* optional */
    info.d_nheads = fdp->cap.heads;
    info.d_ncylinders = fdp->cap.cylinders;
    info.d_secpercyl = fdp->cap.sectors * fdp->cap.heads;

    disk_setdiskinfo(&fdp->disk, &info);
    return 0;
}

static int 
afdclose(struct dev_close_args *ap)
{
    cdev_t dev = ap->a_head.a_dev;
    struct afd_softc *fdp = dev->si_drv1;

    if (count_dev(dev) == 1)
	afd_prevent_allow(fdp, 0); 
    return 0;
}

static int 
afdioctl(struct dev_ioctl_args *ap)
{
    cdev_t dev = ap->a_head.a_dev;
    struct afd_softc *fdp = dev->si_drv1;

    switch (ap->a_cmd) {
    case CDIOCEJECT:
	if (count_dev(dev) > 1)
	    return EBUSY;
	return afd_eject(fdp, 0);

    case CDIOCCLOSE:
	if (count_dev(dev) > 1)
	    return 0;
	return afd_eject(fdp, 1);

    default:
	return ENOIOCTL;
    }
}

static int 
afdstrategy(struct dev_strategy_args *ap)
{
    cdev_t dev = ap->a_head.a_dev;
    struct bio *bio = ap->a_bio;
    struct buf *bp = bio->bio_buf;
    struct afd_softc *fdp = dev->si_drv1;

    if (fdp->device->flags & ATA_D_DETACHING) {
	bp->b_flags |= B_ERROR;
	bp->b_error = ENXIO;
	biodone(bio);
	return(0);
    }

    /* if it's a null transfer, return immediatly. */
    if (bp->b_bcount == 0) {
	bp->b_resid = 0;
	biodone(bio);
	return(0);
    }

    crit_enter();
    bioqdisksort(&fdp->bio_queue, bio);
    crit_exit();
    ata_start(fdp->device->channel);
    return(0);
}

void 
afd_start(struct ata_device *atadev)
{
    struct afd_softc *fdp = atadev->driver;
    struct bio *bio = bioq_first(&fdp->bio_queue);
    struct buf *bp;
    u_int32_t lba;
    u_int16_t count;
    int8_t ccb[16];
    caddr_t data_ptr;

    if (bio == NULL)
	return;

    bioq_remove(&fdp->bio_queue, bio);
    bp = bio->bio_buf;

    /* should reject all queued entries if media have changed. */
    if (fdp->device->flags & ATA_D_MEDIA_CHANGED) {
	bp->b_flags |= B_ERROR;
	bp->b_error = EIO;
	biodone(bio);
	return;
    }

    KKASSERT(bio->bio_offset % fdp->cap.sector_size == 0);

    lba = bio->bio_offset / fdp->cap.sector_size;
    count = bp->b_bcount / fdp->cap.sector_size;
    data_ptr = bp->b_data;
    bp->b_resid = bp->b_bcount; 

    bzero(ccb, sizeof(ccb));

    if (bp->b_cmd == BUF_CMD_READ)
	ccb[0] = ATAPI_READ_BIG;
    else
	ccb[0] = ATAPI_WRITE_BIG;

    ccb[2] = lba>>24;
    ccb[3] = lba>>16;
    ccb[4] = lba>>8;
    ccb[5] = lba;
    ccb[7] = count>>8;
    ccb[8] = count;

    devstat_start_transaction(&fdp->stats);

    atapi_queue_cmd(fdp->device, ccb, data_ptr, count * fdp->cap.sector_size,
		    ((bp->b_cmd == BUF_CMD_READ) ? ATPR_F_READ : 0),
		    30, afd_done, bio);
}

static int 
afd_done(struct atapi_request *request)
{
    struct bio *bio = request->driver;
    struct buf *bp = bio->bio_buf;
    struct afd_softc *fdp = request->device->driver;

    if (request->error || (bp->b_flags & B_ERROR)) {
	bp->b_error = request->error;
	bp->b_flags |= B_ERROR;
    }
    else
	bp->b_resid = bp->b_bcount - request->donecount;
    devstat_end_transaction_buf(&fdp->stats, bp);
    biodone(bio);
    return 0;
}

static int 
afd_eject(struct afd_softc *fdp, int close)
{
    int error;
     
    if ((error = afd_start_stop(fdp, 0)) == EBUSY) {
	if (!close)
	    return 0;
	if ((error = afd_start_stop(fdp, 3)))
	    return error;
	return afd_prevent_allow(fdp, 1);
    }
    if (error)
	return error;
    if (close)
	return 0;
    if ((error = afd_prevent_allow(fdp, 0)))
	return error;
    fdp->device->flags |= ATA_D_MEDIA_CHANGED;
    return afd_start_stop(fdp, 2);
}

static int
afd_start_stop(struct afd_softc *fdp, int start)
{
    int8_t ccb[16] = { ATAPI_START_STOP, 0, 0, 0, start,
		       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    return atapi_queue_cmd(fdp->device, ccb, NULL, 0, 0, 30, NULL, NULL);
}

static int
afd_prevent_allow(struct afd_softc *fdp, int lock)
{
    int8_t ccb[16] = { ATAPI_PREVENT_ALLOW, 0, 0, 0, lock,
		       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    
    if (!strncmp(fdp->device->param->model, "IOMEGA Clik!", 12))
	return 0;
    return atapi_queue_cmd(fdp->device, ccb, NULL, 0, 0, 30, NULL, NULL);
}
