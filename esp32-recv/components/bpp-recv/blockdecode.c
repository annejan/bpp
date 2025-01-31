#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <time.h>
#include <string.h>
#include "structs.h"
#include "hldemux.h"
#include "blockdevif.h"
#include "blockdecode.h"
#include "blkidcache.h"
#include "powerdown.h"

#define ST_WAIT_CATALOG 0
#define ST_WAIT_OLD 1
#define ST_WAIT_DATA 2


struct BlockDecodeHandle{
	int state;
	BlockdevifHandle *bdev;
	BlockdevIf *bdif;
	BlkIdCacheHandle *idcache;
	int noBlocks;
	int currentChangeID;
};


static int allBlocksUpToDate(BlockDecodeHandle *d) {
	for (int i=0; i<d->noBlocks; i++) {
		if (idcacheGet(d->idcache, i) < d->currentChangeID) return 0;
	}
	return 1;
}



void blockdecodeStatus(BlockDecodeHandle *d) {
	printf("Blockdev status: changeid %d, blocks: (* is up-to-date)\n", d->currentChangeID);
	for (int i=0; i<d->noBlocks; i++) {
		if (idcacheGet(d->idcache, i) < d->currentChangeID) printf("."); else printf("*");
	}
	printf("\n");
}


void blockdecodeShutDown(BlockDecodeHandle *d) {
	idcacheFlushToStorage(d->idcache);
}

static void blockdecodeRecv(int subtype, uint8_t *data, int len, void *arg) {
	BlockDecodeHandle *d=(BlockDecodeHandle*)arg;

	const char *tp="unknown";
	if (subtype==BDSYNC_SUBTYPE_BITMAP) tp="bitmap";
	if (subtype==BDSYNC_SUBTYPE_OLDERMARKER) tp="oldermarker";
	if (subtype==BDSYNC_SUBTYPE_CHANGE) tp="change";
	//printf("Blockdecode: Got subtype %s\n", tp);

	if (subtype==BDSYNC_SUBTYPE_BITMAP) {
		BDPacketBitmap *p=(BDPacketBitmap*)data;
		uint32_t idOld=ntohl(p->changeIdOrig);
		uint32_t idNew=ntohl(p->changeIdNew);
		uint16_t noBits=ntohs(p->noBits);
		powerHold((int)arg);
		d->currentChangeID=idNew;
		printf("Bitmap %d bits for %d blocks.\n", (len-sizeof(BDPacketBitmap))*8, d->noBlocks);
		if ((len-sizeof(BDPacketBitmap)) > d->noBlocks/8) return; //encroyable!
		//Update current block map: all blocks newer than changeIdOrig are still up-to-date and
		//can be updated to changeIdNew.
		int i;
		for (i=0; i<noBits; i++) {
			//Update if bit is 1 in bitmap
			if ( i >= (len-sizeof(BDPacketBitmap))*8 || p->bitmap[i/8]&(1<<(i&7))) {
				if (idcacheGet(d->idcache, i)>=idOld) idcacheSet(d->idcache, i, d->currentChangeID);
			}
		}
		//Rest of sectors not in bitmap is always assumed to be up-to-date.
		for (; i<d->noBlocks; i++) idcacheSet(d->idcache, i, d->currentChangeID);
		//See if that action updated all blocks
		if (allBlocksUpToDate(d)) {
			//Yay, we can sleep.
			printf("All up to date. We can sleep.\n");
			d->state=ST_WAIT_CATALOG;
			powerCanSleep((int)arg);
		} else {
			d->state=ST_WAIT_DATA;
			printf("Got bitmap.\n");
		}
	} else if (subtype==BDSYNC_SUBTYPE_OLDERMARKER) {
		BDPacketOldermarker *p=(BDPacketOldermarker*)data;
		//We're only interested in this if we actually need data.
		if (d->state!=ST_WAIT_CATALOG) {
			//See if we're interested in the new blocks. We are if our oldest block
			//is more recent than the oldest block sent in the new blocks.
			//First, find oldest change id.
			uint32_t oldest=0xFFFFFFFF;
			for (int i=0; i<d->noBlocks; i++) {
				int chgid=idcacheGet(d->idcache, i);
				if (chgid<oldest) oldest=chgid;
			}
			//Check if we're interested in the newer or older blocks, or neither.
			if (oldest>ntohl(p->oldestNewTs)) {
				printf("Blockdev: Grabbing new packets.\n");
				//We're not that far behind: all packets we need will be following this announcement
				//right now.
				d->state=ST_WAIT_DATA;
			} else {
				//We're behind: we need the rotation of older packets to get up-to-date.
				//See if the range that will be sent this run is any good.
				int needOldBlocks=0;
				for (int i=ntohs(p->secIdStart); i!=ntohs(p->secIdEnd); i++) {
					if (i>=d->noBlocks) i=0;
					if (idcacheGet(d->idcache, i)!=d->currentChangeID) {
						needOldBlocks=1;
						break;
					}
				}
				if (needOldBlocks) {
					printf("Blockdecode: Skipping new packets. Sleeping %d ms.\n", ntohl(p->delayMs));
					powerCanSleepFor((int)arg, p->delayMs);
					d->state=ST_WAIT_DATA;
				} else {
					printf("Blockdev: Don't need any packets in this cycle. Sleeping\n");
					//Next cycle is entirely useless. Wait for next catalog marker so we can sleep until
					//the next catalog comes.
					d->state=ST_WAIT_CATALOG;
					powerCanSleep((int)arg);
				}
			}
		}
	} else if (subtype==BDSYNC_SUBTYPE_CHANGE) {
		//If no bitmap has come in, don't handle changes.
		if (d->currentChangeID==0) {
			printf("Data ignored; waiting for bitmap first.\n");
			powerCanSleep((int)arg);
			return;
		}
		if (d->state != ST_WAIT_CATALOG) {
			BDPacketChange *p=(BDPacketChange*)data;
			if (ntohl(p->changeId) != d->currentChangeID) {
				//Huh? Must've missed an entire catalog...
				printf("Data changeid %d, last changeid I know of %d. Sleeping this cycle.\n", ntohl(p->changeId), d->currentChangeID);
				d->state=ST_WAIT_CATALOG;
				powerCanSleep((int)arg);
			}
			int blk=ntohs(p->sector);
			if (idcacheGet(d->idcache, blk)>d->currentChangeID) {
				printf("Blockdecode: WtF? Got newer block than sent? (us: %d, remote: %d)\n", idcacheGet(d->idcache, blk), d->currentChangeID);
			} else if (idcacheGet(d->idcache, blk)!=d->currentChangeID) {
				//Write block
				idcacheSetSectorData(d->idcache, blk, p->data, ntohl(p->changeId));
				printf("Blockdecode: Got change for block %d. Writing to disk.\n", blk);
			} else {
				printf("Blockdecode: Got change for block %d. Already had this change.\n", blk);
			}
			//See if we have everything we need.
			if (allBlocksUpToDate(d)) {
				//Yay, we can sleep.
				printf("Blockdecode: Received change final packet. Waiting for catalog ptr to sleep.\n");
				d->state=ST_WAIT_CATALOG;
				powerCanSleep((int)arg);
			}
		}
	} else {
		printf("Blockdecode: unknown subtype %d\n", subtype);
	}
}

BlockDecodeHandle *blockdecodeInit(int type, int size, BlockdevIf *bdIf, void *bdevdesc) {
	BlockDecodeHandle *d=malloc(sizeof(BlockDecodeHandle));
	if (d==NULL) {
		return NULL;
	}
	memset(d, 0, sizeof(BlockDecodeHandle));
	d->bdev=bdIf->init(bdevdesc, size);
	if (d->bdev==NULL) {
		free(d);
		return 0;
	}
	d->state=ST_WAIT_CATALOG;
	d->noBlocks=size/BLOCKDEV_BLKSZ;
	d->idcache=idcacheCreate(d->noBlocks, d->bdev, bdIf);
	d->bdif=bdIf;

	hldemuxAddType(type, blockdecodeRecv, d);
	powerHold((int)d);

	return d;
}

