/*
 *  (C) 2009 by Argonne National Laboratory.
 *      See COPYRIGHT in top-level directory.
 */

#include "scrmfs-runtime-config.h"
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <search.h>
#include <assert.h>
#include <libgen.h>
#define __USE_GNU
#include <pthread.h>

#include <sys/ipc.h>
#include <sys/shm.h>

#include "scrmfs.h"
#include "scrmfs-file.h"

#ifndef HAVE_OFF64_T
typedef int64_t off64_t;
#endif

extern char* __progname_full;

struct scrmfs_job_runtime* scrmfs_global_job = NULL;

//#define SCRMFS_DEBUG
#ifdef SCRMFS_DEBUG
    #define debug(fmt, args... )  printf("%s: "fmt, __func__, ##args)
#else
    #define debug(fmt, args... )
#endif

#ifdef SCRMFS_PRELOAD
#define __USE_GNU
#include <dlfcn.h>
#include <stdlib.h>

#define SCRMFS_FORWARD_DECL(name,ret,args) \
  ret (*__real_ ## name)args = NULL;

#define SCRMFS_DECL(__name) __name

#define MAP_OR_FAIL(func) \
    if (!(__real_ ## func)) \
    { \
        __real_ ## func = dlsym(RTLD_NEXT, #func); \
        if(!(__real_ ## func)) { \
           fprintf(stderr, "SCRMFS failed to map symbol: %s\n", #func); \
           exit(1); \
       } \
    }


#else

#define SCRMFS_FORWARD_DECL(name,ret,args) \
  extern ret __real_ ## name args;

#define SCRMFS_DECL(__name) __wrap_ ## __name

#define MAP_OR_FAIL(func)

#endif

/* ---------------------------------------
 * POSIX wrappers: paths
 * --------------------------------------- */

SCRMFS_FORWARD_DECL(unlink, int, (const char *path));
SCRMFS_FORWARD_DECL(rename, int, (const char *oldpath, const char *newpath));
SCRMFS_FORWARD_DECL(truncate, int, (const char *path, off_t length));
//SCRMFS_FORWARD_DECL(stat, int,( const char *path, struct stat *buf));
SCRMFS_FORWARD_DECL(__lxstat, int, (int vers, const char* path, struct stat *buf));
SCRMFS_FORWARD_DECL(__lxstat64, int, (int vers, const char* path, struct stat64 *buf));
SCRMFS_FORWARD_DECL(__xstat, int, (int vers, const char* path, struct stat *buf));
SCRMFS_FORWARD_DECL(__xstat64, int, (int vers, const char* path, struct stat64 *buf));

/* ---------------------------------------
 * POSIX wrappers: file descriptors
 * --------------------------------------- */

SCRMFS_FORWARD_DECL(creat, int, (const char* path, mode_t mode));
SCRMFS_FORWARD_DECL(creat64, int, (const char* path, mode_t mode));
SCRMFS_FORWARD_DECL(open, int, (const char *path, int flags, ...));
SCRMFS_FORWARD_DECL(open64, int, (const char *path, int flags, ...));
SCRMFS_FORWARD_DECL(read, ssize_t, (int fd, void *buf, size_t count));
SCRMFS_FORWARD_DECL(write, ssize_t, (int fd, const void *buf, size_t count));
SCRMFS_FORWARD_DECL(readv, ssize_t, (int fd, const struct iovec *iov, int iovcnt));
SCRMFS_FORWARD_DECL(writev, ssize_t, (int fd, const struct iovec *iov, int iovcnt));
SCRMFS_FORWARD_DECL(pread, ssize_t, (int fd, void *buf, size_t count, off_t offset));
SCRMFS_FORWARD_DECL(pread64, ssize_t, (int fd, void *buf, size_t count, off64_t offset));
SCRMFS_FORWARD_DECL(pwrite, ssize_t, (int fd, const void *buf, size_t count, off_t offset));
SCRMFS_FORWARD_DECL(pwrite64, ssize_t, (int fd, const void *buf, size_t count, off64_t offset));
SCRMFS_FORWARD_DECL(lseek, off_t, (int fd, off_t offset, int whence));
SCRMFS_FORWARD_DECL(lseek64, off64_t, (int fd, off64_t offset, int whence));
SCRMFS_FORWARD_DECL(fsync, int, (int fd));
SCRMFS_FORWARD_DECL(fdatasync, int, (int fd));
SCRMFS_FORWARD_DECL(mmap, void*, (void *addr, size_t length, int prot, int flags, int fd, off_t offset));
SCRMFS_FORWARD_DECL(mmap64, void*, (void *addr, size_t length, int prot, int flags, int fd, off64_t offset));
SCRMFS_FORWARD_DECL(__fxstat, int, (int vers, int fd, struct stat *buf));
SCRMFS_FORWARD_DECL(__fxstat64, int, (int vers, int fd, struct stat64 *buf));
SCRMFS_FORWARD_DECL(close, int, (int fd));

/* ---------------------------------------
 * POSIX wrappers: file streams
 * --------------------------------------- */

SCRMFS_FORWARD_DECL(fopen, FILE*, (const char *path, const char *mode));
SCRMFS_FORWARD_DECL(fopen64, FILE*, (const char *path, const char *mode));
SCRMFS_FORWARD_DECL(fread, size_t, (void *ptr, size_t size, size_t nmemb, FILE *stream));
SCRMFS_FORWARD_DECL(fwrite, size_t, (const void *ptr, size_t size, size_t nmemb, FILE *stream));
SCRMFS_FORWARD_DECL(fseek, int, (FILE *stream, long offset, int whence));
SCRMFS_FORWARD_DECL(fclose, int, (FILE *fp));

static int scrmfs_initialized = 0;
static int scrmfs_stack_init_done = 0;

/* global persistent memory block (metadata + data) */
static void* scrmfs_superblock = NULL;
static void* free_fid_stack = NULL;
static void* free_chunk_stack = NULL;
static scrmfs_filename_t* scrmfs_filelist = NULL;
static scrmfs_filemeta_t* scrmfs_filemetas = NULL;
static chunk_t *chunk = NULL;
static active_fds_t scrmfs_active_fds[SCRMFS_MAX_FILEDESCS];
static rlim_t scrmfs_fd_limit;
static char*  scrmfs_mount_prefix = NULL;
static size_t scrmfs_mount_prefixlen = 0;
static key_t  scrmfs_mount_key = 0;

#if 0
/* simple memcpy which compilers should be able to vectorize
 * from: http://software.intel.com/en-us/articles/memcpy-performance/
 * icc -restrict -O3 ... */
static inline void* scrmfs_memcpy(void *restrict b, const void *restrict a, size_t n)
{
    char *s1 = b;
    const char *s2 = a;
    for(; 0<n; --n)*s1++ = *s2++;
    return b;
}
#endif

/* mount memfs at some prefix location */
int scrmfs_mount(const char prefix[], int rank)
{
    scrmfs_mount_prefix = strdup(prefix);
    scrmfs_mount_prefixlen = strlen(scrmfs_mount_prefix);
    //scrmfs_mount_key = SCRMFS_SUPERBLOCK_KEY + rank;
    scrmfs_mount_key = IPC_PRIVATE;
    return 0;
}

/* initialize our global pointers into the given superblock */
static void* scrmfs_init_globals(void* superblock)
{
    char* ptr = (char*) superblock;

    /* SCR stack to manage metadata structures */
    free_fid_stack = ptr;
    ptr += scrmfs_stack_bytes(SCRMFS_MAX_FILES);

    /* record list of file names in persistent mem */
    scrmfs_filelist = (scrmfs_filename_t*) ptr;
    ptr += SCRMFS_MAX_FILES * sizeof(scrmfs_filename_t);

    /* chunk offset map */
    scrmfs_filemetas = (scrmfs_filemeta_t*) ptr;
    ptr += SCRMFS_MAX_FILES * sizeof(scrmfs_filemeta_t);

    /* SCR stack to manage actual data chunks */
    free_chunk_stack = ptr;
    ptr += scrmfs_stack_bytes(SCRMFS_MAX_CHUNKS);

    /* chunk repository */
    chunk = (chunk_t*) ptr;
    ptr += SCRMFS_MAX_CHUNKS * sizeof(chunk_t);

    return ptr;
}

/* grab a superblock shm segment from SCR_Init */
/* currently called from each open call */
static void* scrmfs_get_shmblock(size_t size, key_t key)
{
    void *scr_shmblock = NULL;

    /* TODO:improve error-propogation */
    //int scr_shmblock_shmid = shmget(key, size, IPC_CREAT | IPC_EXCL | S_IRWXU | SHM_HUGETLB);
    int scr_shmblock_shmid = shmget(key, size, IPC_CREAT | IPC_EXCL | S_IRWXU);
    if ( scr_shmblock_shmid < 0 ) {
        if ( errno == EEXIST ) {
            /* Superblock exists. Donot init, just get and attach */
            scr_shmblock_shmid = shmget(key, size, 0);
            scr_shmblock = shmat(scr_shmblock_shmid, NULL, 0);
            if(scr_shmblock < 0) {
                perror("shmat() failed");
                return NULL;
            }
            debug("Superblock exists at %p\n!",scr_shmblock);

            /* init our global variables to point to spots in superblock */
            scrmfs_init_globals(scr_shmblock);
        } else {
            perror("shmget() failed");
            return NULL;
        }
    } else {
        /* valid segment created, attach to proc and init structures */
        scr_shmblock = shmat(scr_shmblock_shmid, NULL, 0);
        if(scr_shmblock < 0) {
            perror("shmat() failed");
        }
        debug("Superblock created at %p\n!",scr_shmblock);

        /* init our global variables to point to spots in superblock */
        scrmfs_init_globals(scr_shmblock);

        /* initialize block allocators within block */
        if (! scrmfs_stack_init_done) {
            scrmfs_stack_init(free_fid_stack, SCRMFS_MAX_FILES);
            scrmfs_stack_init(free_chunk_stack, SCRMFS_MAX_CHUNKS);
            scrmfs_stack_init_done = 1;
            debug("Meta-stacks initialized!\n");
        }
    }
    
    return scr_shmblock;
}

static int scrmfs_init()
{
    /* TODO: expose API as a init call that can be called from SCR_Init */ 

    if (! scrmfs_initialized) {
        /* record the max fd for the system */
        /* RLIMIT_NOFILE specifies a value one greater than the maximum
         * file descriptor number that can be opened by this process */
        struct rlimit *r_limit = malloc(sizeof(r_limit));
        if (r_limit == NULL) {
            perror("failed to allocate memory for call to getrlimit");
            return 1;
        }
        if (getrlimit(RLIMIT_NOFILE, r_limit) < 0) {
            perror("rlimit failed:");
            return 1;
        }
        scrmfs_fd_limit = r_limit->rlim_cur;
        free(r_limit);
        debug("FD limit for system = %ld\n",scrmfs_fd_limit);

        scrmfs_mount("/tmp", 0);
        
        /* determine the size of the superblock */
        /* generous allocation for chunk map (one file can take entire space)*/
        size_t superblock_size = scrmfs_stack_bytes(SCRMFS_MAX_FILES) +
                                 (SCRMFS_MAX_FILES * sizeof(scrmfs_filename_t)) +
                                 (SCRMFS_MAX_FILES * sizeof(scrmfs_filemeta_t)) +
                                 scrmfs_stack_bytes(SCRMFS_MAX_CHUNKS) +
                                 (SCRMFS_MAX_CHUNKS * sizeof(chunk_t));

        /* get a superblock of persistent memory and initialize our
         * global variables for this block */
        scrmfs_superblock = scrmfs_get_shmblock (superblock_size, scrmfs_mount_key);
        if(scrmfs_superblock == NULL)
        {
            debug("scrmfs_get_shmblock() failed\n");
            return 1;
        }

        /* remember that we've now initialized the library */
        scrmfs_initialized = 1;
    }

    return 0;
}

/* sets flag if the path is a special path */
static inline int scrmfs_intercept_path(const char* path)
{
    /* initialize our globals if we haven't already */
    if (! scrmfs_initialized) {
      scrmfs_init();
    }

    /* if the path starts with our mount point, intercept it */
    if (strncmp(path, scrmfs_mount_prefix, scrmfs_mount_prefixlen) == 0) {
        return 1;
    }
    return 0;
}

/* given an fd, return 1 if we should intercept this file, 0 otherwise,
 * convert fd to new fd value if needed */
static inline void scrmfs_intercept_fd(int* fd, int* intercept)
{
    int oldfd = *fd;

    /* initialize our globals if we haven't already */
    if (! scrmfs_initialized) {
      scrmfs_init();
    }

    if (oldfd < scrmfs_fd_limit) {
        /* this fd is a real system fd, so leave it as is */
        *intercept = 0;
    } else {
        /* this is an fd we generated and returned to the user,
         * so intercept the call and shift the fd */
        int newfd = oldfd - scrmfs_fd_limit;
        *intercept = 1;
        *fd = newfd;
        debug("Changing fd from exposed %d to internal %d\n",oldfd,newfd);
    }

    return;
}

/* given a file descriptor, return the file id */
static inline int scrmfs_get_fid_from_fd(int fd)
{
  /* check that file descriptor is within range */
  if (fd < 1 || fd > SCRMFS_MAX_FILEDESCS) {
    return -1;
  }

  /* right now, the file descriptor is equal to the file id */
  return fd;
}

/* given a path, return the file id */
static int scrmfs_get_fid_from_path(const char* path)
{
    int i = 0;
    while (i < SCRMFS_MAX_FILES)
    {
        if(scrmfs_filelist[i].in_use &&
           strcmp((void *)&scrmfs_filelist[i].filename, path) == 0)
        {
            debug("File found: scrmfs_filelist[%d].filename = %s\n",i,(void *)&scrmfs_filelist[i].filename);
            return i;
        }
        i++;
    }
    return -1;
}

/* given a file id, return a pointer to the meta data,
 * otherwise return NULL */
static inline scrmfs_filemeta_t* scrmfs_get_meta_from_fid(int fid)
{
    /* check that the file id is within range of our array */
    if (fid >= 0 && fid < SCRMFS_MAX_FILES) {
        /* get a pointer to the file meta data structure */
        scrmfs_filemeta_t* meta = &scrmfs_filemetas[fid];
        return meta;
    }
    return NULL;
}

/* given a chunk id and an offset within that chunk, return the pointer
 * to the memory location corresponding to that location */
static inline void* scrmfs_compute_chunk_buf(const scrmfs_filemeta_t* meta, int id, off_t offset)
{
    /* identify physical chunk id, find start of its buffer and add the offset */
    int chunk_id = meta->chunk_ids[id];
    char* start = chunk[chunk_id].buf;
    char* buf = start + offset;
    return (void*)buf;
}

/* ---------------------------------------
 * File system operations
 * --------------------------------------- */

/* truncate file id to given length */
static int scrmfs_truncate_fid(int fid, off_t length)
{
    /* get meta data for this file */
    scrmfs_filemeta_t* meta = scrmfs_get_meta_from_fid(fid);

    /* determine the number of chunks to leave after truncating */
    off_t num_chunks = 0;
    if (length > 0) {
      num_chunks = (length >> SCRMFS_CHUNK_BITS) + 1;
    }

    /* clear off any extra chunks */
    while (meta->chunks > num_chunks) {
        meta->chunks--;
        debug("truncate chunk %d\n", meta->chunk_ids[meta->chunks]);
        scrmfs_stack_push(free_chunk_stack, meta->chunk_ids[meta->chunks]);
    }
    
    /* set the new size */
    meta->size = length;

    return 0;
}

/* return file resources to free pools */
static int scrmfs_unlink_fid(int fid)
{
    /* return data to free pools */
    scrmfs_truncate_fid(fid, 0);

    /* set this file id as not in use */
    scrmfs_filelist[fid].in_use = 0;

    /* add this id back to the free stack */
    scrmfs_stack_push(free_fid_stack, fid);

    return 0;
}

/* ---------------------------------------
 * POSIX wrappers: paths
 * --------------------------------------- */

int SCRMFS_DECL(rename)(const char *oldpath, const char *newpath)
{
    /* TODO: allow oldpath / newpath to split across memfs and normal
     * linux fs, which means we'll need to do a read / write */

    /* check whether the old path is in our file system */
    if (scrmfs_intercept_path(oldpath)) {
        /* for now, we can only rename within our file system */
        if (! scrmfs_intercept_path(newpath)) {
            /* ERROR: can't yet rename across file systems */
            errno = EXDEV;
            return -1;
        }

        /* verify that we really have a file by the old name */
        int fid = scrmfs_get_fid_from_path(oldpath);
        debug("orig file in position %d\n",fid);
        if (fid < 0) {
            /* ERROR: oldname does not exist */
            debug("Couldn't find entry for %s in SCRMFS\n",oldpath);
            errno = ENOENT;
            return -1;
        }

        /* verify that we don't already have a file by the new name */
        if (scrmfs_get_fid_from_path(newpath) < 0) {
            /* check that new name is within bounds */
            size_t newpathlen = strlen(newpath) + 1;
            if (newpathlen > SCRMFS_MAX_FILENAME) {
                errno = ENAMETOOLONG;
                return -1;
            }

            /* finally overwrite the old name with the new name */
            debug("Changing %s to %s\n",(void *)&scrmfs_filelist[fid].filename, newpath);
            strcpy((void *)&scrmfs_filelist[fid].filename, newpath);
        } else {
            /* ERROR: new name already exists */
            debug("File %s exists\n",newpath);
            errno = EEXIST;
            return -1;
        }
        return 0;
    } else {
        /* for now, we can only rename within our file system */
        if (scrmfs_intercept_path(newpath)) {
            /* ERROR: can't yet rename across file systems */
            errno = EXDEV;
            return -1;
        }

        /* both files are normal linux files, delegate to system call */
        MAP_OR_FAIL(rename);
        int ret = __real_rename(oldpath,newpath);
        return ret;
    }
}

int SCRMFS_DECL(truncate)(const char* path, off_t length)
{
    /* determine whether we should intercept this path or not */
    if (scrmfs_intercept_path(path)) {
        /* lookup the fd for the path */
        int fid = scrmfs_get_fid_from_path(path);
        if (fid < 0) {
            /* ERROR: file does not exist */
            debug("Couldn't find entry for %s in SCRMFS\n",path);
            errno = ENOENT;
            return -1;
        }

        /* truncate the file */
        scrmfs_truncate_fid(fid, length);

        return 0;
    } else {
        MAP_OR_FAIL(truncate);
        int ret = __real_truncate(path, length);
        return ret;
    }
}

int SCRMFS_DECL(unlink)(const char *path)
{
    /* determine whether we should intercept this path or not */
    if (scrmfs_intercept_path(path)) {
        /* get file id for path name */
        int fid = scrmfs_get_fid_from_path(path);
        if (fid < 0) {
            /* ERROR: file does not exist */
            debug("Couldn't find entry for %s in SCRMFS\n",path);
            errno = ENOENT;
            return -1;
        }

        /* delete the file */
        scrmfs_unlink_fid(fid);

        return 0;
    } else {
        MAP_OR_FAIL(unlink);
        int ret = __real_unlink(path);
        return ret;
    }
}

/*
int SCRMFS_DECL(__stat)( const char *path, struct stat *buf)
{

    int ret;
    debug("stat called..\n");

    MAP_OR_FAIL(__stat);
    ret = __real_stat(path,buf);
    
    return ret;

} */

int SCRMFS_DECL(__xstat)(int vers, const char *path, struct stat *buf)
{
    debug("xstat was called for %s....\n",path);
    if (scrmfs_intercept_path(path)) {
        /* get file id for path */
        int fid = scrmfs_get_fid_from_path(path);
        if (fid < 0) {
            /* file doesn't exist */
            errno = ENOENT;
            return -1;
        }

        /* get meta data for this file */
        scrmfs_filemeta_t* meta = scrmfs_get_meta_from_fid(fid);
        
        /* set the file size */
        buf->st_size = meta->size;

        return 0;
    } else { 
        MAP_OR_FAIL(__xstat);
        int ret = __real___xstat(vers, path, buf);
        return ret;
    }
}

int SCRMFS_DECL(__xstat64)(int vers, const char *path, struct stat64 *buf)
{
    /* check whether we should intercept this path */
    if (scrmfs_intercept_path(path)) {
        /* ERROR: fn not yet supported */
        fprintf(stderr, "Function not yet supported @ %s:%d\n", __FILE__, __LINE__);
        errno = ENOENT;
        return -1;
    } else {
        MAP_OR_FAIL(__xstat64);
        int ret = __real___xstat64(vers, path, buf);
        return ret;
    }
}

int SCRMFS_DECL(__lxstat)(int vers, const char *path, struct stat *buf)
{
    /* check whether we should intercept this path */
    if (scrmfs_intercept_path(path)) {
        /* ERROR: fn not yet supported */
        fprintf(stderr, "Function not yet supported @ %s:%d\n", __FILE__, __LINE__);
        errno = ENOENT;
        return -1;
    } else {
        MAP_OR_FAIL(__lxstat);
        int ret = __real___lxstat(vers, path, buf);
        return ret;
    }
}

int SCRMFS_DECL(__lxstat64)(int vers, const char *path, struct stat64 *buf)
{
    /* check whether we should intercept this path */
    if (scrmfs_intercept_path(path)) {
        /* ERROR: fn not yet supported */
        fprintf(stderr, "Function not yet supported @ %s:%d\n", __FILE__, __LINE__);
        errno = ENOENT;
        return -1;
    } else {
        MAP_OR_FAIL(__lxstat64);
        int ret = __real___lxstat64(vers, path, buf);
        return ret;
    }
}

/* MISSING:
 * access()
 * mkdir(), rmdir(), etc */

/* ---------------------------------------
 * POSIX wrappers: file descriptors
 * --------------------------------------- */

int SCRMFS_DECL(creat)(const char* path, mode_t mode)
{
    /* check whether we should intercept this path */
    if (scrmfs_intercept_path(path)) {
        /* ERROR: fn not yet supported */
        fprintf(stderr, "Function not yet supported @ %s:%d\n", __FILE__, __LINE__);
        return -1;
    } else {
        MAP_OR_FAIL(creat);
        int ret = __real_creat(path, mode);
        return ret ;
    }
}

int SCRMFS_DECL(creat64)(const char* path, mode_t mode)
{
    /* check whether we should intercept this path */
    if (scrmfs_intercept_path(path)) {
        /* ERROR: fn not yet supported */
        fprintf(stderr, "Function not yet supported @ %s:%d\n", __FILE__, __LINE__);
        return -1;
    } else {
        MAP_OR_FAIL(creat64);
        int ret = __real_creat64(path, mode);
        return ret;
    }
}

int SCRMFS_DECL(open)(const char *path, int flags, ...)
{
    int ret;

    /* if O_CREAT is set, we should also have some mode flags */
    int mode = 0;
    if (flags & O_CREAT) {
        va_list arg;
        va_start(arg, flags);
        mode = va_arg(arg, int);
        va_end(arg);
    }
    
    /* determine whether we should intercept this path */
    if (scrmfs_intercept_path(path)) {
        /* TODO: handle relative paths using current working directory */

        /* TODO: check that path is short enough */
        size_t pathlen = strlen(path) + 1;
        if (pathlen > SCRMFS_MAX_FILENAME) {
            errno = ENAMETOOLONG;
            return -1;
        }

        /* assume that we'll place the file pointer at the start of the file */
        off_t pos = 0;

        /* check whether this file already exists */
        int fid = scrmfs_get_fid_from_path(path);
        debug("scrmfs_get_fid_from_path() gave %d\n",fid);
        if (fid < 0) {
            /* file does not exist */

            /* create file if O_CREAT is set */
            if (flags & O_CREAT) {
                debug("Couldn't find entry for %s in SCRMFS\n",path);
                debug("scrmfs_superblock = %p; free_fid_stack = %p; free_chunk_stack = %p; scrmfs_filelist = %p; chunks = %p\n",
                                            scrmfs_superblock,free_fid_stack,free_chunk_stack,scrmfs_filelist,chunk);

                /* allocate a file id slot for this new file */
                fid = scrmfs_stack_pop(free_fid_stack);
                debug("scrmfs_stack_pop() gave %d\n",fid);
                if (fid < 0) {
                    /* need to create a new file, but we can't */
                    debug("scrmfs_stack_pop() failed (%d)\n",fid);
                    errno = ENOSPC;
                    return -1;
                }

                /* mark this slot as in use and copy the filename */
                scrmfs_filelist[fid].in_use = 1;
                strcpy((void *)&scrmfs_filelist[fid].filename, path);
                debug("Filename %s got scrmfs fd %d\n",scrmfs_filelist[fid].filename,fid);

                /* initialize meta data */
                scrmfs_filemeta_t* meta = scrmfs_get_meta_from_fid(fid);
                meta->size = 0;
                meta->chunks = 0;
            } else {
                /* ERROR: trying to open a file that does not exist without O_CREATE */
                debug("Couldn't find entry for %s in SCRMFS\n",path);
                errno = ENOENT;
                return -1;
            }
        } else {
            /* file already exists */

            /* if O_CREAT and O_EXCL are set, this is an error */
            if ((flags & O_CREAT) && (flags & O_EXCL)) {
                /* ERROR: trying to open a file that exists with O_CREATE and O_EXCL */
                errno = EEXIST;
                return -1;
            }

            /* if O_TRUNC is set with RDWR or WRONLY, need to truncate file */
            if ((flags & O_TRUNC) && (flags & (O_RDWR | O_WRONLY))) {
                scrmfs_truncate_fid(fid, 0);
            }

            /* if O_APPEND is set, we need to place file pointer at end of file */
            if (flags & O_APPEND) {
                scrmfs_filemeta_t* meta = scrmfs_get_meta_from_fid(fid);
                pos = meta->size;
            }
        }

        /* TODO: allocate a free file descriptor and associate it with fid */
        /* set file pointer */
        scrmfs_active_fds[fid].pos = pos;
        debug("SCRMFS_open generated fd %d for file %s\n",fid,path);    

        /* don't conflict with active system fds that range from 0 - (fd_limit) */
        ret = fid + scrmfs_fd_limit;
    } else {
        MAP_OR_FAIL(open);
        if (flags & O_CREAT) {
            ret = __real_open(path, flags, mode);
        } else {
            ret = __real_open(path, flags);
        }
    }

    return ret;
}

int SCRMFS_DECL(open64)(const char* path, int flags, ...)
{
    int ret;

    /* if O_CREAT is set, we should also have some mode flags */
    int mode = 0;
    if (flags & O_CREAT) {
        va_list arg;
        va_start(arg, flags);
        mode = va_arg(arg, int);
        va_end(arg);
    }

    /* check whether we should intercept this path */
    if (scrmfs_intercept_path(path)) {
        /* ERROR: fn not yet supported */
        fprintf(stderr, "Function not yet supported @ %s:%d\n", __FILE__, __LINE__);
        return -1;
    } else {
        MAP_OR_FAIL(open64);
        if (flags & O_CREAT) {
            ret = __real_open64(path, flags, mode);
        } else {
            ret = __real_open64(path, flags);
        }
    }

    return ret;
}

off_t SCRMFS_DECL(lseek)(int fd, off_t offset, int whence)
{
    int intercept;
    scrmfs_intercept_fd(&fd, &intercept);
    if (intercept) {
        /* TODO: check that fd is actually in use */

        /* get the file id for this file descriptor */
        int fid = scrmfs_get_fid_from_fd(fd);

        /* check that file descriptor is good */
        scrmfs_filemeta_t* meta = scrmfs_get_meta_from_fid(fid);
        if (meta == NULL) {
            /* bad file descriptor */
            errno = EBADF;
            return (off_t)-1;
        }

        debug("seeking from %ld\n",scrmfs_active_fds[fd].pos);        
        off_t current_pos = scrmfs_active_fds[fd].pos;
        if (whence == SEEK_SET) {
            /* seek to offset */
            current_pos = offset;
        } else if (whence == SEEK_CUR) {
            /* seek to current position + offset */
            current_pos += offset;
        } else if (whence == SEEK_END) {
            /* seek to EOF + offset */
            current_pos = meta->size + offset;
        }
        scrmfs_active_fds[fd].pos = current_pos;

        debug("seeking to %ld\n",scrmfs_active_fds[fd].pos);        
        return current_pos;
    } else {
        MAP_OR_FAIL(lseek);
        int ret = __real_lseek(fd, offset, whence);
        return ret;
    }
}

off64_t SCRMFS_DECL(lseek64)(int fd, off64_t offset, int whence)
{
    /* check whether we should intercept this path */
    int intercept;
    scrmfs_intercept_fd(&fd, &intercept);
    if (intercept) {
        /* ERROR: fn not yet supported */
        fprintf(stderr, "Function not yet supported @ %s:%d\n", __FILE__, __LINE__);
        errno = EBADF;
        return (off64_t)-1;
    } else {
      MAP_OR_FAIL(lseek64);
      off64_t ret = __real_lseek64(fd, offset, whence);
      return ret;
    }
}

ssize_t SCRMFS_DECL(read)(int fd, void *buf, size_t count)
{
    ssize_t ret;

    int intercept;
    scrmfs_intercept_fd(&fd, &intercept);
    if (intercept) {
        /* get the file id for this file descriptor */
        int fid = scrmfs_get_fid_from_fd(fd);

        /* get a pointer to the file meta data structure */
        scrmfs_filemeta_t* meta = scrmfs_get_meta_from_fid(fid);
        if (meta == NULL) {
            /* ERROR: invalid file descriptor */
            errno = EBADF;
            return -1;
        }

        /* get the current file pointer position */
        off_t oldpos = scrmfs_active_fds[fd].pos;

        /* check that we don't read past the end of the file */
        off_t newpos = oldpos + count;
        if (newpos > meta->size) {
            /* trying to read past the end of the file, so
             * adjust new file pointer position and count */
            newpos = meta->size;
            count = (size_t) (newpos - oldpos);
        }

        /* update position */
        scrmfs_active_fds[fd].pos = newpos;

        /* assume that we'll read all data */
        ret = count;

        /* get pointer to position within current chunk */
        int chunk_id = oldpos >> SCRMFS_CHUNK_BITS;
        off_t chunk_offset = oldpos & SCRMFS_CHUNK_MASK;
        void* chunk_buf = scrmfs_compute_chunk_buf(meta, chunk_id, chunk_offset);

        /* determine how many bytes remain in the current chunk */
        size_t remaining = SCRMFS_CHUNK_SIZE - chunk_offset;
        if (count <= remaining) {
            /* all bytes for this write fit within the current chunk */
            memcpy(buf, chunk_buf, count);
        } else {
            /* read what's left of current chunk */
            char* ptr = (char*) buf;
            memcpy(ptr, chunk_buf, remaining);
            ptr += remaining;

            /* read from the next chunk */
            size_t read = remaining;
            while (read < count) {
                /* get pointer to start of next chunk */
                chunk_id++;
                chunk_buf = scrmfs_compute_chunk_buf(meta, chunk_id, 0);

                /* compute size to read from this chunk */
                size_t to_read = count - read;
                if (to_read > SCRMFS_CHUNK_SIZE) {
                    to_read = SCRMFS_CHUNK_SIZE;
                }

                /* write data */
                memcpy(ptr, chunk_buf, to_read);
                ptr += to_read;

                /* update number of bytes written */
                read += to_read;
            }
        }
    } else {
        MAP_OR_FAIL(read);
        ret = __real_read(fd, buf, count);
    }

    return ret;
}

ssize_t SCRMFS_DECL(write)(int fd, const void *buf, size_t count)
{
    ssize_t ret;

    /* check file descriptor to determine whether we should pick off this call */
    int intercept;
    scrmfs_intercept_fd(&fd, &intercept);
    if (intercept) {
        /* assume we'll succeed with the write,
         * so set return value to count */
        ret = count;

        /* get the file id for this file descriptor */
        int fid = scrmfs_get_fid_from_fd(fd);

        /* get a pointer to the file meta data structure */
        scrmfs_filemeta_t* meta = scrmfs_get_meta_from_fid(fid);
        if (meta == NULL) {
            /* ERROR: invalid file descriptor */
            errno = EBADF;
            return -1;
        }
 
        /* Raghu: Added a new struct to store positions of active fids.
         * This struct is not part of superblock - doesn't have to be persistent */
        off_t oldpos = scrmfs_active_fds[fd].pos;
        off_t newpos = oldpos + count;
        scrmfs_active_fds[fd].pos = newpos;

        /* if we write past the end of the file, we need to update the
         * file size, and we may need to allocate more chunks */
        if (newpos > meta->size) {
            /* update file size */
            meta->size = newpos;

            /* determine whether we need to allocate more chunks */
            off_t maxsize = meta->chunks << SCRMFS_CHUNK_BITS;
            if (newpos > maxsize) {
                /* compute number of additional bytes we need */
                off_t additional = newpos - maxsize;
                while (additional > 0)
                {
                  /* allocate a new chunk */
                  int id = scrmfs_stack_pop(free_chunk_stack);
                  if (id < 0)
                  {
                      debug("scrmfs_stack_pop() failed (%d)\n", id);

                      /* ERROR: device out of space */
                      errno = ENOSPC;
                      return -1;
                  }

                  /* add it to the chunk list of this file */
                  meta->chunk_ids[meta->chunks] = id;
                  meta->chunks++;

                  /* subtract bytes from the number we need */
                  additional -= SCRMFS_CHUNK_SIZE;
                }
            }
        }

        /* get pointer to position within current chunk */
        int chunk_id = oldpos >> SCRMFS_CHUNK_BITS;
        off_t chunk_offset = oldpos & SCRMFS_CHUNK_MASK;
        void* chunk_buf = scrmfs_compute_chunk_buf(meta, chunk_id, chunk_offset);

        /* determine how many bytes remain in the current chunk */
        size_t remaining = SCRMFS_CHUNK_SIZE - chunk_offset;
        if (count <= remaining) {
            /* all bytes for this write fit within the current chunk */
            memcpy(chunk_buf, buf, count);
//            _intel_fast_memcpy(chunk_buf, buf, count);
//            scrmfs_memcpy(chunk_buf, buf, count);
        } else {
            /* otherwise, fill up the remainder of the current chunk */
            char* ptr = (char*) buf;
            memcpy(chunk_buf, ptr, remaining);
//            _intel_fast_memcpy(chunk_buf, ptr, remaining);
//            scrmfs_memcpy(chunk_buf, ptr, remaining);
            ptr += remaining;

            /* then write the rest of the bytes starting from beginning
             * of chunks */
            size_t written = remaining;
            while (written < count) {
                /* get pointer to start of next chunk */
                chunk_id++;
                chunk_buf = scrmfs_compute_chunk_buf(meta, chunk_id, 0);

                /* compute size to write to this chunk */
                size_t nwrite = count - written;
                if (nwrite > SCRMFS_CHUNK_SIZE) {
                  nwrite = SCRMFS_CHUNK_SIZE;
                }

                /* write data */
                memcpy(chunk_buf, ptr, nwrite);
//                _intel_fast_memcpy(chunk_buf, ptr, nwrite);
//                scrmfs_memcpy(chunk_buf, ptr, nwrite);
                ptr += nwrite;

                /* update number of bytes written */
                written += nwrite;
            }
        }
    } else {
        /* lookup and record address of real function */
        MAP_OR_FAIL(write);

        /* don't intercept, just pass the call on to the real write call */
        ret = __real_write(fd, buf, count);
    }

    return(ret);
}

ssize_t SCRMFS_DECL(readv)(int fd, const struct iovec *iov, int iovcnt)
{
    /* check whether we should intercept this path */
    int intercept;
    scrmfs_intercept_fd(&fd, &intercept);
    if (intercept) {
        /* ERROR: fn not yet supported */
        fprintf(stderr, "Function not yet supported @ %s:%d\n", __FILE__, __LINE__);
        errno = EBADF;
        return -1;
    } else {
        MAP_OR_FAIL(readv);
        ssize_t ret = __real_readv(fd, iov, iovcnt);
        return ret;
    }
}

ssize_t SCRMFS_DECL(writev)(int fd, const struct iovec *iov, int iovcnt)
{
    /* check whether we should intercept this path */
    int intercept;
    scrmfs_intercept_fd(&fd, &intercept);
    if (intercept) {
        /* ERROR: fn not yet supported */
        fprintf(stderr, "Function not yet supported @ %s:%d\n", __FILE__, __LINE__);
        errno = EBADF;
        return -1;
    } else {
        MAP_OR_FAIL(writev);
        ssize_t ret = __real_writev(fd, iov, iovcnt);
        return ret;
    }
}

ssize_t SCRMFS_DECL(pread)(int fd, void *buf, size_t count, off_t offset)
{
    /* check whether we should intercept this path */
    int intercept;
    scrmfs_intercept_fd(&fd, &intercept);
    if (intercept) {
        /* ERROR: fn not yet supported */
        fprintf(stderr, "Function not yet supported @ %s:%d\n", __FILE__, __LINE__);
        errno = EBADF;
        return -1;
    } else {
        MAP_OR_FAIL(pread);
        ssize_t ret = __real_pread(fd, buf, count, offset);
        return ret;
    }
}

ssize_t SCRMFS_DECL(pread64)(int fd, void *buf, size_t count, off64_t offset)
{
    /* check whether we should intercept this path */
    int intercept;
    scrmfs_intercept_fd(&fd, &intercept);
    if (intercept) {
        /* ERROR: fn not yet supported */
        fprintf(stderr, "Function not yet supported @ %s:%d\n", __FILE__, __LINE__);
        errno = EBADF;
        return -1;
    } else {
        MAP_OR_FAIL(pread64);
        ssize_t ret = __real_pread64(fd, buf, count, offset);
        return ret;
    }
}

ssize_t SCRMFS_DECL(pwrite)(int fd, const void *buf, size_t count, off_t offset)
{
    /* check whether we should intercept this path */
    int intercept;
    scrmfs_intercept_fd(&fd, &intercept);
    if (intercept) {
        /* ERROR: fn not yet supported */
        fprintf(stderr, "Function not yet supported @ %s:%d\n", __FILE__, __LINE__);
        errno = EBADF;
        return -1;
    } else {
        MAP_OR_FAIL(pwrite);
        ssize_t ret = __real_pwrite(fd, buf, count, offset);
        return ret;
    }
}

ssize_t SCRMFS_DECL(pwrite64)(int fd, const void *buf, size_t count, off64_t offset)
{
    /* check whether we should intercept this path */
    int intercept;
    scrmfs_intercept_fd(&fd, &intercept);
    if (intercept) {
        /* ERROR: fn not yet supported */
        fprintf(stderr, "Function not yet supported @ %s:%d\n", __FILE__, __LINE__);
        errno = EBADF;
        return -1;
    } else {
        MAP_OR_FAIL(pwrite64);
        ssize_t ret = __real_pwrite64(fd, buf, count, offset);
        return ret;
    }
}

int SCRMFS_DECL(fsync)(int fd)
{
    int intercept;
    scrmfs_intercept_fd(&fd, &intercept);
    if (intercept) {

        /* TODO: check that fd is actually in use */

        /* nothing to do in our case */
        return 0;
    } else {
        MAP_OR_FAIL(fsync);
        int ret = __real_fsync(fd);
        return ret;
    }
}

int SCRMFS_DECL(fdatasync)(int fd)
{
    /* check whether we should intercept this path */
    int intercept;
    scrmfs_intercept_fd(&fd, &intercept);
    if (intercept) {
        /* ERROR: fn not yet supported */
        fprintf(stderr, "Function not yet supported @ %s:%d\n", __FILE__, __LINE__);
        errno = EBADF;
        return -1;
    } else {
        MAP_OR_FAIL(fdatasync);
        int ret = __real_fdatasync(fd);
        return ret;
    }
}

void* SCRMFS_DECL(mmap)(void *addr, size_t length, int prot, int flags,
    int fd, off_t offset)
{
    /* check whether we should intercept this path */
    int intercept;
    scrmfs_intercept_fd(&fd, &intercept);
    if (intercept) {
        /* ERROR: fn not yet supported */
        fprintf(stderr, "Function not yet supported @ %s:%d\n", __FILE__, __LINE__);
        errno = EBADF;
        return MAP_FAILED;
    } else {
        MAP_OR_FAIL(mmap);
        void* ret = __real_mmap(addr, length, prot, flags, fd, offset);
        return ret;
    }
}

void* SCRMFS_DECL(mmap64)(void *addr, size_t length, int prot, int flags,
    int fd, off64_t offset)
{
    /* check whether we should intercept this path */
    int intercept;
    scrmfs_intercept_fd(&fd, &intercept);
    if (intercept) {
        /* ERROR: fn not yet supported */
        fprintf(stderr, "Function not yet supported @ %s:%d\n", __FILE__, __LINE__);
        errno = EBADF;
        return MAP_FAILED;
    } else {
        MAP_OR_FAIL(mmap64);
        void* ret = __real_mmap64(addr, length, prot, flags, fd, offset);
        return ret;
    }
}

int SCRMFS_DECL(__fxstat)(int vers, int fd, struct stat *buf)
{
    /* check whether we should intercept this path */
    int intercept;
    scrmfs_intercept_fd(&fd, &intercept);
    if (intercept) {
        /* ERROR: fn not yet supported */
        fprintf(stderr, "Function not yet supported @ %s:%d\n", __FILE__, __LINE__);
        errno = EBADF;
        return -1;
    } else {
        MAP_OR_FAIL(__fxstat);
        int ret = __real___fxstat(vers, fd, buf);
        return ret;
    }
}

int SCRMFS_DECL(__fxstat64)(int vers, int fd, struct stat64 *buf)
{
    /* check whether we should intercept this path */
    int intercept;
    scrmfs_intercept_fd(&fd, &intercept);
    if (intercept) {
        /* ERROR: fn not yet supported */
        fprintf(stderr, "Function not yet supported @ %s:%d\n", __FILE__, __LINE__);
        errno = EBADF;
        return -1;
    } else {
        MAP_OR_FAIL(__fxstat64);
        int ret = __real___fxstat64(vers, fd, buf);
        return ret;
    }
}

int SCRMFS_DECL(close)(int fd)
{
    int intercept;
    scrmfs_intercept_fd(&fd, &intercept);
    if (intercept) {
        /* TODO: what to do if underlying file has been deleted? */

        /* TODO: check that fd is actually in use */
        int fid = scrmfs_get_fid_from_fd(fd);
        if (fid < 0) {
            errno = EBADF;
            return -1;
        }

        /* TODO: free file descriptor */

        debug("closing fd %d\n",fd);
        return 0;
    } else {
        MAP_OR_FAIL(close);
        int ret = __real_close(fd);
        return ret;
    }
}

/* ---------------------------------------
 * POSIX wrappers: file streams
 * --------------------------------------- */

FILE* SCRMFS_DECL(fopen)(const char *path, const char *mode)
{
    /* check whether we should intercept this path */
    if (scrmfs_intercept_path(path)) {
        /* ERROR: fn not yet supported */
        fprintf(stderr, "Function not yet supported @ %s:%d\n", __FILE__, __LINE__);
        errno = ENOENT;
        return NULL;
    } else {
        MAP_OR_FAIL(fopen);
        FILE* ret = __real_fopen(path, mode);
        return ret;
    }
}

FILE* SCRMFS_DECL(fopen64)(const char *path, const char *mode)
{
    /* check whether we should intercept this path */
    if (scrmfs_intercept_path(path)) {
        /* ERROR: fn not yet supported */
        fprintf(stderr, "Function not yet supported @ %s:%d\n", __FILE__, __LINE__);
        errno = ENOENT;
        return NULL;
    } else {
        MAP_OR_FAIL(fopen64);
        FILE* ret = __real_fopen64(path, mode);
        return ret;
    }
}

size_t SCRMFS_DECL(fread)(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    MAP_OR_FAIL(fread);
    size_t ret = __real_fread(ptr, size, nmemb, stream);
    return(ret);
}

size_t SCRMFS_DECL(fwrite)(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    MAP_OR_FAIL(fwrite);
    size_t ret = __real_fwrite(ptr, size, nmemb, stream);
    return ret;
}

int SCRMFS_DECL(fseek)(FILE *stream, long offset, int whence)
{
    MAP_OR_FAIL(fseek);
    int ret = __real_fseek(stream, offset, whence);
    return ret;
}

int SCRMFS_DECL(fclose)(FILE *fp)
{
    //int tmp_fd = fileno(fp);
    MAP_OR_FAIL(fclose);
    int ret = __real_fclose(fp);
    return ret;
}

/* ---------------------------------------
 * Library routines
 * --------------------------------------- */

void scrmfs_finalize(struct scrmfs_job_runtime* job)
{
    if(!job)
    {
        return;
    }

    free(job);
}

void scrmfs_initialize(int argc, char** argv,  int nprocs, int rank)
{
    int i;
    char* disable;
    char* disable_timing;
    char* envstr;
    char* truncate_string = "<TRUNCATED>";
    int truncate_offset;
    int chars_left = 0;
    int ret;
    int tmpval;

    disable = getenv("SCRMFS_DISABLE");
    if(disable)
    {
        /* turn off tracing */
        return;
    }

    disable_timing = getenv("SCRMFS_DISABLE_TIMING");

    if(scrmfs_global_job != NULL)
    {
        return;
    }

    /* allocate structure to track scrmfs_global_job information */
    scrmfs_global_job = malloc(sizeof(*scrmfs_global_job));
    if(!scrmfs_global_job)
    {
        return;
    }
    memset(scrmfs_global_job, 0, sizeof(*scrmfs_global_job));

    if(disable_timing)
    {
        scrmfs_global_job->flags |= CP_FLAG_NOTIMING;
    }

    /* set up file records */
    for(i=0; i<CP_MAX_FILES; i++)
    {
        scrmfs_global_job->file_runtime_array[i].log_file = 
            &scrmfs_global_job->file_array[i];
    }

    strcpy(scrmfs_global_job->log_job.version_string, CP_VERSION);
    scrmfs_global_job->log_job.magic_nr = CP_MAGIC_NR;
    scrmfs_global_job->log_job.uid = getuid();
    scrmfs_global_job->log_job.start_time = time(NULL);
    scrmfs_global_job->log_job.nprocs = nprocs;

    /* record exe and arguments */
    for(i=0; i<argc; i++)
    {
        chars_left = CP_EXE_LEN-strlen(scrmfs_global_job->exe);
        strncat(scrmfs_global_job->exe, argv[i], chars_left);
        if(i < (argc-1))
        {
            chars_left = CP_EXE_LEN-strlen(scrmfs_global_job->exe);
            strncat(scrmfs_global_job->exe, " ", chars_left);
        }
    }

    /* if we don't see any arguments, then use glibc symbol to get
     * program name at least (this happens in fortran)
     */
    if(argc == 0)
    {
        chars_left = CP_EXE_LEN-strlen(scrmfs_global_job->exe);
        strncat(scrmfs_global_job->exe, __progname_full, chars_left);
        chars_left = CP_EXE_LEN-strlen(scrmfs_global_job->exe);
        strncat(scrmfs_global_job->exe, " <unknown args>", chars_left);
    }

    if(chars_left == 0)
    {
        /* we ran out of room; mark that string was truncated */
        truncate_offset = CP_EXE_LEN - strlen(truncate_string);
        sprintf(&scrmfs_global_job->exe[truncate_offset], "%s", 
            truncate_string);
    }
}
