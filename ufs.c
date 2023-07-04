#define FUSE_USE_VERSION 30
#define __USE_GNU

#include <fuse.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

char dir_list[256][256];
int curr_dir_idx = -1;

char files_list[256][256];
int curr_file_idx = -1;

char files_content[256][256];
int curr_file_content_idx = -1;

#define BSIZE 4096 // block size
#define BNUM 262144  // block count in a device
#define NDIRECT 12  // 12 blocks to store data (12 direct data blocks)
#define NINDIRECT (BSIZE / sizeof(unsigned int))  // (1024 indirect data blocks)
#define MAXFILE (NDIRECT + NINDIRECT)  // totally 1036 data blocks


#define T_DIR 1 // Directory
#define T_FILE 2 // File

// a total map from block content
struct dinode {
	short type; // File type
	short pad[1];
	unsigned int nlink;
	unsigned int size; // Size of file (bytes). 0 for dir.
	unsigned int addrs[NDIRECT + 1]; // Data block addresses. Must >= BDATASTART. 0 is Null
};  // size of dinode: 2 + 2 + 4 + 4 + 4 * 13 = 64

struct inode {
	unsigned int dev; // Device number
	unsigned int inum; // Inode number. Root is 1. Null is 0
	int ref; // Reference count. Init to 0
	int valid; // inode has been read from disk?
	short type; // 0 or T_DIR or T_FILE
	unsigned int nlink;
	unsigned int size;
	unsigned int addrs[NDIRECT + 1];
};

#define NINODE 1024

struct {
	struct inode inode[NINODE];
} itable;

#define DIRSIZ 254

struct dirent {
	unsigned short inum; // Root is 1. Null is 0
	char name[DIRSIZ];
};  // size: 256

/* bitmap: one block can store 4096 * 8 = 32768 bits, needs (BNUM) / (8 * BSIZE) = 8 BitmapBlocks */

#define BMAPSTART 1024  // 1024 * 64 innodes are put in front
#define BDATASTART 1032  // 4096 * 8 * 8 bits are put in front

#define IPB (BSIZE / sizeof(struct dinode))  // 64 innodes per block
#define IBLOCK(inum) ((inum) / IPB)  // give innode num, return the block innode is stored
#define BBLOCK(bno) ((bno) / (8 * BSIZE) + BMAPSTART)  // give block num, return the bitmap block where bno's is_active is stored

#define MIN(a, b) (a < b ? a : b)
#define MAX(a, b) (a > b ? a : b)

int fd = -1;

// return fd
int open_disk() {
	int fd = 1;
	if ((fd = open("/tmp/disk", __O_DIRECT | __O_NOATIME | O_RDWR)) < 0) {
		fprintf(stderr, "failed to open. errno: %d\n", errno);
		// return -1;
		exit(-1);
	}
	return fd;
}

// return cnt
int read_disk(int fd, void* buffer, unsigned int bno)
{
	// printf("=== read_disk ===\n");
	// printf("fd: %d\n", fd);
	// printf("bno: %d\n", bno);
	lseek(fd, bno * BSIZE, SEEK_SET);
	int ret;
	if ((ret = read(fd, buffer, BSIZE)) < 0) {
		fprintf(stderr, "failed to read file. ret: %d, errno: %d\n", ret, errno);
		// return -1;
		exit(-1);
	}
	return ret;
}

// return cnt
int write_disk(int fd, void* buffer, unsigned int bno)
{
	// printf("=== write_disk ===\n");
	// printf("fd: %d\n", fd);
	// printf("bno: %d\n", bno);
	lseek(fd, bno * BSIZE, SEEK_SET);
	int ret;
	if ((ret = write(fd, buffer, BSIZE)) < 0) {
		fprintf(stderr, "failed to write file. ret: %d, errno: %d\n", ret, errno);
		// return -1;
		exit(-1);
	}
	fsync(fd);
	return ret;
}

// return if close success
int close_disk(int fd)
{
	int ret;
	if ((ret = close(fd)) < 0) {
		fprintf(stderr, "failed to close file. errno: %d\n", errno);
		// return -1;
		exit(-1);
	}
	return ret;
}

void* get_buffer()
{
	void* buffer = NULL;
	if (posix_memalign(&buffer, 512, BSIZE) != 0) {
		fprintf(stderr, "Failed to alloc aligned buffer\n");
		// return NULL;
		exit(-1);
	}
	memset(buffer, 0, BSIZE);
	return buffer;
}

// free(buffer);

void init_inode()
{
	for (struct inode *ip = &itable.inode[0]; ip < &itable.inode[NINODE]; ip++) {
		ip->dev = 0;
		ip->inum = 0;
		ip->ref = 0;
		ip->valid = 0;
		ip->type = 0;
		ip->nlink = 0;
		ip->size = 0;
		for (size_t i = 0; i < NDIRECT + 1; i++) {
			ip->addrs[i] = 0;
		}
	}
}

void init_root()
{
	void* buffer = get_buffer();
	read_disk(fd, buffer, IBLOCK(1));
	struct dinode *dip = (struct dinode *)buffer + 1;
	
	memset(dip, 0, sizeof(*dip));
	dip->type = T_DIR;
	dip->nlink = 2;
	dip->size = 0; // dir size is 0!
	write_disk(fd, buffer, IBLOCK(1));
	free(buffer);
}

void init_bitmap()
{
	void* buffer = get_buffer();
	for (unsigned int bno = BMAPSTART; bno < BDATASTART; bno++) {
		// read_disk(fd, buffer, bno);
		memset(buffer, 0, BSIZE);
		write_disk(fd, buffer, bno);
	}
	free(buffer);
}

static void binit(unsigned int bno)
{
	// struct buf *bp;
	void *buffer = get_buffer();
	// bp = bread(dev, bno);
	// read_disk(fd, buffer, bno);
	memset(buffer, 0, BSIZE);
	// bwrite(bp);
	write_disk(fd, buffer, bno);
	free(buffer);
}

// Allocate a zeroed disk data block.
static unsigned int balloc()
{
	unsigned int b, bi, m;
	// struct buf *bp = 0;
	void *buffer = get_buffer();
	char *a;

	for (b = 0; b < BNUM; b += BSIZE * 8) {
		// bp = bread(dev, BBLOCK(b));
		unsigned int bit_bno = BBLOCK(b);
		read_disk(fd, buffer, bit_bno);
		for (bi = 0; bi < BSIZE * 8 && b + bi < BNUM; bi++) {
			if (b + bi < BDATASTART) continue;
			m = 1 << (bi % 8);
			a = (char *)buffer + (bi / 8);
			if ((*a & m) == 0) { // Is block free?
				*a |= m; // Mark block in use.
				// bwrite(bp);
				write_disk(fd, buffer, bit_bno);
				// brelse(bp);
				free(buffer);
				binit(b + bi);
				return b + bi;
			}
		}
		// brelse(bp);
	}
	free(buffer);
	fprintf(stderr, "balloc: out of blocks\n");
	exit(-1);
}

// Free a disk block.
static void bfree(unsigned int bno)
{
	// struct buf *bp;
	void *buffer = get_buffer();

	// bp = bread(dev, BBLOCK(b, sb));
	unsigned int bit_bno = BBLOCK(bno);
	read_disk(fd, buffer, bit_bno);
	unsigned int bi = bno % (BSIZE * 8);
	unsigned int m = 1 << (bi % 8);
	char *a = (char *)buffer + (bi / 8);
	if ((*a & m) == 0) {
		fprintf(stderr, "freeing free block\n");
		exit(-1);
	}
	*a &= ~m;
	// bwrite(bp);
	write_disk(fd, buffer, bit_bno);
	// brelse(bp);
	free(buffer);
}

static struct inode *iget(unsigned int inum)
{
	// printf("== iget ==\n");
	struct inode *ip, *empty;
	// Is the inode already in the table?
	empty = 0;
	for (ip = &itable.inode[0]; ip < &itable.inode[NINODE]; ip++) {
		if (ip->ref > 0 && ip->inum == inum) {
			ip->ref++;
			return ip;
		}
		if (empty == 0 && ip->ref == 0) // Remember empty slot.
			empty = ip;
	}

	// Recycle an inode entry.
	if (empty == 0) {
		fprintf(stderr, "iget: no inodes\n");
		exit(-1);
	}

	ip = empty;
	ip->inum = inum;
	ip->ref = 1;
	ip->valid = 0;
	return ip;
}

void ivalid(struct inode *ip)
{
	// printf("== ivalid ==\n");
	void* buffer = get_buffer();
	struct dinode *dip;
	if (ip->valid == 0) {
		// bp = bread(ip->dev, IBLOCK(ip->inum, sb));
		read_disk(fd, buffer, IBLOCK(ip->inum));
		dip = (struct dinode *)buffer + ip->inum % IPB;
		ip->type = dip->type;
		ip->nlink = dip->nlink;
		ip->size = dip->size;
		memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
		free(buffer);
		ip->valid = 1;
		if (ip->type == 0) {
			printf("ip->inum: %d\n", ip->inum);
			fprintf(stderr, "ivalid: no type\n");
			exit(-1);
		}
	}
}

void iput(struct inode *ip)
{
	// if (ip->ref == 1 && ip->valid && ip->nlink == 0) {
	// 	// inode has no links and no other references: truncate and free.
	// 	itrunc(ip);  // ip->size = 0
	// 	ip->type = 0;
	// 	iupdate(ip);
	// 	ip->valid = 0;
	// }
	ip->ref--;
}

void iupdate(struct inode *ip)
{
	// struct buf *bp;
	struct dinode *dip;

	void *buffer = get_buffer();
	// bp = bread(ip->dev, IBLOCK(ip->inum, sb));
	read_disk(fd, buffer, IBLOCK(ip->inum));
	dip = (struct dinode *)buffer + ip->inum % IPB;
	dip->type = ip->type;
	dip->nlink = ip->nlink;
	dip->size = ip->size;
	memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
	// bwrite(bp);
	write_disk(fd, buffer, IBLOCK(ip->inum));
	// brelse(bp);
	free(buffer);
}

// Truncate inode (discard contents).
void itrunc(struct inode *ip)
{
	int i, j;
	// struct buf *bp;
	void *buffer = get_buffer();
	unsigned int *a;

	for (i = 0; i < NDIRECT; i++) {
		if (ip->addrs[i] != 0) {
			bfree(ip->addrs[i]);
			ip->addrs[i] = 0;
		}
	}

	if (ip->addrs[NDIRECT] != 0) {
		// bp = bread(ip->dev, ip->addrs[NDIRECT]);
		read_disk(fd, buffer, ip->addrs[NDIRECT]);
		a = (unsigned int *)buffer;
		for (j = 0; j < NINDIRECT; j++) {
			if (a[j] != 0)
				bfree(a[j]);
		}
		// brelse(bp);
		bfree(ip->addrs[NDIRECT]);
		ip->addrs[NDIRECT] = 0;
	}
	free(buffer);

	ip->size = 0;
	iupdate(ip);
}


struct inode *root_dir()
{
	// printf("== root_dir ==\n");
	struct inode *r = iget(1);  // 1 is root inode
	ivalid(r);
	return r;
}


// give ip and index of ip->addrs, return the actual bno where data should be stored
static unsigned int bmap(struct inode *ip, unsigned int bn)
{
	// struct buf *bp;
	// printf("== bmap ==\n");
	if (bn < NDIRECT) {
		if (ip->addrs[bn] == 0)
			ip->addrs[bn] = balloc();
		return ip->addrs[bn];
	}
	bn -= NDIRECT;

	if (bn < NINDIRECT) {
		// Load indirect block, allocating if necessary.
		if (ip->addrs[NDIRECT] == 0)
			ip->addrs[NDIRECT] = balloc();
		// bp = bread(ip->dev, addr);

		void *buffer = get_buffer();
		read_disk(fd, buffer, ip->addrs[NDIRECT]);
		unsigned int *a = (unsigned int *)buffer;
		if (a[bn] == 0) {
			a[bn] = balloc();
			// bwrite(bp);
			write_disk(fd, buffer, ip->addrs[NDIRECT]);
		}
		// brelse(bp);
		free(buffer);
		return a[bn];
	}

	fprintf(stderr, "bmap: out of range\n");
	exit(-1);
}

int readi(struct inode *ip, void *dst, unsigned int off, unsigned int n)
{
	// printf("== readi ==\n");
	// printf("off: %d\n", off);
	// printf("n: %d\n", n);
	
	unsigned int tot, m;
	// struct buf *bp;
	void* buffer = get_buffer();

	if (off > ip->size || off + n < off)
		return 0;
	if (off + n > ip->size)
		n = ip->size - off;

	for (tot = 0; tot < n; tot += m, off += m, dst += m) {
		// bp = bread(ip->dev, bmap(ip, off / BSIZE));
		read_disk(fd, buffer, bmap(ip, off / BSIZE));
		m = MIN(n - tot, BSIZE - off % BSIZE);
		memmove(dst, (char *)buffer + (off % BSIZE), m);
		// brelse(bp);
	}
	free(buffer);
	return tot;
}

int writei(struct inode *ip, void *src, unsigned int off, unsigned int n)
{
	// printf("== writei ==\n");
	unsigned int tot, m;
	// struct buf *bp;
	void* buffer = get_buffer();

	if (off > ip->size || off + n < off)
		return -1;
	if (off + n > MAXFILE * BSIZE)
		return -1;

	for (tot = 0; tot < n; tot += m, off += m, src += m) {
		// bp = bread(ip->dev, bmap(ip, off / BSIZE));
		unsigned int data_bno = bmap(ip, off / BSIZE);
		read_disk(fd, buffer, data_bno);
		m = MIN(n - tot, BSIZE - off % BSIZE);
		memmove((char *)buffer + (off % BSIZE), src, m);
		// bwrite(bp);
		write_disk(fd, buffer, data_bno);
		// brelse(bp);
	}
	free(buffer);

	if (off > ip->size)
		ip->size = off;

	// write the i-node back to disk even if the size didn't change
	// because the loop above might have called bmap() and added a new
	// block to ip->addrs[].
	iupdate(ip);

	return tot;
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
// If not found, set *poff to byte offset of empty entry.
struct inode *dirlookup(struct inode *dp, const char *name, unsigned int *poff)
{
	// printf("== dirlookup ==\n");
	unsigned int off, inum;
	struct dirent de;

	if (dp->type != T_DIR) {
		fprintf(stderr, "dirlookup not DIR\n");
		exit(-1);
	}

	// printf("dirloopup: name: %s\n", name);

	int meet_empty = 0;
	for (off = 0; off < dp->size; off += sizeof(de)) {
		if (readi(dp, (void *)&de, off, sizeof(de)) != sizeof(de)) {
			fprintf(stderr, "dirlookup read\n");
			exit(-1);
		}
		// printf("de.inum: %d\n", de.inum);
		// printf("de.name: %s\n", de.name);
		// printf("name: %s\n", name);
		if (de.inum == 0) {
			if (poff != NULL && meet_empty == 0) {
				meet_empty = 1;
				*poff = off;
			}
			continue;
		}
		
		if (strncmp(name, de.name, DIRSIZ) == 0) {
			// entry matches path element
			if (poff != NULL)
				*poff = off;
			inum = de.inum;
			return iget(inum);
		}
	}
	if (poff != NULL && meet_empty == 0) {
		*poff = off;
	}
	return NULL;
}

struct inode *ialloc(short type)
{
	// printf("== ialloc ==\n");
	unsigned int inum;
	// struct buf *bp;
	void *buffer = get_buffer();
	struct dinode *dip;
	// printf("ialloc: type: %d\n", type);

	for (inum = 1; inum < BMAPSTART * IPB; inum++) {
		// bp = bread(dev, IBLOCK(inum, sb));
		read_disk(fd, buffer, IBLOCK(inum));
		dip = (struct dinode *)buffer + inum % IPB;
		if (dip->type == 0) { // a free inode
			memset(dip, 0, sizeof(*dip));
			dip->type = type;
			if (type == T_DIR) dip->nlink = 2;
			else if (type == T_FILE) dip->nlink = 1;
			dip->size = 0;

			struct inode* ip = iget(inum);
			ip->type = type;
			ip->nlink = dip->nlink;
			ip->size = 0;
			ip->valid = 1;  // already ivalid(ip)!

			// bwrite(bp);
			write_disk(fd, buffer, IBLOCK(inum));
			// brelse(bp);
			free(buffer);
			return ip;
		}
		// brelse(bp);
	}
	fprintf(stderr, "ialloc: no inodes\n");
	exit(-1);
}

int dirlink(struct inode *dp, const char *name, unsigned int inum, unsigned int off)
{
	// Check that name is not present.
	// struct inode *ip;
	// if ((ip = dirlookup(dp, name, 0)) != 0) {
	// 	iput(ip);
	// 	return -1;
	// }

	// Look for an empty dirent.
	// for (off = 0; off < dp->size; off += sizeof(de)) {
	// 	if (readi(dp, (void *)&de, off, sizeof(de)) != sizeof(de)) {
	// 		fprintf(stderr, "dirlink read\n");
	// 		exit(-1);
	// 	}
	// 	if (de.inum == 0)
	// 		break;
	// }

	// printf("== dirlink ==\n");
	// printf("inum: %d\n", inum);
	// printf("name: %s\n", name);
	struct dirent de;
	// if (readi(dp, (void *)&de, off, sizeof(de)) != sizeof(de)) {
	// 	fprintf(stderr, "dirlink read\n");
	// 	exit(-1);
	// }
	strncpy(de.name, name, DIRSIZ);
	de.inum = inum;
	if (writei(dp, (void *)&de, off, sizeof(de)) != sizeof(de)) {
		fprintf(stderr, "dirlink write\n");
		exit(-1);
	}
	return 0;
}

int dirunlink(struct inode *dp, unsigned int off)
{
	// if name is not present. else find the dirent successfully.
	// struct inode *ip;
	// if ((ip = dirlookup(dp, name, &off)) == 0) {
	// 	return -1;
	// }
	// iput(ip);

	// printf("== dirunlink ==\n");
	struct dirent de;
	// if (readi(dp, (void *)&de, off, sizeof(de)) != sizeof(de)) {
	// 	fprintf(stderr, "dirlink read\n");
	// 	exit(-1);
	// }
	strncpy(de.name, "", DIRSIZ);
	de.inum = 0;
	if (writei(dp, (void *)&de, off, sizeof(de)) != sizeof(de)) {
		fprintf(stderr, "dirlink write\n");
		exit(-1);
	}
	return 0;
}

int add_file(const char *name, short type)
{
	// curr_dir_idx++;
	// strcpy(dir_list[curr_dir_idx], dir_name);
	// printf("== add_file ==\n");
	struct inode *ip, *dp;
	dp = root_dir(); // Remember that the root_inode is open in this step, so it needs closing then.
	unsigned int off = 0;
	if ((ip = dirlookup(dp, name, &off)) != 0) {
		printf("cannot create an existed file or dir\n");
		iput(ip);
		iput(dp); // Close the root_inode
		return -1;
	}

	ip = ialloc(type);  // already ivalid(ip);
	if (off >= dp->size) {
		dp->size = off + sizeof(struct dirent);
	}
	dirlink(dp, name, ip->inum, off);
	unsigned int off2, count;
	struct dirent de;
	count = 0;
	for (off2 = 0; off2 < dp->size; off2 += sizeof(de)) {
		if (readi(dp, (void *)&de, off2, sizeof(de)) != sizeof(de)) {
			fprintf(stderr,"dirlookup read\n");
			exit(-1);
		}
		// printf("de.inum: %d\n", de.inum);
		// printf("de.name: %s\n", de.name);
		if (de.inum == 0)
			continue;
		count++;
	}

	iput(ip);
	iput(dp);
	return 0;
}

int rm_file(const char *name, short type) 
{
	// printf("== rm_file ==\n");
	struct inode *ip, *dp;
	dp = root_dir(); // Remember that the root_inode is open in this step, so it needs closing then.
	unsigned int off = 0;
	if ((ip = dirlookup(dp, name, &off)) == 0) {
		printf("cannot remove an unexisted file or dir\n");
		iput(dp); // Close the root_inode
		return -1;
	}

	ivalid(ip);
	if (type == T_DIR && ip->type == type) {
		// now it must be empty
		dirunlink(dp, off);
		itrunc(ip); // ip->size = 0;
		ip->type = 0;
		iupdate(ip);
		ip->valid = 0;
		iput(ip);
		iput(dp);
		return 0;
	}
	else if (type == T_FILE && ip->type == type) {
		dirunlink(dp, off);
		itrunc(ip); // ip->size = 0;
		ip->type = 0;
		iupdate(ip);
		ip->valid = 0;
		iput(ip);
		iput(dp);
		return 0;
	}
	else {
		printf("not a directory\n");
		iput(ip);
		iput(dp); // Close the root_inode
		return -1;
	}
}

int is_dir(const char *name) 
{
	name++; // Eliminating "/" in the path
	
	// for (int curr_idx = 0; curr_idx <= curr_dir_idx; curr_idx++)
	// 	if (strcmp(name, dir_list[curr_idx]) == 0)
	// 		return 1;
	
	struct inode *ip, *dp;
	dp = root_dir(); // Remember that the root_inode is open in this step, so it needs closing then.
	if ((ip = dirlookup(dp, name, 0)) == 0) {
		printf("cannot find dir\n");
		iput(dp); // Close the root_inode
		return 0;  // not dir
	}

	int ret;
	ivalid(ip);
	if (ip->type == T_DIR) ret = 1;
	else if (ip->type == T_FILE) ret = 0;
	else {
		fprintf(stderr, "is_dir: type is zero\n");
		exit(-1);
	}
	iput(ip);
	iput(dp);
	return ret;
}

int is_empty_dir(const char *name)
{
	name++; // Eliminating "/" in the path

	struct inode *ip, *dp;
	dp = root_dir(); // Remember that the root_inode is open in this step, so it needs closing then.
	if ((ip = dirlookup(dp, name, 0)) == 0) {
		printf("cannot find dir\n");
		iput(dp); // Close the root_inode
		return 0;  // not dir
	}

	int ret;
	ivalid(ip);
	if (ip->type == T_DIR) {
		// now it must be empty
		ret = 1;
	}
	else if (ip->type == T_FILE) ret = 0;
	else {
		fprintf(stderr, "is_dir: type is zero\n");
		exit(-1);
	}
	iput(ip);
	iput(dp);
	return ret;
}

int is_file(const char *name)
{
	name++; // Eliminating "/" in the path
	
	// for (int curr_idx = 0; curr_idx <= curr_file_idx; curr_idx++)
	// 	if (strcmp(path, files_list[curr_idx]) == 0)
	// 		return 1;
	
	struct inode *ip, *dp;
	dp = root_dir(); // Remember that the root_inode is open in this step, so it needs closing then.
	if ((ip = dirlookup(dp, name, 0)) == 0) {
		printf("cannot find file\n");
		iput(dp); // Close the root_inode
		return 0;  // not dir
	}

	int ret;
	ivalid(ip);
	if (ip->type == T_FILE) ret = 1;
	else if (ip->type == T_DIR) ret = 0;
	else {
		fprintf(stderr, "is_dir: type is zero\n");
		exit(-1);
	}
	iput(ip);
	iput(dp);
	return ret;
}

unsigned int get_file_inum(const char *name)
{
	// printf("== get_file_inum ==\n");
	name++; // Eliminating "/" in the path
	// for (int curr_idx = 0; curr_idx <= curr_file_idx; curr_idx++)
	// 	if (strcmp(path, files_list[curr_idx]) == 0)
	// 		return curr_idx;
	struct inode *ip, *dp;
	dp = root_dir();
	if ((ip = dirlookup(dp, name, 0)) == 0) {
		printf("cannot find file\n");
		iput(dp); // Close the root_inode
		return -1;  // not dir
	}

	int inum = ip->inum;
	iput(ip);
	iput(dp);
	return inum;
}


static void *do_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
	(void) conn;
	cfg->kernel_cache = 1;
	return NULL;
}


static int do_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
	// printf("== do_getattr ==\n");
	// printf("path: %s\n", path);
	(void) fi;
	
	// st->st_uid = getuid(); // The owner of the file/directory is the user who mounted the filesystem
	// st->st_gid = getgid(); // The group of the file/directory is the same as the group of the user who mounted the filesystem
	// st->st_atime = time(NULL); // The last "a"ccess of the file/directory is right now
	// st->st_mtime = time(NULL); // The last "m"odification of the file/directory is right now
	
	if (strcmp(path, "/") == 0) {
		st->st_mode = S_IFDIR;
		st->st_nlink = 2; // Why "two" hardlinks instead of "one"? The answer is here: http://unix.stackexchange.com/a/101536
		return 0;
	}

	path++;
	struct inode *ip, *dp;
	dp = root_dir(); // Remember that the root_inode is open in this step, so it needs closing then.
	if ((ip = dirlookup(dp, path, 0)) == 0) {
		printf("cannot find dir\n");
		iput(dp); // Close the root_inode
		return -ENOENT;  // not exist
	}

	ivalid(ip);
	if (ip->type == T_DIR) {
		// st->st_mode = S_IFDIR | 0755;
		st->st_mode = S_IFDIR;
		st->st_nlink = 2; // Why "two" hardlinks instead of "one"? The answer is here: http://unix.stackexchange.com/a/101536
	}
	else if (ip->type == T_FILE) {
		// st->st_mode = S_IFREG | 0644;
		st->st_mode = S_IFREG;
		st->st_nlink = 1;
		st->st_size = ip->size;
	}
	else {
		return -ENOENT;
	}
	iput(ip);
	iput(dp);
	return 0;
}

static int do_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
	// printf("== do_readdir ==\n");
	// printf("path: %s\n", path);
	(void) offset;
	(void) fi;
	(void) flags;
	
	filler(buffer, ".", NULL, 0, 0); // Current Directory
	filler(buffer, "..", NULL, 0, 0); // Parent Directory
	
	if (strcmp(path, "/") == 0) // If the user is trying to show the files/directories of the root directory show the following
	{
		// for (int curr_idx = 0; curr_idx <= curr_dir_idx; curr_idx++)
		// 	filler(buffer, dir_list[curr_idx], NULL, 0, 0);
	
		// for (int curr_idx = 0; curr_idx <= curr_file_idx; curr_idx++)
		// 	filler(buffer, files_list[curr_idx], NULL, 0, 0);
		unsigned int off, count;
		struct dirent de;

		struct inode* dp = root_dir();
		count = 0;
		for (off = 0; off < dp->size; off += sizeof(de)) {
			if (readi(dp, (void *)&de, off, sizeof(de)) != sizeof(de)) {
				fprintf(stderr,"dirlookup read\n");
				exit(-1);
			}
			if (de.inum == 0)
				continue;
			filler(buffer, de.name, NULL, 0, 0);
			count++;
		}
		iput(dp);
		return 0;
	}
	
	return -ENOENT;
}

static int do_mkdir(const char *path, mode_t mode)
{
	// printf("== do_mkdir ==\n");
	path++;
	add_file(path, T_DIR);
	return 0;
}

static int do_mknod(const char *path, mode_t mode, dev_t rdev)
{
	// printf("== do_mknod ==\n");
	path++;
	add_file(path, T_FILE);
	return 0;
}

static int do_rmdir(const char *path)
{
	// printf("== do_rmdir ==\n");
	path++;
	rm_file(path, T_DIR);
	return 0;
}

static int do_unlink(const char *path)
{
	// printf("== do_unlink ==\n");
	path++;
	rm_file(path, T_FILE);
	return 0;
}

static int do_rename1(const char* src, const char* dst, unsigned int flags)
{
	// printf("== do_rename1 ==\n");
	// printf("src: %s, dst: %s\n", src, dst);
	src++;
	dst++;
	struct inode *ip, *dp;
	dp = root_dir(); // Remember that the root_inode is open in this step, so it needs closing then.
	// path++;
	unsigned int off = 0;
	if ((ip = dirlookup(dp, src, &off)) == 0) {
		printf("cannot find dir\n");
		iput(dp); // Close the root_inode
		return -1;  // not exist
	}
	ivalid(ip);
	dirlink(dp, dst, ip->inum, off);

	iput(ip);
	iput(dp);
	return 0;
}

static int do_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi)
{
	// printf("== do_read ==\n");
	// int file_idx = get_file_index(path);
	// if (file_idx == -1) return -1;
	struct inode *ip, *dp;
	dp = root_dir(); // Remember that the root_inode is open in this step, so it needs closing then.
	path++;
	if ((ip = dirlookup(dp, path, 0)) == 0) {
		printf("file not exist\n");
		iput(dp); // Close the root_inode
		return -1;  // not exist
	}
	ivalid(ip);
	if (ip->type != T_FILE) {
		printf("not a file!\n");
		iput(ip);
		iput(dp);
		return -1;
	}

	// char *content = files_content[file_idx];
	// memcpy(buffer, content + offset, size);
	// return strlen(content) - offset;
	int r = readi(ip, (void *)buffer, offset, size);
	iput(ip);
	iput(dp);
	return r;
}

static int do_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *info)
{
	// printf("== do_write ==\n");
	// int file_idx = get_file_index(path);
	// if (file_idx == -1) // No such file
	// 	return;
	struct inode *ip, *dp;
	dp = root_dir(); // Remember that the root_inode is open in this step, so it needs closing then.
	path++;
	if ((ip = dirlookup(dp, path, 0)) == 0) {
		printf("cannot find dir\n");
		iput(dp); // Close the root_inode
		return -1;  // not exist
	}
	ivalid(ip);
	if (ip->type != T_FILE) {
		printf("not a file!\n");
		iput(ip);
		iput(dp);
		return -1;
	}

	// strcpy(files_content[file_idx], buffer);
	// return size;
	int r = writei(ip, (void *)buffer, offset, size);
	iput(ip);
	iput(dp);
	return r;
}

static struct fuse_operations operations = {
	.init   	= do_init,
    .getattr	= do_getattr,
    .readdir	= do_readdir,
    .mkdir		= do_mkdir,
    .mknod		= do_mknod,
	.rmdir	    = do_rmdir,
	.unlink     = do_unlink,
	.rename     = do_rename1,
    .read		= do_read,
    .write		= do_write,
};

int main(int argc, char *argv[])
{
	fd = open_disk();

	init_root();
	init_bitmap();
	init_inode();
	
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	int ret = 0;
	ret = fuse_main(argc, argv, &operations, NULL);

	fuse_opt_free_args(&args);

	close_disk(fd);

	return ret;
}
