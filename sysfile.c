//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "fat_fs.h"
#include "file.h"
#include "fcntl.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=proc->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;

  for(fd = 0; fd < NOFILE; fd++){
    if(proc->ofile[fd] == 0){
      proc->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

int
sys_dup(void)
{
  struct file *f;
  int fd;
  
  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

int
sys_read(void)
{
  struct file *f;
  int n;
  char *p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  return fileread(f, p, n);
}

int
sys_write(void)
{
  struct file *f;
  int n;
  char *p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  return filewrite(f, p, n);
}

int
sys_close(void)
{
  int fd;
  struct file *f;
  
  if(argfd(0, &fd, &f) < 0)
    return -1;
  proc->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

int
sys_fstat(void)
{
  struct file *f;
  struct stat *st;
  
  if(argfd(0, 0, &f) < 0 || argptr(1, (void*)&st, sizeof(*st)) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
// int
// sys_link(void)
// {
//   char name[DIRSIZ], *new, *old;
//   struct inode *dp, *ip;

//   if(argstr(0, &old) < 0 || argstr(1, &new) < 0)
//     return -1;

//   begin_op();
//   if((ip = fat32_namei(old)) == 0){
//     end_op();
//     return -1;
//   }

//   fat32_ilock(ip);
//   if(ip->type == T_DIR){
//     fat32_iunlockput(ip);
//     end_op();
//     return -1;
//   }

//   ip->nlink++;
//   fat32_iupdate(ip);
//   fat32_iunlock(ip);

//   if((dp = fat32_nameiparent(new, name)) == 0)
//     goto bad;
//   fat32_ilock(dp);
//   if(dp->dev != ip->dev || fat32_dirlink(dp, name, ip->inum) < 0){
//     fat32_iunlockput(dp);
//     goto bad;
//   }
//   fat32_iunlockput(dp);
//   fat32_iput(ip);

//   end_op();

//   return 0;

// bad:
//   fat32_ilock(ip);
//   ip->nlink--;
//   fat32_iupdate(ip);
//   fat32_iunlockput(ip);
//   end_op();
//   return -1;
// }

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct direntry de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(fat32_readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.deName[0] != 0xE5 && de.deName[0] != 0x00)
      return 0;
  }
  return 1;
}

//PAGEBREAK!
int
sys_unlink(void)
{
  struct inode *ip, *dp;
//  struct dirent de;
  char name[DIRSIZ], *path;
  uint off;

  if(argstr(0, &path) < 0)
    return -1;

//  begin_op();
  if((dp = fat32_nameiparent(path, name)) == 0){
 //   end_op();
    return -1;
  }

  fat32_ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = fat32_dirlookup(dp, name, &off)) == 0)
    goto bad;
  fat32_ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    fat32_iunlockput(ip);
    goto bad;
  }

  fat32_iunlockput(dp);
  if(ip->inum != 2){
    fat32_iupdate(dp);
  }

  ip->nlink--;
  fat32_iupdate(ip);
  fat32_iunlockput(ip);
//  end_op();

  return 0;

bad:

  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  uint off;
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = fat32_nameiparent(path, name)) == 0)
    return 0;
  fat32_ilock(dp);

  if((ip = fat32_dirlookup(dp, name, &off)) != 0){
    fat32_iunlockput(dp);
    fat32_ilock(ip);
    if(type == T_FILE && ip->type == T_FILE)
      return ip;
    fat32_iunlockput(ip);
    return 0;
  }

  if((ip = fat32_ialloc(dp, type)) == 0)
    panic("create: ialloc");

  fat32_ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  fat32_iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
//    dp->nlink++;  // for ".."
    fat32_iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(fat32_dirlink(ip, ".", ip) < 0 || fat32_dirlink(ip, "..", dp) < 0)
      panic("create dots");
  }

  if(fat32_dirlink(dp, name, ip) < 0)
    panic("create: dirlink");

  fat32_iunlockput(dp);

  return ip;
}

int
sys_open(void)
{
  char *path;
  int fd, omode;
  struct file *f;
  struct inode *ip;

  if(argstr(0, &path) < 0 || argint(1, &omode) < 0)
    return -1;

//  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
//      end_op();
      return -1;
    }
  } else {
    if((ip = fat32_namei(path)) == 0){
//      end_op();
      return -1;
    }
    fat32_ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      fat32_iunlockput(ip);
     // end_op();
      return -1;
    }
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    fat32_iunlockput(ip);
//    end_op();
    return -1;
  }
  fat32_iunlock(ip);
//  end_op();

  f->type = FD_INODE;
  f->ip = ip;
  f->off = 0;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
  return fd;
}

int
sys_mkdir(void)
{
  char *path;
  struct inode *ip;

//  begin_op();
  if(argstr(0, &path) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
//    end_op();
    return -1;
  }
  fat32_iunlockput(ip);
//  end_op();
  return 0;
}

int
sys_mknod(void)
{
  struct inode *ip;
  char *path;
  int len;
  int major, minor;
  
//  begin_op();
  if((len=argstr(0, &path)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEV, major, minor)) == 0){
 //   end_op();
    return -1;
  }
  fat32_iunlockput(ip);
 // end_op();
  return 0;
}

int
sys_chdir(void)
{
  char *path;
  struct inode *ip;

 // begin_op();
  if(argstr(0, &path) < 0 || (ip = fat32_namei(path)) == 0){
  //  end_op();
    return -1;
  }
  fat32_ilock(ip);
  if(ip->type != T_DIR){
    fat32_iunlockput(ip);
  //  end_op();
    return -1;
  }
  fat32_iunlock(ip);
  fat32_iput(proc->cwd);
 // end_op();
  proc->cwd = ip;
  return 0;
}

int
sys_exec(void)
{
  char *path, *argv[MAXARG];
  int i;
  uint uargv, uarg;

  if(argstr(0, &path) < 0 || argint(1, (int*)&uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv))
      return -1;
    if(fetchint(uargv+4*i, (int*)&uarg) < 0)
      return -1;
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    if(fetchstr(uarg, &argv[i]) < 0)
      return -1;
  }
  return exec(path, argv);
}

int
sys_pipe(void)
{
  int *fd;
  struct file *rf, *wf;
  int fd0, fd1;

  if(argptr(0, (void*)&fd, 2*sizeof(fd[0])) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      proc->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  fd[0] = fd0;
  fd[1] = fd1;
  return 0;
}
