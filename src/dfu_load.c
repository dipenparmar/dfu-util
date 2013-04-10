/* This is supposed to be a "real" DFU implementation, just as specified in the
 * USB DFU 1.0 Spec.  Not overloaded like the Atmel one...
 *
 * The code was originally intended to interface with a USB device running the
 * "sam7dfu" firmware (see http://www.openpcd.org/) on an AT91SAM7 processor.
 *
 * (C) 2007-2008 by Harald Welte <laforge@gnumonks.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include <libusb.h>

#include "portable.h"
#include "dfu.h"
#include "usb_dfu.h"
#include "dfu_file.h"
#include "dfu_load.h"
#include "quirks.h"

extern int verbose;

int dfuload_do_upload(struct dfu_if *dif, int xfer_size,
    int expected_size, struct dfu_file *file)
{
	int total_bytes = 0;
	unsigned short transaction = 0;
	unsigned char *buf;
	int ret;

	buf = dfu_malloc(xfer_size);

	printf("Copying data from DFU device to PC\n");
	dfu_progress_bar("Upload", 0, 1);

	while (1) {
		int rc, write_rc;
		rc = dfu_upload(dif->dev_handle, dif->interface,
		    xfer_size, transaction++, buf);
		if (rc < 0) {
			ret = rc;
			goto out_free;
		}
		write_rc = fwrite(buf, 1, rc, file->filep);
		if (write_rc < rc) {
			errx(EX_IOERR, "Short file write: %s",
				strerror(errno));
			ret = total_bytes;
			goto out_free;
		}
		total_bytes += rc;

		if (total_bytes < 0)
			errx(EX_SOFTWARE, "Received too many bytes");

		if (rc < xfer_size) {
			/* last block, return */
			ret = total_bytes;
			break;
		}
		dfu_progress_bar("Upload", total_bytes, expected_size);
	}
	ret = 0;

out_free:
	dfu_progress_bar("Upload", total_bytes, total_bytes);
	free(buf);
	if (verbose)
		printf("Received a total of %i bytes\n", total_bytes);
	if (expected_size != 0 && total_bytes != expected_size)
		errx(EX_SOFTWARE, "Unexpected number of bytes uploaded from device");
	return ret;
}

#define PROGRESS_BAR_WIDTH 50

int dfuload_do_dnload(struct dfu_if *dif, int xfer_size, struct dfu_file *file)
{
	int bytes_sent = 0;
	unsigned char *buf;
	unsigned short transaction = 0;
	struct dfu_status dst;
	int ret;

	buf = dfu_malloc(xfer_size);

	printf("Copying data from PC to DFU device\n");

	dfu_progress_bar("Download", 0, 1);
	while (bytes_sent < file->size - file->suffixlen) {
		int bytes_left;
		int chunk_size;

		bytes_left = file->size - file->suffixlen - bytes_sent;
		if (bytes_left < xfer_size)
			chunk_size = bytes_left;
		else
			chunk_size = xfer_size;
		ret = fread(buf, 1, chunk_size, file->filep);
		if (ret < 0) {
			err(EX_IOERR, "Could not read from file %s", file->name);
			goto out_free;
		}
		ret = dfu_download(dif->dev_handle, dif->interface,
		    ret, transaction++, ret ? buf : NULL);
		if (ret < 0) {
			errx(EX_IOERR, "Error during download");
			goto out_free;
		}
		bytes_sent += ret;

		do {
			ret = dfu_get_status(dif->dev_handle, dif->interface, &dst);
			if (ret < 0) {
				errx(EX_IOERR, "Error during download get_status");
				goto out_free;
			}

			if (dst.bState == DFU_STATE_dfuDNLOAD_IDLE ||
					dst.bState == DFU_STATE_dfuERROR)
				break;

			/* Wait while device executes flashing */
			if (dif->quirks & QUIRK_POLLTIMEOUT)
				milli_sleep(DEFAULT_POLLTIMEOUT);
			else
				milli_sleep(dst.bwPollTimeout);

		} while (1);
		if (dst.bStatus != DFU_STATUS_OK) {
			printf(" failed!\n");
			printf("state(%u) = %s, status(%u) = %s\n", dst.bState,
				dfu_state_to_string(dst.bState), dst.bStatus,
				dfu_status_to_string(dst.bStatus));
			ret = -1;
			goto out_free;
		}
		dfu_progress_bar("Download", bytes_sent, bytes_sent + bytes_left);
	}

	/* send one zero sized download request to signalize end */
	ret = dfu_download(dif->dev_handle, dif->interface,
	    0, transaction, NULL);
	if (ret < 0) {
		errx(EX_IOERR, "Error sending completion packet");
		goto out_free;
	}

	dfu_progress_bar("Download", bytes_sent, bytes_sent);

	if (verbose)
		printf("Sent a total of %i bytes\n", bytes_sent);

get_status:
	/* Transition to MANIFEST_SYNC state */
	ret = dfu_get_status(dif->dev_handle, dif->interface, &dst);
	if (ret < 0) {
		errx(EX_IOERR, "unable to read DFU status");
		goto out_free;
	}
	printf("state(%u) = %s, status(%u) = %s\n", dst.bState,
		dfu_state_to_string(dst.bState), dst.bStatus,
		dfu_status_to_string(dst.bStatus));
	if (!(dif->quirks & QUIRK_POLLTIMEOUT))
		milli_sleep(dst.bwPollTimeout);

	/* FIXME: deal correctly with ManifestationTolerant=0 / WillDetach bits */
	switch (dst.bState) {
	case DFU_STATE_dfuMANIFEST_SYNC:
	case DFU_STATE_dfuMANIFEST:
		/* some devices (e.g. TAS1020b) need some time before we
		 * can obtain the status */
		milli_sleep(1000);
		goto get_status;
		break;
	case DFU_STATE_dfuIDLE:
		break;
	}
	printf("Done!\n");

out_free:
	free(buf);

	return bytes_sent;
}
