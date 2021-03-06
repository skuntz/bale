// Copyright (c) 2019, Institute for Defense Analyses
// 4850 Mark Center Drive, Alexandria, VA 22311-1882; 703-845-2500
// This material may be reproduced by or for the U.S. Government 
// pursuant to the copyright license under the clauses at DFARS 
// 252.227-7013 and 252.227-7014.
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//   * Redistributions of source code must retain the above copyright
//     notice, this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright
//     notice, this list of conditions and the following disclaimer in the
//     documentation and/or other materials provided with the distribution.
//   * Neither the name of the copyright holder nor the
//     names of its contributors may be used to endorse or promote products
//     derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT HOLDER NOR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
// INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
// OF THE POSSIBILITY OF SUCH DAMAGE.


#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "convey_impl.h"
#include "private.h"


/*** Memory Allocation ***/

static void*
wrap_mpp_alloc(void* alc8r, size_t size, const char* tag, uint64_t value)
{
  return mpp_alloc(size);
}

static void*
wrap_mpp_alloc_align(void* alc8r, size_t size, const char* tag, uint64_t value)
{
  return mpp_alloc_align(size, CONVEY_MAX_ALIGN, CONVEY_MAX_ALIGN);
}

static void
wrap_mpp_free(void* alc8r, void* ptr)
{
  mpp_free(ptr);
}

const convey_alc8r_t convey_imp_alloc = {
  .alc8r = NULL,
  .grab = &wrap_mpp_alloc,
  .free = &wrap_mpp_free,
};

const convey_alc8r_t convey_imp_alloc_align = {
  .alc8r = NULL,
  .grab = &wrap_mpp_alloc_align,
  .free = &wrap_mpp_free,
};

bool
convey_prep_aligned(void** handle, size_t item_size, size_t header_size,
                    size_t align)
{
  // If the alignment wanted is zero or is not a power of two, or if it
  // fails to divide the item size, then ignore it.
  if (align == 0 || (align & (align - 1)) || item_size % align)
    align = item_size & ~(item_size - 1);
  if (align <= header_size || header_size == 0)
    return true;
  if (align >= CONVEY_MAX_ALIGN)
    align = CONVEY_MAX_ALIGN;
  int err = posix_memalign(handle, align, item_size);
  return (err == 0);
}


/*** Error Handling ***/

int
convey_imp_panic(convey_t* self, const char* where, int error)
{
  mprint(MY_PROC, 0, "%s: %s\n", where, convey_error_string(self, error));
  if (-error > 0 && -error < 64)
    self->suppress |= UINT64_C(1) << -error;
  return error;
}


/*** Statistics ***/

int64_t
convey_imp_statistic(int64_t stats[], int which)
{
  if (!stats || which < 0 || which >= convey_imp_N_STATS)
    return -INT64_C(1);
  int64_t answer = stats[which];
  if (which >= convey_CUMULATIVE)
    answer += stats[which - convey_CUMULATIVE];
  return answer;
}

void
convey_imp_update_stats(int64_t stats[])
{
  if (stats)
    for (int i = convey_BEGINS; i < convey_CUMULATIVE; i++) {
      stats[convey_CUMULATIVE + i] += stats[i];
      stats[i] = 0;
    }
}


/*** Processes Per Node ***/

size_t
convey_procs_per_node(void)
{
#if MPP_NO_MIMD
  return 1;
#else
  static size_t procs_per_node = 0;

# if HAVE_MPP_UTIL
  if (!mpp_comm_is_world(MPP_COMM_CURR))
    return 1;
# endif
  if (procs_per_node)
    return procs_per_node;

  long result = 0;
  char* string = getenv("CONVEY_NLOCAL");
  if (string)
    result = atol(string);
  result = mpp_max_long(result);
  if (result > 0 && PROCS % result == 0) {
    procs_per_node = result;
    return result;
  }

  result = 1;

# if HAVE_GETHOSTNAME
  char hostname[256];
  gethostname(hostname, 256);
  hostname[255] = 0;

  // Get a candidate for the number of processes per node
  static char roothost[256];
  memcpy(roothost, hostname, 256);
  mpp_broadcast64(roothost, 32, 0);
  bool equal = (strcmp(roothost, hostname) == 0);
  long guess = mpp_max_long(equal ? MY_PROC + 1 : 0);

  // Verify that it works
  if (guess > 1 && (PROCS % guess) == 0) {
    int root = MY_PROC - (MY_PROC % guess);
    memcpy(roothost, hostname, 256);

#  if MPP_USE_SHMEM
    mpp_barrier(1);
    if (MY_PROC != root)
      shmem_get64(roothost, roothost, 32, root);
#  elif HAVE_MPP_UTIL && MPP_USE_MPI
    mpp_comm_t team;
    mpp_comm_split(guess, &team);
    mpp_comm_push(team, true);
    mpp_broadcast64(roothost, 32, 0);
    mpp_comm_pop(true);
#  elif MPP_USE_MPI
    MPI_Comm team;
    MPI_Comm_split(MPI_COMM_WORLD, PROCS / guess, PROCS % guess, &team);
    MPI_Bcast(roothost, 32, MPI_UINT64_T, root, team);
    MPI_Comm_free(&team);
#  else
    static shared [32] uint64_t allnames[32 * THREADS];
    memcpy((void*) &allnames[32 * MYTHREAD], roothost, 256);
    upc_barrier(__LINE__);
    if (MYTHREAD != root) {
      uint64_t temp[32];
      for (int i = 0; i < 32; i++)
        temp[i] = allnames[32 * root + i];
      memcpy(roothost, temp, 256);
    }
#  endif

    equal = (strcmp(roothost, hostname) == 0);
    if (mpp_and_long(equal))
      result = guess;
  }
# endif

  procs_per_node = result;
  return result;
#endif  // !MPP_NO_MIMD
}
