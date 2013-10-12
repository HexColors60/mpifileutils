#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h> /* asctime / localtime */

#include <pwd.h> /* for getpwent */
#include <grp.h> /* for getgrent */
#include <errno.h>
#include <string.h>

#include <libgen.h> /* dirname */

#include "libcircle.h"
#include "dtcmp.h"
#include "bayer.h"

// getpwent getgrent to read user and group entries

/* TODO: change globals to struct */
static int verbose   = 0;
static int walk_stat = 1;

typedef struct rm_elem {
  char* name;
  int depth;
  bayer_filetype type;
  int have_mode;
  mode_t mode;
  struct rm_elem* next;
} rm_elem_t;

static rm_elem_t* rm_head = NULL;
static rm_elem_t* rm_tail = NULL;

static void create_list(bayer_flist flist)
{
  /* step through and print data */
  uint64_t index = 0;
  uint64_t size = bayer_flist_size(flist);
  while (index < size) {
    /* allocate element for remove list */
    rm_elem_t* elem = (rm_elem_t*) BAYER_MALLOC(sizeof(rm_elem_t));

    /* get file name */
    const char* file = bayer_flist_file_get_name(flist, index);
    elem->name = BAYER_STRDUP(file);

    /* get file depth */
    elem->depth = bayer_flist_file_get_depth(flist, index);

    /* get file type */
    elem->type = bayer_flist_file_get_type(flist, index);
    if (elem->type != TYPE_DIR) {
      elem->type = TYPE_FILE;
    }

    /* get mode, if we have it */
    elem->have_mode = 0;
    if (bayer_flist_have_detail(flist)) {
      elem->have_mode = 1;
      elem->mode = (mode_t) bayer_flist_file_get_mode(flist, index);
    }

    /* attach element to remove list */
    if (rm_head == NULL) {
      rm_head = elem;
    }
    if (rm_tail != NULL) {
      rm_tail->next = elem;
    }
    rm_tail = elem;
    elem->next = NULL;

    /* go to next file */
    index++;
  }

  return;
}

static void free_list()
{
  rm_elem_t* elem = rm_head;
  while (elem != NULL) {
    rm_elem_t* next = elem->next;
    bayer_free(&elem->name);
    bayer_free(&elem);
    elem = next;
  }
  rm_head = NULL;
  rm_tail = NULL;
}

static int rm_depth;      /* tracks current level at which to remove items */
static uint64_t rm_count; /* tracks number of items local process has removed */

static void flist_split_levels(bayer_flist srclist, int* outlevels, int* outmin, bayer_flist** outlists)
{
  int i;

  if (outlevels == NULL || outmin == NULL || outlists == NULL) {
    return;
  }

  *outlevels = 0;
  *outmin    = -1;
  *outlists  = NULL;

  uint64_t total = bayer_flist_global_size(srclist);
  if (total == 0) {
    return;
  }

  int min = bayer_flist_min_depth(srclist);
  int max = bayer_flist_max_depth(srclist);
  int levels = max - min + 1;
  bayer_flist* lists = (bayer_flist*) BAYER_MALLOC(levels * sizeof(bayer_flist));

  for (i = 0; i < levels; i++) {
    bayer_flist_subset(srclist, &lists[i]);
  }

  uint64_t index = 0;
  uint64_t size = bayer_flist_size(srclist);
  while (index < size) {
    int depth = bayer_flist_file_get_depth(srclist, index);
    int depth_index = depth - min;
    bayer_flist dstlist = lists[depth_index];
    bayer_flist_file_copy(srclist, index, dstlist);
    index++;
  }

  for (i = 0; i < levels; i++) {
    bayer_flist_summarize(lists[i]);
  }

  *outlevels = levels;
  *outmin    = min;
  *outlists  = lists;

  return;
}

static void flist_free_levels(int levels, bayer_flist** outlists)
{
  if (outlists == NULL) {
    return;
  }

  int i;
  bayer_flist* lists = *outlists;
  for (i = 0; i < levels; i++) {
    bayer_flist_free(&lists[i]);
  }

  bayer_free(outlists);
  return;
}

static void remove_type(char type, const char* name)
{
  if (type == 'd') {
    int rc = bayer_rmdir(name);
    if (rc != 0) {
      printf("Failed to rmdir `%s' (errno=%d %s)\n", name, errno, strerror(errno));
    }
  } else if (type == 'f') {
    int rc = bayer_unlink(name);
    if (rc != 0) {
      printf("Failed to unlink `%s' (errno=%d %s)\n", name, errno, strerror(errno));
    }
  } else if (type == 'u') {
    int rc = remove(name);
    if (rc != 0) {
      printf("Failed to remove `%s' (errno=%d %s)\n", name, errno, strerror(errno));
    }
  } else {
    /* print error */
    printf("Unknown type=%c name=%s @ %s:%d\n", type, name, __FILE__, __LINE__);
  }

  return;
}

static void remove_process(CIRCLE_handle* handle)
{
  char path[CIRCLE_MAX_STRING_LEN];
  handle->dequeue(path);
  
  char item = path[0];
  char* name = &path[1];
  remove_type(item, name);
  rm_count++;

  return;
}

static void remove_create(CIRCLE_handle* handle)
{
  char path[CIRCLE_MAX_STRING_LEN];

  /* enqueues all items at rm_depth to be deleted */
  const rm_elem_t* elem = rm_head;
  while (elem != NULL) {
    if (elem->depth == rm_depth) {
      if (elem->type == TYPE_DIR) {
        path[0] = 'd';
      } else if (elem->type == TYPE_FILE || elem->type == TYPE_LINK) {
        path[0] = 'f';
      } else {
        path[0] = 'u';
      }
      size_t len = strlen(elem->name) + 2;
      if (len <= CIRCLE_MAX_STRING_LEN) {
        strcpy(&path[1], elem->name);
        handle->enqueue(path);
      } else {
        printf("Filename longer than %lu\n", (unsigned long)CIRCLE_MAX_STRING_LEN);
      }
    }
    elem = elem->next;
  }


  return;
}

/* insert all items to be removed into libcircle for
 * dynamic load balancing */
static void remove_libcircle()
{
  /* initialize libcircle */
  CIRCLE_init(0, NULL, CIRCLE_SPLIT_EQUAL | CIRCLE_CREATE_GLOBAL);

  /* set libcircle verbosity level */
  enum CIRCLE_loglevel loglevel = CIRCLE_LOG_WARN;
  if (verbose) {
//    loglevel = CIRCLE_LOG_INFO;
  }
  CIRCLE_enable_logging(loglevel);

  /* register callbacks */
  CIRCLE_cb_create(&remove_create);
  CIRCLE_cb_process(&remove_process);

  /* run the libcircle job */
  CIRCLE_begin();
  CIRCLE_finalize();

  return;
}

/* for given depth, just remove the files we know about */
static void remove_direct()
{
  /* each process directly removes its elements */
  const rm_elem_t* elem = rm_head;
  while (elem != NULL) {
      if (elem->depth == rm_depth) {
          if (elem->type == TYPE_DIR) {
            int rc = bayer_rmdir(elem->name);
            if (rc != 0) {
              printf("Failed to rmdir `%s' (errno=%d %s)\n", elem->name, errno, strerror(errno));
            }
          } else if (elem->type == TYPE_FILE || elem->type == TYPE_LINK) {
            int rc = bayer_unlink(elem->name);
            if (rc != 0) {
              printf("Failed to unlink `%s' (errno=%d %s)\n", elem->name, errno, strerror(errno));
            }
          } else {
            int rc = remove(elem->name);
            if (rc != 0) {
              printf("Failed to remove `%s' (errno=%d %s)\n", elem->name, errno, strerror(errno));
            }
          }
          rm_count++;
      }
      elem = elem->next;
  }
  return;
}

/* spread with synchronization */
  /* allreduce to get total count of items */

  /* sort by name */

  /* alltoall to determine which processes to send / recv from */

  /* alltoallv to exchange data */

  /* pt2pt with left and right neighbors to determine if they have the same dirname */

  /* delete what we can witout waiting */

  /* if my right neighbor has same dirname, send him msg when we're done */

  /* if my left neighbor has same dirname, wait for msg */


/* Bob Jenkins one-at-a-time hash: http://en.wikipedia.org/wiki/Jenkins_hash_function */
static uint32_t jenkins_one_at_a_time_hash(const char *key, size_t len)
{
  uint32_t hash, i;
  for(hash = i = 0; i < len; ++i) {
      hash += key[i];
      hash += (hash << 10);
      hash ^= (hash >> 6);
  }
  hash += (hash << 3);
  hash ^= (hash >> 11);
  hash += (hash << 15);
  return hash;
}

static int get_first_nonzero(const int* buf, int size)
{
  int i;
  for (i = 0; i < size; i++) {
    if (buf[i] != 0) {
      return i;
    }
  }
  return -1;
}

/* for given depth, evenly spread the files among processes for
 * improved load balancing */
static void remove_spread(bayer_flist flist, uint64_t* rmcount)
{
    uint64_t index;

    /* get our rank and number of ranks in job */
    int rank, ranks;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &ranks);
  
    /* allocate memory for alltoall exchanges */
    int* sendcounts = (int*) BAYER_MALLOC(ranks * sizeof(int));
    int* sendsizes  = (int*) BAYER_MALLOC(ranks * sizeof(int));
    int* senddisps  = (int*) BAYER_MALLOC(ranks * sizeof(int));
    int* recvsizes  = (int*) BAYER_MALLOC(ranks * sizeof(int));
    int* recvdisps  = (int*) BAYER_MALLOC(ranks * sizeof(int));

    /* get number of items */
    uint64_t my_count  = bayer_flist_size(flist);
    uint64_t all_count = bayer_flist_global_size(flist);
    uint64_t offset    = bayer_flist_global_offset(flist);

    /* for this implementation, we remove our portion of the list */
    *rmcount = my_count;

    /* compute number of bytes we'll send */
    size_t sendbytes = 0;
    for (index = 0; index < my_count; index++) {
        const char* name = bayer_flist_file_get_name(flist, index);
        size_t len = strlen(name) + 2;
        sendbytes += len;
    }

    /* compute the number of items that each rank should have */
    uint64_t low = all_count / (uint64_t)ranks;
    uint64_t extra = all_count - low * (uint64_t)ranks;

    /* compute number that we'll send to each rank and initialize sendsizes and offsets */
    int i;
    for (i = 0; i < ranks; i++) {
        /* compute starting element id and count for given rank */
        uint64_t start, num;
        if (i < extra) {
            num = low + 1;
            start = i * num;
        } else {
            num = low;
            start = (i - extra) * num + extra * (low + 1);
        }

        /* compute the number of items we'll send to this task */
        uint64_t sendcnt = 0;
        if (my_count > 0) {
            if (start <= offset && offset < start + num) {
                /* this rank overlaps our range,
                 * and its first element comes at or before our first element */
                sendcnt = num - (offset - start);
                if (my_count < sendcnt) {
                    /* the number the rank could receive from us
                     * is more than we have left */
                    sendcnt = my_count;
                }
            } else if (offset < start && start < offset + my_count) {
                /* this rank overlaps our range,
                 * and our first element comes strictly before its first element */
                sendcnt = my_count - (start - offset);
                if (num < sendcnt) {
                    /* the number the rank can receive from us
                     * is less than we have left */
                    sendcnt = num;
                }
            }
        }

        /* record the number of items we'll send to this task */
        sendcounts[i]  = (int) sendcnt;

        /* set sizes and displacements to 0, we'll fix this later */
        sendsizes[i] = 0;
        senddisps[i] = 0;
    }

    /* allocate space */
    char* sendbuf = (char*) BAYER_MALLOC(sendbytes);

    /* copy data into buffer */
    int dest = -1;
    int disp = 0;
    for (index = 0; index < my_count; index++) {
        /* get name and type of item */
        const char* name = bayer_flist_file_get_name(flist, index);
        bayer_filetype type = bayer_flist_file_get_type(flist, index);

        /* get rank that we're packing data for */
        if (dest == -1) {
          dest = get_first_nonzero(sendcounts, ranks);
          if (dest == -1) {
            /* error */
          }
          /* about to copy first item for this rank,
           * record its displacement */
          senddisps[dest] = disp;
        }

        /* identify region to be sent to rank */
        char* path = sendbuf + disp;

        /* first character encodes item type */
        if (type == TYPE_DIR) {
            path[0] = 'd';
        } else if (type == TYPE_FILE || type == TYPE_LINK) {
            path[0] = 'f';
        } else {
            path[0] = 'u';
        }

        /* now copy in the path */
        strcpy(&path[1], name);

        /* add bytes to sendsizes and increase displacement */
        size_t count = strlen(name) + 2;
        sendsizes[dest] += count;
        disp += count;

        /* decrement the count for this rank */
        sendcounts[dest]--;
        if (sendcounts[dest] == 0) {
          dest = -1;
        }
    }

    /* compute displacements */
    senddisps[0] = 0;
    for (i = 1; i < ranks; i++) {
        senddisps[i] = senddisps[i-1] + sendsizes[i-1];
    }

    /* alltoall to specify incoming counts */
    MPI_Alltoall(sendsizes, 1, MPI_INT, recvsizes, 1, MPI_INT, MPI_COMM_WORLD);

    /* compute size of recvbuf and displacements */
    size_t recvbytes = 0;
    recvdisps[0] = 0;
    for (i = 0; i < ranks; i++) {
        recvbytes += recvsizes[i];
        if (i > 0) {
            recvdisps[i] = recvdisps[i-1] + recvsizes[i-1];
        }
    }

    /* allocate recvbuf */
    char* recvbuf = (char*) BAYER_MALLOC(recvbytes);

    /* alltoallv to send data */
    MPI_Alltoallv(
        sendbuf, sendsizes, senddisps, MPI_CHAR,
        recvbuf, recvsizes, recvdisps, MPI_CHAR, MPI_COMM_WORLD
    );

    /* delete data */
    char* item = recvbuf;
    while (item < recvbuf + recvbytes) {
        /* get item name and type */
        char type = item[0];
        char* name = &item[1];

        /* delete item */
        remove_type(type, name);

        /* go to next item */
        size_t item_size = strlen(item) + 1;
        item += item_size;
    }

    /* free memory */
    bayer_free(&recvbuf);
    bayer_free(&recvdisps);
    bayer_free(&recvsizes);
    bayer_free(&sendbuf);
    bayer_free(&senddisps);
    bayer_free(&sendsizes);
    bayer_free(&sendcounts);

    return;
}

/* for given depth, hash directory name and map to processes to
 * test whether having all files in same directory on one process
 * matters */
static void remove_map()
{
    /* get our rank and number of ranks in job */
    int rank, ranks;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &ranks);
  
    int* sendsizes  = (int*) BAYER_MALLOC(ranks * sizeof(int));
    int* senddisps  = (int*) BAYER_MALLOC(ranks * sizeof(int));
    int* sendoffset = (int*) BAYER_MALLOC(ranks * sizeof(int));
    int* recvsizes  = (int*) BAYER_MALLOC(ranks * sizeof(int));
    int* recvdisps  = (int*) BAYER_MALLOC(ranks * sizeof(int));

    /* initialize sendsizes and offsets */
    int i;
    for (i = 0; i < ranks; i++) {
        sendsizes[i]  = 0;
        sendoffset[i] = 0;
    }

    /* compute number of bytes we'll send to each rank */
    size_t sendbytes = 0;
    const rm_elem_t* elem = rm_head;
    while (elem != NULL) {
        if (elem->depth == rm_depth) {
            /* identify a rank responsible for this item */
            char* dir = BAYER_STRDUP(elem->name);
            dirname(dir);
            size_t dir_len = strlen(dir);
            uint32_t hash = jenkins_one_at_a_time_hash(dir, dir_len);
            int rank = (int) (hash % (uint32_t)ranks);
            bayer_free(&dir);

            /* total number of bytes we'll send to each rank and the total overall */
            size_t count = strlen(elem->name) + 2;
            sendsizes[rank] += count;
            sendbytes += count;
        }
        elem = elem->next;
    }

    /* compute displacements */
    senddisps[0] = 0;
    for (i = 1; i < ranks; i++) {
        senddisps[i] = senddisps[i-1] + sendsizes[i-1];
    }

    /* allocate space */
    char* sendbuf = (char*) BAYER_MALLOC(sendbytes);

    /* TODO: cache results from above */
    /* copy data into buffer */
    elem = rm_head;
    while (elem != NULL) {
        if (elem->depth == rm_depth) {
            /* identify a rank responsible for this item */
            char* dir = BAYER_STRDUP(elem->name);
            dirname(dir);
            size_t dir_len = strlen(dir);
            uint32_t hash = jenkins_one_at_a_time_hash(dir, dir_len);
            int rank = (int) (hash % (uint32_t)ranks);
            bayer_free(&dir);

            /* identify region to be sent to rank */
            size_t count = strlen(elem->name) + 2;
            char* path = sendbuf + senddisps[rank] + sendoffset[rank];

            /* first character encodes item type */
            if (elem->type == TYPE_DIR) {
                path[0] = 'd';
            } else if (elem->type == TYPE_FILE || elem->type == TYPE_LINK) {
                path[0] = 'f';
            } else {
                path[0] = 'u';
            }

            /* now copy in the path */
            strcpy(&path[1], elem->name);

            /* bump up the offset for this rank */
            sendoffset[rank] += count;
        }
        elem = elem->next;
    }

    /* alltoall to specify incoming counts */
    MPI_Alltoall(sendsizes, 1, MPI_INT, recvsizes, 1, MPI_INT, MPI_COMM_WORLD);

    /* compute size of recvbuf and displacements */
    size_t recvbytes = 0;
    recvdisps[0] = 0;
    for (i = 0; i < ranks; i++) {
        recvbytes += recvsizes[i];
        if (i > 0) {
            recvdisps[i] = recvdisps[i-1] + recvsizes[i-1];
        }
    }

    /* allocate recvbuf */
    char* recvbuf =  (char*) BAYER_MALLOC(recvbytes);

    /* alltoallv to send data */
    MPI_Alltoallv(
        sendbuf, sendsizes, senddisps, MPI_CHAR,
        recvbuf, recvsizes, recvdisps, MPI_CHAR, MPI_COMM_WORLD
    );

    /* delete data */
    char* item = recvbuf;
    while (item < recvbuf + recvbytes) {
        /* get item name and type */
        char type = item[0];
        char* name = &item[1];

        /* delete item */
        remove_type(type, name);
        rm_count++;

        /* go to next item */
        size_t item_size = strlen(item) + 1;
        item += item_size;
    }

    /* free memory */
    bayer_free(&recvbuf);
    bayer_free(&recvdisps);
    bayer_free(&recvsizes);
    bayer_free(&sendbuf);
    bayer_free(&sendoffset);
    bayer_free(&senddisps);
    bayer_free(&sendsizes);

    return;
}

/* for each depth, sort files by filename and then remove, to test
 * whether it matters to limit the number of directories each process
 * has to reference (e.g., locking) */
static void remove_sort()
{
    /* get max filename length and count number of items at this depth */
    int max_len = 0;
    uint64_t my_count = 0;
    const rm_elem_t* elem = rm_head;
    while (elem != NULL) {
        if (elem->depth == rm_depth) {
            /* identify a rank responsible for this item */
            int len = (int) strlen(elem->name) + 1;
            if (len > max_len) {
                max_len = len;
            }
            my_count++;
        }
        elem = elem->next;
    }

    /* bail out if total count is 0 */
    int64_t all_count;
    MPI_Allreduce(&my_count, &all_count, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
    if (all_count == 0) {
        return;
    }

    /* compute max string size */
    int chars;
    MPI_Allreduce(&max_len, &chars, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);

    /* create key datatype (filename) and comparison op */
    MPI_Datatype dt_key;
    DTCMP_Op op_str;
    DTCMP_Str_create_ascend(chars, &dt_key, &op_str);

    /* create keysat datatype (filename + type) */
    MPI_Datatype types[2], dt_keysat;
    types[0] = dt_key;
    types[1] = MPI_CHAR;
    DTCMP_Type_create_series(2, types, &dt_keysat);

    /* allocate send buffer */
    int sendcount = (int) my_count;
    size_t sendbufsize = sendcount * (chars + 1);
    char* sendbuf = (char*) BAYER_MALLOC(sendbufsize);

    /* copy data into buffer */
    elem = rm_head;
    char* ptr = sendbuf;
    while (elem != NULL) {
        if (elem->depth == rm_depth) {
            /* encode the filename first */
            strcpy(ptr, elem->name);
            ptr += chars;

            /* last character encodes item type */
            if (elem->type == TYPE_DIR) {
                ptr[0] = 'd';
            } else if (elem->type == TYPE_FILE || elem->type == TYPE_LINK) {
                ptr[0] = 'f';
            } else {
                ptr[0] = 'u';
            }
            ptr++;
        }
        elem = elem->next;
    }

    /* sort items */
    void* recvbuf;
    int recvcount;
    DTCMP_Handle handle;
    DTCMP_Sortz(
        sendbuf, sendcount, &recvbuf, &recvcount,
        dt_key, dt_keysat, op_str, DTCMP_FLAG_NONE, MPI_COMM_WORLD, &handle
    );

    /* delete data */
    int delcount = 0;
    ptr = (char*)recvbuf;
    while (delcount < recvcount) {
        /* get item name */
        char* name = ptr;
        ptr += chars;

        /* get item type */
        char type = ptr[0];
        ptr++;

        /* delete item */
        remove_type(type, name);
        rm_count++;
        delcount++;
    }

    /* free output data */
    DTCMP_Free(&handle);

    /* free our send buffer */
    bayer_free(&sendbuf);

    /* free key comparison operation */
    DTCMP_Op_free(&op_str);

    /* free datatypes */
    MPI_Type_free(&dt_keysat);
    MPI_Type_free(&dt_key);

    return;
}

/* iterate through linked list of files and set ownership, timestamps, and permissions
 * starting from deepest level and working backwards */
static void remove_files(bayer_flist flist)
{
    int level;

    /* get our rank and number of ranks in job */
    int rank, ranks;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &ranks);

    /* split files into separate lists by directory depth */
    int levels, minlevel;
    bayer_flist* lists;
    flist_split_levels(flist, &levels, &minlevel, &lists);

    /* dive from shallow to deep, ensure all directories have write bit set */
    for (level = 0; level < levels; level++) {
        /* get list of items for this level */
        bayer_flist list = lists[level];

        /* determine whether we have details at this level */
        int detail = bayer_flist_have_detail(list);

        /* iterate over items and set write bit on directories if needed */
        uint64_t index;
        uint64_t size = bayer_flist_size(list);
        for (index = 0; index < size; index++) {
            /* check whether we have a directory */
            bayer_filetype type = bayer_flist_file_get_type(list, index);
            if (type == TYPE_DIR) {
                /* assume we have to set the bit */
                int set_write_bit = 1;
                if (detail) {
                    mode_t mode = (mode_t) bayer_flist_file_get_mode(list, index);
                    if (mode & S_IWUSR) {
                        /* we have the mode of the file, and the bit is already set */
                        set_write_bit = 0;
                    }
                }

                /* set the bit if needed */
                if (set_write_bit) {
                    const char* name = bayer_flist_file_get_name(list, index);
                    int rc = chmod(name, S_IRWXU);
                    if (rc != 0) {
                      printf("Failed to chmod directory `%s' (errno=%d %s)\n", name, errno, strerror(errno));
                    }
                }
            }
        }

        /* wait for all procs to finish before we start next level */
        MPI_Barrier(MPI_COMM_WORLD);
    }

    /* now remove files starting from deepest level */
    for (level = levels - 1; level >= 0; level--) {
        double start = MPI_Wtime();

        /* get list of items for this level */
        bayer_flist list = lists[level];

        uint64_t count = 0;
//        remove_direct();
        remove_spread(list, &count);
//        remove_map();
//      remove_sort();
//      TODO: remove spread then sort
//        remove_libcircle();
        
        /* wait for all procs to finish before we start
         * with files at next level */
        MPI_Barrier(MPI_COMM_WORLD);

        double end = MPI_Wtime();

        if (verbose) {
            uint64_t min, max, sum;
            MPI_Allreduce(&count, &min, 1, MPI_UINT64_T, MPI_MIN, MPI_COMM_WORLD);
            MPI_Allreduce(&count, &max, 1, MPI_UINT64_T, MPI_MAX, MPI_COMM_WORLD);
            MPI_Allreduce(&count, &sum, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
            double rate = 0.0;
            if (end - start > 0.0) {
              rate = (double)sum / (end - start);
            }
            double time = end - start;
            if (rank == 0) {
                printf("level=%d min=%lu max=%lu sum=%lu rate=%f secs=%f\n",
                  (minlevel + level), (unsigned long)min, (unsigned long)max, (unsigned long)sum, rate, time
                );
                fflush(stdout);
            }
        }
    }

    flist_free_levels(levels, &lists);

    return;
}

static void print_usage()
{
  printf("\n");
  printf("Usage: drm [options] <path>\n");
  printf("\n");
  printf("Options:\n");
  printf("  -i, --input <file>  - read list from file\n");
  printf("  -l, --lite          - walk file system without stat\n");
  printf("  -v, --verbose       - verbose output\n");
  printf("  -h, --help          - print usage\n");
  printf("\n");
  fflush(stdout);
  return;
}

int main(int argc, char **argv)
{
  /* initialize MPI */
  MPI_Init(&argc, &argv);

  /* get our rank and the size of comm_world */
  int rank, ranks;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &ranks);

  char* inputname = NULL;
  int walk = 0;

  int option_index = 0;
  static struct option long_options[] = {
    {"input",    1, 0, 'i'},
    {"lite",     0, 0, 'l'},
    {"help",     0, 0, 'h'},
    {"verbose",  0, 0, 'v'},
    {0, 0, 0, 0}
  };

  int usage = 0;
  while (1) {
    int c = getopt_long(
      argc, argv, "i:lhv",
      long_options, &option_index
    );

    if (c == -1) {
      break;
    }

    switch (c) {
    case 'i':
      inputname = BAYER_STRDUP(optarg);
      break;
    case 'l':
      walk_stat = 0;
      break;
    case 'h':
      usage = 1;
      break;
    case 'v':
      verbose = 1;
      break;
    case '?':
      usage = 1;
      break;
    default:
      if (rank == 0) {
        printf("?? getopt returned character code 0%o ??\n", c);
      }
    }
  }

  /* paths to walk come after the options */
  char* target = NULL;
  if (optind < argc) {
    /* got a path to walk */
    walk = 1;

    /* get absolute path and remove ".", "..", consecutive "/",
     * and trailing "/" characters */
    char* path = argv[optind];
    target = bayer_path_strdup_abs_reduce_str(path);

    /* currently only allow one path */
    if (argc - optind > 1) {
      usage = 1;
    }

    /* don't allow input file and walk */
    if (inputname != NULL) {
      usage = 1;
    }
  } else {
    /* if we're not walking, we must be reading,
     * and for that we need a file */
    if (inputname == NULL) {
      usage = 1;
    }
  }

  if (usage) {
    if (rank == 0) {
      print_usage();
    }
    MPI_Finalize();
    return 0;
  }

  /* TODO: check stat fields fit within MPI types */
  // if (sizeof(st_uid) > uint64_t) error(); etc...

  /* initialize our sorting library */
  DTCMP_Init();

  uint64_t all_count = 0;
  uint64_t walk_start, walk_end;

  /* create an empty file list */
  bayer_flist flist;

  if (walk) {
    time_t walk_start_t = time(NULL);
    if (walk_start_t == (time_t)-1) {
      /* TODO: ERROR! */
    }
    walk_start = (uint64_t) walk_start_t;

    /* report walk count, time, and rate */
    if (verbose && rank == 0) {
      char walk_s[30];
      size_t rc = strftime(walk_s, sizeof(walk_s)-1, "%FT%T", localtime(&walk_start_t));
      if (rc == 0) {
        walk_s[0] = '\0';
      }
      printf("%s: Walking directory: %s\n", walk_s, target);
      fflush(stdout);
    }

    /* walk file tree and record stat data for each file */
    double start_walk = MPI_Wtime();
    bayer_flist_walk_path(target, walk_stat, &flist);
    double end_walk = MPI_Wtime();

    time_t walk_end_t = time(NULL);
    if (walk_end_t == (time_t)-1) {
      /* TODO: ERROR! */
    }
    walk_end = (uint64_t) walk_end_t;

    /* get total file count */
    all_count = bayer_flist_global_size(flist);

    /* report walk count, time, and rate */
    if (verbose && rank == 0) {
      double time = end_walk - start_walk;
      double rate = 0.0;
      if (time > 0.0) {
        rate = ((double)all_count) / time;
      }
      printf("Walked %lu files in %f seconds (%f files/sec)\n",
        all_count, time, rate
      );
    }
  } else {
    /* read data from cache file */
    double start_read = MPI_Wtime();
    bayer_flist_read_cache(inputname, &flist);
    double end_read = MPI_Wtime();

    /* get total file count */
    all_count = bayer_flist_global_size(flist);

    /* report read count, time, and rate */
    if (verbose && rank == 0) {
      double time = end_read - start_read;
      double rate = 0.0;
      if (time > 0.0) {
        rate = ((double)all_count) / time;
      }
      printf("Read %lu files in %f seconds (%f files/sec)\n",
        all_count, time, rate
      );
    }
  }

  /* remove files */
  double start_remove = MPI_Wtime();
  remove_files(flist);
  double end_remove = MPI_Wtime();

  /* report read count, time, and rate */
  if (verbose && rank == 0) {
    double time = end_remove - start_remove;
    double rate = 0.0;
    if (time > 0.0) {
      rate = ((double)all_count) / time;
    }
    printf("Removed %lu files in %f seconds (%f files/sec)\n",
      all_count, time, rate
    );
  }

  bayer_flist_free(&flist);

  bayer_free(&inputname);

  /* shut down the sorting library */
  DTCMP_Finalize();

  /* free target directory */
  bayer_free(&target);

  /* shut down MPI */
  MPI_Finalize();

  return 0;
}