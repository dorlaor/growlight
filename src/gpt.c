#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <iconv.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <openssl/err.h>
#include <openssl/rand.h>

#include "gpt.h"
#include "crc32.h"
#include "growlight.h"

#define GUIDSIZE 16 // 128 bits

#define LBA_SIZE 512u
#define MBR_OFFSET 440u
#define MBR_SIZE (LBA_SIZE - MBR_OFFSET)

static const unsigned char GPT_PROTECTIVE_MBR[LBA_SIZE - MBR_OFFSET] =
 "\x00\x00\x00\x00\x00\x00"	// 6 bytes of zeros
 "\x80"				// bootable (violation of GPT spec, but some
 				//  BIOS/MBR *and* UEFI won't boot otherwise)
 "\x00\x00\x00"			// CHS of first absolute sector
 "\xee"				// Protective partition type
 "\xff\xff\xff"			// CHS of last absolute sector
 "\x00\x00\x00\x00"		// LBA of first absolute sector
 "\xff\xff\xff\xff"		// Sectors in partition
 "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
 "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
 "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
 "\x55\xaa";			// MBR signature

static const unsigned char gpt_signature[8] = "\x45\x46\x49\x20\x50\x41\x52\x54";

// One LBA block, padded with zeroes at the end. 92 bytes.
typedef struct __attribute__ ((packed)) gpt_header {
	uint64_t signature;		// "EFI PART", 45 46 49 20 50 41 52 54
	uint32_t revision;		// Through UEFI 2.3.1: 00 00 01 00
	uint32_t headsize;		// Header size in little endian,
					// excludes padding: 5c 00 00 00 == 92
// byte 0x10
	uint32_t crc;			// crc32, 0 through headsize
	uint32_t reserved;		// must be 0
	uint64_t lba;			// location of this header
// byte 0x20
	uint64_t backuplba;		// location of backup header
	uint64_t first_usable;		// first usable lba
// byte 0x30
	uint64_t last_usable;		// last usable lba
	unsigned char disk_guid[GUIDSIZE];
	uint64_t partlba;		// partition entries lba for this copy
// byte 0x50
	uint32_t partcount;		// supported partition entry count
	uint32_t partsize;		// size of partition entries
	uint32_t partcrc;		// crc32 of partition array
} gpt_header;

// 128-byte GUID partition entry. A GPT table must provide space for at least
// MINIMUM_GPT_ENTRIES (128) of these, equal to 16k (32 512-byte sectors, or
// 4 4096-byte sectors) in both copies of the GPT.
typedef struct __attribute__ ((packed)) gpt_entry {
	unsigned char type_guid[GUIDSIZE];
	unsigned char part_guid[GUIDSIZE];
	uint64_t first_lba;
	uint64_t last_lba;
	uint64_t flags;
	uint16_t name[36];	// 36 UTF-16LE code units
} gpt_entry;

#define MINIMUM_GPT_ENTRIES 128

static void
update_crc(gpt_header *head,size_t lbasize){
	const gpt_entry *gpes = (const gpt_entry *)((char *)head + lbasize);
	size_t hs = head->headsize; // FIXME little-endian; swap on BE machines

	head->partcrc = crc32(gpes,head->partcount * head->partsize);
	head->crc = 0;
	head->crc = crc32(head,hs);
}

static int
update_backup(int fd,const gpt_header *ghead,unsigned gptlbas,
			unsigned backuplba,unsigned lbasize,
			unsigned pgsize,int realdata){
	const off_t absdevoff = backuplba * lbasize;
	const size_t mapoff = absdevoff % pgsize;
	gpt_header *gh;
	size_t mapsize;
	void *map;

	mapsize = pgsize * ((mapoff + gptlbas * lbasize) / pgsize +
			!!((mapoff + gptlbas * lbasize) % pgsize));
	if((map = mmap(NULL,mapsize,PROT_READ|PROT_WRITE,MAP_SHARED,fd,
				absdevoff - mapoff)) == MAP_FAILED){
		diag("Error mapping %zub at %d (%s?)\n",mapsize,fd,strerror(errno));
		return -1;
	}
	gh = (gpt_header *)((char *)map + mapoff);
	if(realdata){
		memcpy(gh,ghead,gptlbas * lbasize);
		gh->lba = gh->backuplba;
		gh->partlba = gh->lba + 1;
		if(lbasize > sizeof(*gh)){
			memset((char *)gh + sizeof(*gh),0,lbasize - sizeof(*gh));
		}
		update_crc(gh,lbasize);
	}else{
		memset(gh,0,gptlbas * lbasize);
	}
	if(msync(map,mapsize,MS_SYNC|MS_INVALIDATE)){
		diag("Error syncing %d (%s?)\n",fd,strerror(errno));
		munmap(map,mapsize);
		return -1;
	}
	if(munmap(map,mapsize)){
		diag("Error unmapping %d (%s?)\n",fd,strerror(errno));
		return -1;
	}
	return 0;
}

static int
initialize_gpt(gpt_header *gh,size_t lbasize,unsigned backuplba,unsigned firstusable){
	memcpy(&gh->signature,gpt_signature,sizeof(gh->signature));
	gh->revision = 0x10000u;
	gh->headsize = sizeof(*gh);
	gh->reserved = 0;
	gh->lba = 1;
	// ->crc is set by update_crc()
	gh->backuplba = backuplba;
	gh->first_usable = firstusable;
	gh->last_usable = backuplba - 1;
	if(RAND_bytes(gh->disk_guid,GUIDSIZE) != 1){
		diag("%s",ERR_error_string(ERR_get_error(),NULL));
		return -1;
	}
	gh->partlba = gh->lba + 1;
	gh->partcount = MINIMUM_GPT_ENTRIES;
	gh->partsize = sizeof(gpt_entry);
	// ->partcrc is set by update_crc()
	if(lbasize > sizeof(*gh)){
		memset((char *)gh + sizeof(*gh),0,lbasize - sizeof(*gh));
	}
	return 0;
}

// Write out a GPT and its backup on the device represented by fd, using
// lbasize-byte LBA. The device ought have lbas lbasize-byte sectors. We will
// write to the second-from-the-first, and the final, groups of sectors. lbas
// must be greater than or equal to 1 + 2 * (1 + ceil(16k / lbasize).
//
// We can either zero it all out, or create a new empty GPT. Set realdata not
// equal to 0 to perform the latter.
static int
write_gpt(int fd,ssize_t lbasize,unsigned long lbas,unsigned realdata){
	ssize_t s = lbasize - (MINIMUM_GPT_ENTRIES * sizeof(gpt_entry) % lbasize);
	const size_t gptlbas = 1 + !!s + (MINIMUM_GPT_ENTRIES * sizeof(gpt_entry) / lbasize);
	off_t backuplba = lbas - gptlbas;
	int pgsize = getpagesize();
	gpt_header *ghead;
	size_t mapsize;
	void *map;
	off_t off;

	assert(pgsize > 0 && pgsize % lbasize == 0);
	// The first copy goes into LBA 1. Calculate offset into map due to
	// lbasize possibly (probably) not equalling the page size.
	if((off = lbasize % pgsize) == 0){
		mapsize = lbasize;
	}else{
		mapsize = 0;
	}
	mapsize += gptlbas * lbasize;
	mapsize = ((mapsize / pgsize) + !!(mapsize % pgsize)) * pgsize;
	assert(mapsize % pgsize == 0);
	map = mmap(NULL,mapsize,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
	if(map == MAP_FAILED){
		diag("Error mapping %zub at %d (%s?)\n",mapsize,fd,strerror(errno));
		return -1;
	}
	ghead = (gpt_header *)((char *)map + off);
	if(!realdata){
		memset(ghead,0,gptlbas * lbasize);
	}else{
		// FIXME unsure that firstusable calculation is correct
		if(initialize_gpt(ghead,lbasize,backuplba,gptlbas)){
			munmap(map,mapsize);
			return -1;
		}
		update_crc(ghead,lbasize);
	}
	if(msync(map,mapsize,MS_SYNC|MS_INVALIDATE)){
		diag("Error syncing %d (%s?)\n",fd,strerror(errno));
		munmap(map,mapsize);
		return -1;
	}
	if(update_backup(fd,ghead,gptlbas,backuplba,lbasize,pgsize,realdata)){
		munmap(map,mapsize);
		return -1;
	}
	if(munmap(map,mapsize)){
		diag("Error unmapping %d (%s?)\n",fd,strerror(errno));
		return -1;
	}
	return 0;
}

int new_gpt(device *d){
	size_t mapsize;
	void *map;
	int fd;

	if(d->layout != LAYOUT_NONE){
		diag("Won't create partition table on non-disk %s\n",d->name);
		return -1;
	}
	if(d->size % LBA_SIZE){
		diag("Won't create GPT on (%ju %% %u == %juB) disk %s\n",
			d->size,LBA_SIZE,d->size % LBA_SIZE,d->name);
		return -1;
	}
	if(d->size < LBA_SIZE + 2 * (LBA_SIZE + MINIMUM_GPT_ENTRIES * sizeof(gpt_entry))){
		diag("Won't create GPT on %juB disk %s\n",d->size,d->name);
		return -1;
	}
	if((fd = openat(devfd,d->name,O_RDWR|O_CLOEXEC|O_DIRECT)) < 0){
		diag("Couldn't open %s (%s?)\n",d->name,strerror(errno));
		return -1;
	}
	// protective MBR in first LBA
	mapsize = getpagesize(); // FIXME check for insanity
	if((map = mmap(NULL,mapsize,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0)) == MAP_FAILED){
		diag("Couldn't map %s (%s?)\n",d->name,strerror(errno));
		close(fd);
		return -1;
	}
	memcpy((char *)map + MBR_OFFSET,GPT_PROTECTIVE_MBR,MBR_SIZE);
	if(munmap(map,mapsize)){
		diag("Couldn't unmap MBR for %s (%s?)\n",d->name,strerror(errno));
		close(fd);
		return -1;
	}
	if(write_gpt(fd,LBA_SIZE,d->size / LBA_SIZE,1)){
		diag("Couldn't write GPT on %s (%s?)\n",d->name,strerror(errno));
		close(fd);
		return -1;
	}
	if(fsync(fd)){
		diag("Warning: error syncing %d for %s (%s?)\n",fd,d->name,strerror(errno));
	}
	if(close(fd)){
		diag("Error closing %d for %s (%s?)\n",fd,d->name,strerror(errno));
		return -1;
	}
	return 0;
}

int zap_gpt(device *d){
	size_t mapsize;
	void *map;
	int fd;

	if(d->layout != LAYOUT_NONE){
		diag("Won't zap partition table on non-disk %s\n",d->name);
		return -1;
	}
	if(d->blkdev.pttable == NULL || strcmp(d->blkdev.pttable,"gpt")){
		diag("No GPT on disk %s\n",d->name);
		return -1;
	}
	if((fd = openat(devfd,d->name,O_RDWR|O_CLOEXEC|O_DIRECT)) < 0){
		diag("Couldn't open %s (%s?)\n",d->name,strerror(errno));
		return -1;
	}
	mapsize = getpagesize(); // FIXME check for insanity
	if((map = mmap(NULL,mapsize,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0)) == MAP_FAILED){
		diag("Couldn't map %s (%s?)\n",d->name,strerror(errno));
		close(fd);
		return -1;
	}
	memset((char *)map + MBR_OFFSET,0,MBR_SIZE);
	if(munmap(map,mapsize)){
		diag("Couldn't unmap MBR for %s (%s?)\n",d->name,strerror(errno));
		close(fd);
		return -1;
	}
	if(write_gpt(fd,LBA_SIZE,d->size / LBA_SIZE,0)){
		diag("Couldn't write GPT on %s (%s?)\n",d->name,strerror(errno));
		close(fd);
		return -1;
	}
	if(fsync(fd)){
		diag("Warning: error syncing %d for %s (%s?)\n",fd,d->name,strerror(errno));
	}
	if(close(fd)){
		diag("Error closing %d for %s (%s?)\n",fd,d->name,strerror(errno));
		return -1;
	}
	return 0;
}

static int
gpt_name(const wchar_t *name,uint16_t *name16le){
	size_t len,olen = 36;
	iconv_t icv;

	errno = 0;
	if((icv = iconv_open("WCHAR_T","UTF16LE")) == (iconv_t)-1 && errno){
		diag("Can't convert WCHAR_T to UTF16LE (%s?)\n",strerror(errno));
		return -1;
	}
	len = wcslen(name);
	fprintf(stderr,"NAME: %ls %zu\n",name,len);
	if(iconv(icv,(char **)&name,&len,(char **)&name16le,&olen) == (size_t)-1 && errno){
		diag("Error converting name (%s? %zu/%zu left)\n",strerror(errno),len,olen);
		iconv_close(icv);
		return -1;
	}
	if(iconv_close(icv)){
		return -1;
	}
	return 0;
}

#include "popen.h"
int add_gpt(device *d,const wchar_t *name,uintmax_t size){
	unsigned lbas = size / LBA_SIZE;
	ssize_t s = LBA_SIZE - (MINIMUM_GPT_ENTRIES * sizeof(gpt_entry) % LBA_SIZE);
	const size_t gptlbas = 1 + !!s + (MINIMUM_GPT_ENTRIES * sizeof(gpt_entry) / LBA_SIZE);
	off_t backuplba = lbas - 1 - gptlbas;
	int pgsize = getpagesize();
	gpt_header *ghead;
	gpt_entry *gpe;
	size_t mapsize;
	void *map;
	off_t off;
	int fd;

	assert(pgsize && pgsize % LBA_SIZE == 0);
	if(!name){
		diag("GPT partitions ought be named!\n");
		return -1;
	}
	if(d->layout != LAYOUT_NONE){
		diag("Won't add partition table on non-disk %s\n",d->name);
		return -1;
	}
	if(d->blkdev.pttable == NULL || strcmp(d->blkdev.pttable,"gpt")){
		diag("No GPT on disk %s\n",d->name);
		return -1;
	}
	if(size % d->logsec){
		diag("Size %ju is not a mulitple of sector %u\n",size,d->logsec);
		return -1;
	}
	// The first copy goes into LBA 1.
	if((fd = openat(devfd,d->name,O_RDWR|O_CLOEXEC|O_DIRECT)) < 0){
		diag("Couldn't open %s (%s?)\n",d->name,strerror(errno));
		return -1;
	}
	off = (LBA_SIZE % pgsize == 0) ? LBA_SIZE : 0;
	if(off == 0){
		mapsize = LBA_SIZE;
	}else{
		mapsize = 0;
	}
	mapsize += gptlbas * LBA_SIZE;
	mapsize = ((mapsize / pgsize) + (mapsize % pgsize)) * pgsize;
	map = mmap(NULL,mapsize,PROT_READ|PROT_WRITE,MAP_SHARED,fd,off);
	if(map == MAP_FAILED){
		close(fd);
		return -1;
	}
	ghead = (gpt_header *)((char *)map + off);
	// FIXME must walk the partition table entries and find unused
	if(ghead->partcount >= MINIMUM_GPT_ENTRIES){
		diag("GPT partition table full (%u/%u)\n",ghead->partcount,MINIMUM_GPT_ENTRIES);
		munmap(map,mapsize);
		close(fd);
		return -1;
	}
	gpe = (gpt_entry *)((char *)ghead + LBA_SIZE) + ghead->partcount;
	memset(gpe->type_guid,0,GUIDSIZE); // all 0's is "GPT unused"
	if(RAND_bytes(gpe->part_guid,GUIDSIZE) != 1){
		diag("%s",ERR_error_string(ERR_get_error(),NULL));
		munmap(map,mapsize);
		close(fd);
		return -1;
	}
	// FIXME need to ensure they're not used by existing partitions!
	gpe->first_lba = ghead->first_usable;
	gpe->last_lba = ghead->last_usable;
	if(gpt_name(name,gpe->name)){
		diag("Couldn't convert %ls for %s\n",name,d->name);
		munmap(map,mapsize);
		close(fd);
		return -1;
	}
	update_crc(ghead,LBA_SIZE);
	if(update_backup(fd,ghead,gptlbas,backuplba,LBA_SIZE,pgsize,1)){
		munmap(map,mapsize);
		close(fd);
		return -1;
	}
	if(munmap(map,mapsize)){
		int e = errno;

		diag("Error munmapping %s (%s?)\n",d->name,strerror(errno));
		close(fd);
		errno = e;
		return -1;
	}
	if(close(fd)){
		int e = errno;

		diag("Error closing %s (%s?)\n",d->name,strerror(errno));
		errno = e;
		return -1;
	}
	return 0;
	/*
	uintmax_t sectors;
	unsigned partno;
	const device *p;

	sectors = size / LBA_SIZE;
	partno = 1;
	for(p = d->parts ; p ; p = p->next){
		if(partno == p->partdev.pnumber){
			const device *pcheck;

			do{
				++partno;
				for(pcheck = d->parts ; pcheck != p ; pcheck = pcheck->next){
					if(partno == pcheck->partdev.pnumber){
						break;
					}
				}
			}while(p != pcheck);
		}
	}
	if(vspopen_drain("sgdisk --new=%u:0:%ju /dev/%s",partno,sectors,d->name)){
		return -1;
	}
	return 0;
	*/
}