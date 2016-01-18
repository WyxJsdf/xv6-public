// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation 
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"
#include "fat_fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
struct FAT32_DBR dbr;   // there should be one per dev, but we run with one dev

// Read the super block.
void
readDbr(int dev, struct FAT32_DBR *dbr)
{
  struct buf *bp;
  bp = bread(dev, 0);
  memmove(dbr, bp->data, sizeof(*dbr));
  brelse(bp);
}

// Zero a block.

// Blocks. 
uint getFATStart(uint cnum, uint *offset)
{
  uint ret;
  *offset = (cnum * 4);
  ret = dbr.RsvdSecCnt + (*offset / dbr.BytesPerSec);
  *offset %= dbr.BytesPerSec;
  return ret;
}

void updateFATs(struct buf* sp){
  struct buf *tp;
  int i, offset;
  for (i = 1, offset = dbr.FATSz32; i < dbr.NumFATs; i++, offset += dbr.FATSz32) {
    tp = bread(sp->dev, sp->blockno + offset);
    memmove(tp->data, sp->data, SECSIZE);
    bwrite(tp);
    brelse(tp);
  }
}

uint getFirstSector(uint cnum)
{
  return (cnum - 2) * dbr.SecPerClus + dbr.RsvdSecCnt + dbr.NumFATs * dbr.FATSz32;
}
// Allocate a zeroed disk cluster.
static uint
fat32_calloc(uint dev)
{
  uint cnum, nowSec, lastSec;
  struct buf *bp, *bfsi;
  struct FSInfo *fsi;
  readDbr(dev, &dbr);
  bfsi = bread(dev, dbr.FSInfo);
  fsi = (struct FSInfo*)bfsi->data;
  lastSec = 0;
  for (cnum = fsi->Nxt_Free + 1; cnum < dbr.TotSec32 / dbr.SecPerClus; cnum++){
    uint offset;
    nowSec = getFATStart(cnum, &offset);
    if (nowSec != lastSec){
      if (bp)
        brelse(bp);
      bp = bread(dev, nowSec);
      lastSec = nowSec;
    }
    if (!*(uint *)(bp->data + offset)){
      *(uint *)(bp->data + offset) = LAST_FAT_VALUE;
      fsi->Nxt_Free++;
      fsi->Free_Count--;
      updateFATs(bp);
      bwrite(bp);
      brelse(bp);
      bwrite(bfsi);
      brelse(bfsi);
      return cnum;
    }
  }
  for (cnum = 2; cnum <= fsi->Nxt_Free; cnum++){
    uint offset;
    nowSec = getFATStart(cnum, &offset);
    if (nowSec != lastSec){
      if (bp)
        brelse(bp);
      bp = bread(dev, nowSec);
      lastSec = nowSec;
    }
    if (!*(uint *)(bp->data + offset)){
      *(uint *)(bp->data + offset) = LAST_FAT_VALUE;
      fsi->Nxt_Free = cnum;
      fsi->Free_Count--;
      updateFATs(bp);
      bwrite(bp);
      brelse(bp);
      bwrite(bfsi);
      brelse(bfsi);
      return cnum;
    }
  }
  panic("balloc: out of clusters");
}

// Free a disk cluster.
// static void
// bfree(int dev, uint cnum)
// {
//     struct buf *bp;
//     uint offset, nowSec;
//     readDbr(dev, &dbr);
//     nowSec = getFATStart(cnum, &offset);
//     bp = bread(dev, nowSec);
//     *(uint *)(bp->data + offset) = 0;
//     bwrite(bp);
//     brelse(bp);
// }


struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} icache;

void
fat32_iinit(int dev)
{
    initlock(&icache.lock, "icache");
    readDbr(dev, &dbr);
    cprintf("dbr: BytesPerSec: %d   SecPerClus: %d   NumFATs: %d   TotSec32:   %d", dbr.BytesPerSec, dbr.SecPerClus, dbr.NumFATs, dbr.TotSec32);
}

static struct inode* fat32_iget(uint dev, uint inum, uint dirCluster);

struct inode* fat32_ialloc(struct inode *dp, short type){
    uint cnum = fat32_calloc(dp->dev);
    struct inode* ip = fat32_iget(dp->dev, cnum, dp->inum);
    ip->type = type;
    ip->size = 0;
   return ip;
}

void
fat32_iupdate(struct inode *ip)
{
  struct buf *bp, *bp1;
  struct direntry *dip;
  uint dirCluster = ip->dirCluster, st, nowSec, offset, lastSec, i, j;
  
  readDbr(ip->dev, &dbr);
  lastSec = 0;
  do{
    st = getFirstSector(dirCluster);
    for (i = st; i < st + dbr.SecPerClus; i++){
      bp = bread(ip->dev, i);
      for (j = 0; j < SECSIZE; j+=sizeof(struct direntry)){
        dip = (struct direntry*)(bp->data+j);
        if (((dip->deHighClust << 16)|dip->deLowCluster) == ip->inum){
          dip->deAttributes = (uchar)ip->type;
          dip->deCTime = (ushort)ip->major;
          dip->deCDate = (ushort)ip->minor;
          dip->deFileSize = ip->size;
          bwrite(bp);
          brelse(bp);
          if (bp1)
            brelse(bp1);
          return;
        }
      }
      brelse(bp);
    }
    nowSec = getFATStart(dirCluster, &offset);
    if (nowSec != lastSec){
      if (bp1)
        brelse(bp1);
      bp1 = bread(ip->dev, nowSec);
      lastSec = nowSec;
    }
    if (*(uint *)(bp1->data + offset) < LAST_FAT_VALUE)
       dirCluster = *(uint *)(bp1->data + offset);
     else break;
  }while (1);
  panic("update error");

}

static struct inode*
fat32_iget(uint dev, uint inum, uint dirCluster)
{
  struct inode *ip, *empty;
  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for(ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if(empty == 0 && ip->ref == 0)    // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode cache entry.
  if(empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->flags = 0;
  ip->dirCluster = dirCluster;
  release(&icache.lock);

  return ip;
}

struct inode*
fat32_idup(struct inode *ip)
{
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

void
fat32_ilock(struct inode *ip)
{
  struct buf *bp, *bp1;
  struct direntry *dip;
  uint dirCluster = ip->dirCluster, st, i,j, nowSec, offset, lastSec;

  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  acquire(&icache.lock);
  while(ip->flags & I_BUSY)
    sleep(ip, &icache.lock);
  ip->flags |= I_BUSY;
  release(&icache.lock);
  
 if (ip->inum == 2) {     // Root file
    ip->type = T_DIR;
    ip->nlink = 1;
    ip->flags |= I_VALID;
    return;
  }
  if(!(ip->flags & I_VALID)){
    readDbr(ip->dev, &dbr);
    lastSec = 0;
    do{
    st = getFirstSector(dirCluster);
    for (i = st; i < st + dbr.SecPerClus; i++){
      bp = bread(ip->dev, i);
      for (j = 0; j < SECSIZE; j+=sizeof(struct direntry)){
        dip = (struct direntry*)(bp->data+j);
        if (((dip->deHighClust << 16)|dip->deLowCluster) == ip->inum){
          ip->type = (short)dip->deAttributes;
          ip->major = (short) dip->deCTime;
          ip->minor = (short) dip->deCDate;
          ip->nlink = 1;
          ip->size = dip->deFileSize;
          brelse(bp);
          if (bp1)
            brelse(bp1);
          ip->flags |= I_VALID;
          if(ip->type == 0)
            panic("ilock: no type");
          return;
        }
      }
      brelse(bp);
    }
    nowSec = getFATStart(dirCluster, &offset);
    if (nowSec != lastSec){
      if (bp1)
        brelse(bp1);
      bp1 = bread(ip->dev, nowSec);
      lastSec = nowSec;
    }
    if (*(uint *)(bp1->data + offset) < LAST_FAT_VALUE)
       dirCluster = *(uint *)(bp1->data + offset);
     else break;
  }while (1);
}
  panic("ilock error");
}

// Unlock the given inode.
void
fat32_iunlock(struct inode *ip)
{
  if(ip == 0 || !(ip->flags & I_BUSY) || ip->ref < 1)
    panic("iunlock");

  acquire(&icache.lock);
  ip->flags &= ~I_BUSY;
  wakeup(ip);
  release(&icache.lock);
}

static void
fat32_itrunc(struct inode *ip)
{
  struct buf *bp, *bp1;
  struct direntry *dip;
  struct FSInfo *fsi;
  uint dirCluster = ip->dirCluster, st, nowSec, offset, lastSec, cnum, i, j;
  readDbr(ip->dev, &dbr);

  lastSec = 0;
  do{
    st = getFirstSector(dirCluster);
    for (i = st; i < st + dbr.SecPerClus; i++){
      bp = bread(ip->dev, i);
      for (j = 0; j < SECSIZE; j+=sizeof(struct direntry)){
        dip = (struct direntry*)(bp->data+j);
        if (((dip->deHighClust << 16)|dip->deLowCluster) == ip->inum){
          dip->deName[0] = 0xE5;
          bwrite(bp);
          brelse(bp);
          if (bp1)
            brelse(bp1);
          goto handleFAT;
        }
      }
      brelse(bp);
    }
    nowSec = getFATStart(dirCluster, &offset);
    if (nowSec != lastSec){
      if (bp1)
        brelse(bp1);
      bp1 = bread(ip->dev, nowSec);
      lastSec = nowSec;
    }
    if (*(uint *)(bp1->data + offset) < LAST_FAT_VALUE)
       dirCluster = *(uint *)(bp1->data + offset);
     else break;
  }while (1);
  panic("itrunc error");
  
handleFAT:
  bp1 = bread(ip->dev, dbr.FSInfo);
  fsi = (struct FSInfo*)bp1->data; 
  cnum = ip->inum;
  bp = 0; lastSec = 0;
  do{
    nowSec = getFATStart(cnum, &offset);
    if (nowSec != lastSec){
      if (bp) {
        updateFATs(bp);
        bwrite(bp);
        brelse(bp);
      }
      bp = bread(ip->dev, nowSec);
      lastSec = nowSec;
    }
    cnum = *(uint*)(bp->data + offset);
    *(uint*)(bp->data + offset) = 0;
    fsi->Free_Count ++;
    if (cnum >= LAST_FAT_VALUE) break;
  } while (1);
  updateFATs(bp);
  bwrite(bp);
  brelse(bp);
  bwrite(bp1);
  brelse(bp1);
  ip->size = 0;
}


void
fat32_iput(struct inode *ip)
{
  acquire(&icache.lock);
  if(ip->ref == 1 && (ip->flags & I_VALID) && ip->nlink == 0){
    // inode has no links and no other references: truncate and free.
    if(ip->flags & I_BUSY)
      panic("iput busy");
    ip->flags |= I_BUSY;
    release(&icache.lock);
    fat32_itrunc(ip);
    ip->type = 0;
    fat32_iupdate(ip);
    acquire(&icache.lock);
    ip->flags = 0;
    wakeup(ip);
  }
  ip->ref--;
  release(&icache.lock);
}

// Common idiom: unlock, then put.
void
fat32_iunlockput(struct inode *ip)
{
  fat32_iunlock(ip);
  fat32_iput(ip);
}



// Copy stat information from inode.
void
fat32_stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

int
fat32_readi(struct inode *ip, char *dst, uint off, uint n)
{
  struct buf *bp, *bp1;
  uint nowSec, lastSec, cnum, nowOff = 0, offset, i, st, j, s1, t1;
  readDbr(ip->dev, &dbr);
  uint tt = (uint)dbr.BytesPerSec * dbr.SecPerClus;
  if(ip->type == T_DEV){
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].read)
      return -1;
    return devsw[ip->major].read(ip, dst, n);
  }
  if (ip->type == T_DIR)
    n = 32;
  else{
    if(off > ip->size || off + n < off)
      return -1;
    if(off + n > ip->size)
      n = ip->size - off;    
  }
  cnum = ip->inum;
  lastSec = 0;
  while (1){
    if (nowOff + tt > off){
      st = getFirstSector(cnum);
      for (i = st, j=nowOff; i < st + dbr.SecPerClus && j < off +n; i++, j+=SECSIZE)
        if (j + SECSIZE > off){
          bp = bread(ip->dev, i);
          if (j < off)
            s1 = off - j;
          else s1 = 0;
          if (j + SECSIZE < off + n)
            t1 = SECSIZE;
          else t1 = off + n - j;
          memmove(dst, bp->data+s1, t1-s1);
          brelse(bp);
          dst += t1-s1;
        }
      if (j >= off + n){
        if (bp1)
          brelse(bp1);
        return n;
      }
    }
    nowOff += tt;
    nowSec = getFATStart(cnum, &offset);
    if (nowSec != lastSec){
      if (bp1)
        brelse(bp1);
      bp1 = bread(ip->dev, nowSec);
      lastSec = nowSec;
    }
    if (*(uint *)(bp1->data + offset) < LAST_FAT_VALUE)
       cnum = *(uint *)(bp1->data + offset);
     else break;
  }
  if (bp1)
    brelse(bp1);
  panic("readi error");
}

int
fat32_writei(struct inode *ip, char *src, uint off, uint n)
{
  struct buf *bp, *bp1;
  uint nowSec, lastSec, cnum, nowOff = 0, offset, i, st, j, s1, t1;
  readDbr(ip->dev, &dbr);
  uint tt = (uint)dbr.BytesPerSec * dbr.SecPerClus;
  if(ip->type == T_DEV){
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].write)
      return -1;
    return devsw[ip->major].write(ip, src, n);
  }
  if(off > ip->size || off + n < off)
    return -1;

  cnum = ip->inum;
  lastSec = 0;
  while (1){
    if (off < tt + nowOff){
      st = getFirstSector(cnum);
      for (i = st, j=nowOff; i < st + dbr.SecPerClus && j < off +n; i++, j+=SECSIZE)
        if (j + SECSIZE > off){
          bp = bread(ip->dev, i);
          if (j < off)
            s1 = off - j;
          else s1 = 0;
          if (j + SECSIZE < off + n)
            t1 = SECSIZE;
          else t1 = off + n - j;
          memmove(bp->data+s1, src, t1-s1);
          bwrite(bp);
          brelse(bp);
          src += t1-s1;
        }
      if (j >= off + n){
        if (bp1){
          updateFATs(bp1);
          bwrite(bp1);
          brelse(bp1);
        }
        if (n > 0 && off + n > ip->size){
          ip->size = off + n;
          fat32_iupdate(ip);
        }
        return n;
      }
    }
    nowOff += tt;
    nowSec = getFATStart(cnum, &offset);
    if (nowSec != lastSec){
      if (bp1){
        updateFATs(bp1);
        bwrite(bp1);
        brelse(bp1);
      }
      bp1 = bread(ip->dev, nowSec);
      lastSec = nowSec;
    }
    if (*(uint *)(bp1->data + offset) < LAST_FAT_VALUE)
       cnum = *(uint *)(bp1->data + offset);
     else{
        cnum = fat32_calloc(ip->dev);
        *(uint *)(bp1->data + offset) = cnum;
     }
  }
}

//PAGEBREAK!
// Directories

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode*
fat32_dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, cnum;
  struct direntry dip;

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");  

  for(off = 0; off < dp->size; off += sizeof(dip)){
    if(fat32_readi(dp, (char*)&dip, off, sizeof(dip)) != sizeof(dip))
      panic("dirlink read");
//    if(dip.inum == 0)
 //     continue;
    if(namecmp(name, (char*)dip.deName) == 0){
      // entry matches path element
      if(poff)
        *poff = off;
      cnum = (dip.deHighClust << 16) | dip.deLowCluster;
      return fat32_iget(dp->dev, cnum, dp->inum);
    }
  }
  return 0;
} 

// Write a new directory entry (name, inum) into the directory dp.
int
fat32_dirlink(struct inode *dp, char *name, struct inode*dp1)
{
  int off;
  struct direntry de;
  struct inode *ip;

  // Check that name is not present.
  if((ip = fat32_dirlookup(dp, name, 0)) != 0){
    fat32_iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(fat32_readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.deName[0] == 0xE5)
      break;
  }

  strncpy((char*)de.deName, name, DIRSIZ);
  de.deHighClust = dp1->inum >> 16;
  de.deLowCluster = dp1->inum & 0xffff;
  de.deFileSize = dp1->size;
  if(fat32_writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("dirlink");
  return 0;
}

static char*
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/')
    path++;
  return path;
}

static struct inode*
fat32_namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  if(*path == '.'){
    ip = proc->cwd;
    if (ip->inum == 2)
      path++;
   }
  while((path = skipelem(path, name)) != 0){
    fat32_ilock(ip);
    if(ip->type != T_DIR){
      fat32_iunlockput(ip);
      return 0;
    }
    if(nameiparent && *path == '\0'){
      // Stop one level early.
      fat32_iunlock(ip);
      return ip;
    }
    if((next = fat32_dirlookup(ip, name, 0)) == 0){
      fat32_iunlockput(ip);
      return 0;
    }
    fat32_iunlockput(ip);
    ip = next;
  }
  if(nameiparent){
    fat32_iput(ip);
    return 0;
  }
  return ip;
}

struct inode*
fat32_namei(char *path)
{
  char name[DIRSIZ];
  return fat32_namex(path, 0, name);
}

struct inode*
fat32_nameiparent(char *path, char *name)
{
  return fat32_namex(path, 1, name);
}
