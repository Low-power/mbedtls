/*
 *  Message Processing Stack, Reader implementation
 *
 *  Copyright The Mbed TLS Contributors
 *  SPDX-License-Identifier: Apache-2.0
 *
 *  Licensed under the Apache License, Version 2.0 (the "License"); you may
 *  not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  This file is part of Mbed TLS (https://tls.mbed.org)
 */

#include "mps_reader.h"
#include "mps_common.h"
#include "mps_trace.h"

#include <string.h>

#if ( defined(__ARMCC_VERSION) || defined(_MSC_VER) ) && \
    !defined(inline) && !defined(__cplusplus)
#define inline __inline
#endif

#if defined(MBEDTLS_MPS_ENABLE_TRACE)
static int mbedtls_mps_trace_id = MBEDTLS_MPS_TRACE_BIT_READER;
#endif /* MBEDTLS_MPS_ENABLE_TRACE */

/*
 * GENERAL NOTE ON CODING STYLE
 *
 * The following code intentionally separates memory loads
 * and stores from other operations (arithmetic or branches).
 * This leads to the introduction of many local variables
 * and significantly increases the C-code line count, but
 * should not increase the size of generated assembly.
 *
 * This reason for this is twofold:
 * (1) It will ease verification efforts using the VST
 *     whose program logic cannot directly reason
 *     about instructions containing a load or store in
 *     addition to other operations (e.g. *p = *q or
 *     tmp = *p + 42).
 * (2) Operating on local variables and writing the results
 *     back to the target contexts on success only
 *     allows to maintain structure invariants even
 *     on failure - this in turn has two benefits:
 *     (2.a) If for some reason an error code is not caught
 *           and operation continues, functions are nonetheless
 *           called with sane contexts, reducing the risk
 *           of dangerous behavior.
 *     (2.b) Randomized testing is easier if structures
 *           remain intact even in the face of failing
 *           and/or non-sensical calls.
 *     Moreover, it might even reduce code-size because
 *     the compiler need not write back temporary results
 *     to memory in case of failure.
 *
 */

static inline void mps_reader_zero( mbedtls_mps_reader *rd )
{
    /* A plain memset() would likely be more efficient,
     * but the current way of zeroing makes it harder
     * to overlook fields which should not be zero-initialized.
     * It's also more suitable for VF efforts since it
     * doesn't require reasoning about structs being
     * interpreted as unstructured binary blobs. */
    static mbedtls_mps_reader const zero =
        { .frag      = NULL,
          .frag_len  = 0,
          .commit    = 0,
          .end       = 0,
          .pending   = 0,
          .acc       = NULL,
          .acc_len   = 0,
          .acc_avail = 0,
          .acc_share  = { .acc_remaining = 0 }
        };
    *rd = zero;
}

int mbedtls_mps_reader_init( mbedtls_mps_reader *rd,
                             unsigned char *acc,
                             mbedtls_mps_size_t acc_len )
{
    MBEDTLS_MPS_TRACE_INIT( "reader_init, acc len %u", (unsigned) acc_len );
    mps_reader_zero( rd );
    rd->acc = acc;
    rd->acc_len = acc_len;
    MBEDTLS_MPS_TRACE_RETURN( 0 );
}

int mbedtls_mps_reader_free( mbedtls_mps_reader *rd )
{
    MBEDTLS_MPS_TRACE_INIT( "reader_free" );
    mps_reader_zero( rd );
    MBEDTLS_MPS_TRACE_RETURN( 0 );
}

int mbedtls_mps_reader_feed( mbedtls_mps_reader *rd,
                             unsigned char *new_frag,
                             mbedtls_mps_size_t new_frag_len )
{
    unsigned char *acc;
    mbedtls_mps_size_t copy_to_acc;
    MBEDTLS_MPS_TRACE_INIT( "reader_feed, frag %p, len %u",
                (void*) new_frag, (unsigned) new_frag_len );

    if( new_frag == NULL )
        MBEDTLS_MPS_TRACE_RETURN( MBEDTLS_ERR_MPS_READER_INVALID_ARG );

    MBEDTLS_MPS_STATE_VALIDATE_RAW( rd->frag == NULL,
        "mbedtls_mps_reader_feed() requires reader to be in producing mode" );

    acc = rd->acc;
    if( acc != NULL )
    {
        mbedtls_mps_size_t aa, ar;

        ar = rd->acc_share.acc_remaining;
        aa = rd->acc_avail;

        copy_to_acc = ar;
        if( copy_to_acc > new_frag_len )
            copy_to_acc = new_frag_len;

        acc += aa;

        if( copy_to_acc > 0 )
            memcpy( acc, new_frag, copy_to_acc );

        MBEDTLS_MPS_TRACE( mbedtls_mps_trace_comment,
                "Copy new data of size %u of %u into accumulator at offset %u",
                (unsigned) copy_to_acc, (unsigned) new_frag_len, (unsigned) aa );

        /* Check if, with the new fragment, we have enough data. */
        ar -= copy_to_acc;
        if( ar > 0 )
        {
            /* Need more data */
            aa += copy_to_acc;
            rd->acc_share.acc_remaining = ar;
            rd->acc_avail = aa;
            MBEDTLS_MPS_TRACE_RETURN( MBEDTLS_ERR_MPS_READER_NEED_MORE );
        }

        MBEDTLS_MPS_TRACE( mbedtls_mps_trace_comment,
                           "Enough data available to serve user request" );

        rd->acc_share.frag_offset = aa;
        aa += copy_to_acc;
        rd->acc_avail = aa;
    }
    else
    {
        rd->acc_share.frag_offset = 0;
    }

    rd->frag = new_frag;
    rd->frag_len = new_frag_len;
    rd->commit = 0;
    rd->end = 0;
    MBEDTLS_MPS_TRACE_RETURN( 0 );
}


int mbedtls_mps_reader_get( mbedtls_mps_reader *rd,
                            mbedtls_mps_size_t desired,
                            unsigned char **buffer,
                            mbedtls_mps_size_t *buflen )
{
    unsigned char *frag, *acc;
    mbedtls_mps_size_t end, fo, fl, frag_fetched, frag_remaining;
    MBEDTLS_MPS_TRACE_INIT( "reader_get %p, desired %u",
                            (void*) rd, (unsigned) desired );

    frag = rd->frag;
    MBEDTLS_MPS_STATE_VALIDATE_RAW( frag != NULL,
          "mbedtls_mps_reader_get() requires reader to be in consuming mode" );

    /* The fragment offset indicates the offset of the fragment
     * from the accmulator, if the latter is present. Use a offset
     * of \c 0 if no accumulator is used. */
    acc = rd->acc;
    if( acc == NULL )
        fo = 0;
    else
        fo = rd->acc_share.frag_offset;

    MBEDTLS_MPS_TRACE( mbedtls_mps_trace_comment,
            "frag_off %u, end %u, acc_avail %d",
            (unsigned) fo, (unsigned) rd->end,
            acc == NULL ? -1 : (int) rd->acc_avail );

    /* Check if we're still serving from the accumulator. */
    end = rd->end;
    if( end < fo )
    {
        MBEDTLS_MPS_TRACE( mbedtls_mps_trace_comment,
                           "Serve the request from the accumulator" );
        if( fo - end < desired )
        {
            /* Illustration of supported and unsupported cases:
             *
             * - Allowed #1
             *
             *                          +-----------------------------------+
             *                          |               frag                |
             *                          +-----------------------------------+
             *
             *             end end+desired
             *              |       |
             *        +-----v-------v-------------+
             *        |          acc              |
             *        +---------------------------+
             *                          |         |
             *                   fo/frag_offset aa/acc_avail
             *
             * - Allowed #2
             *
             *                          +-----------------------------------+
             *                          |               frag                |
             *                          +-----------------------------------+
             *
             *                  end          end+desired
             *                   |                |
             *        +----------v----------------v
             *        |          acc              |
             *        +---------------------------+
             *                          |         |
             *                   fo/frag_offset aa/acc_avail
             *
             * - Not allowed #1 (could be served, but we don't actually use it):
             *
             *                      +-----------------------------------+
             *                      |               frag                |
             *                      +-----------------------------------+
             *
             *              end        end+desired
             *               |             |
             *        +------v-------------v------+
             *        |          acc              |
             *        +---------------------------+
             *                      |              |
             *                fo/frag_offset   aa/acc_avail
             *
             *
             * - Not allowed #2 (can't be served with a contiguous buffer):
             *
             *                      +-----------------------------------+
             *                      |               frag                |
             *                      +-----------------------------------+
             *
             *              end                 end + desired
             *               |                        |
             *        +------v--------------------+   v
             *        |            acc            |
             *        +---------------------------+
             *                      |             |
             *                fo/frag_offset   aa/acc_avail
             *
             * In case of Allowed #1 and #2 we're switching to serve from
             * `frag` starting from the next call to mbedtls_mps_reader_get().
             */

            mbedtls_mps_size_t aa;
            aa = rd->acc_avail;
            if( aa - end != desired )
            {
                /* It might be possible to serve some of these situations by
                 * making additional space in the accumulator, removing those
                 * parts that have already been committed.
                 * On the other hand, this brings additional complexity and
                 * enlarges the code size, while there doesn't seem to be a use
                 * case where we don't attempt exactly the same `get` calls when
                 * resuming on a reader than what we tried before pausing it.
                 * If we believe we adhere to this restricted usage throughout
                 * the library, this check is a good opportunity to
                 * validate this. */
                MBEDTLS_MPS_TRACE_RETURN(
                    MBEDTLS_ERR_MPS_READER_INCONSISTENT_REQUESTS );
            }
        }

        acc += end;
        *buffer = acc;
        if( buflen != NULL )
            *buflen = desired;

        end += desired;
        rd->end = end;
        rd->pending = 0;

        MBEDTLS_MPS_TRACE_RETURN( 0 );
    }

    /* Attempt to serve the request from the current fragment */
    MBEDTLS_MPS_TRACE( mbedtls_mps_trace_comment,
                       "Serve the request from the current fragment." );

    fl = rd->frag_len;
    frag_fetched = end - fo; /* The amount of data from the current fragment
                              * that has already been passed to the user. */
    frag += frag_fetched;
    frag_remaining = fl - frag_fetched; /* Remaining data in fragment */

    /* Check if we can serve the read request from the fragment. */
    if( frag_remaining < desired )
    {
        MBEDTLS_MPS_TRACE( mbedtls_mps_trace_comment,
                           "There's not enough data in the current fragment "
                           "to serve the request." );
        /* There's not enough data in the current fragment,
         * so either just RETURN what we have or fail. */
        if( buflen == NULL )
        {
            if( frag_remaining > 0 )
            {
                rd->pending = desired - frag_remaining;
                MBEDTLS_MPS_TRACE( mbedtls_mps_trace_comment,
                       "Remember to collect %u bytes before re-opening",
                       (unsigned) rd->pending );
            }
            MBEDTLS_MPS_TRACE_RETURN( MBEDTLS_ERR_MPS_READER_OUT_OF_DATA );
        }

        desired = frag_remaining;
    }

    /* There's enough data in the current fragment to serve the
     * (potentially modified) read request. */
    *buffer = frag;
    if( buflen != NULL )
        *buflen = desired;

    end += desired;
    rd->end = end;
    rd->pending = 0;
    MBEDTLS_MPS_TRACE_RETURN( 0 );
}

int mbedtls_mps_reader_commit( mbedtls_mps_reader *rd )
{
    mbedtls_mps_size_t end;
    MBEDTLS_MPS_TRACE_INIT( "reader_commit" );
    MBEDTLS_MPS_STATE_VALIDATE_RAW( rd->frag != NULL,
       "mbedtls_mps_reader_commit() requires reader to be in consuming mode" );

    end = rd->end;
    rd->commit = end;

    MBEDTLS_MPS_TRACE_RETURN( 0 );
}

int mbedtls_mps_reader_reclaim( mbedtls_mps_reader *rd,
                                mbedtls_mps_size_t *paused )
{
    unsigned char *frag, *acc;
    mbedtls_mps_size_t pending, commit;
    mbedtls_mps_size_t al, fo, fl;
    MBEDTLS_MPS_TRACE_INIT( "mbedtls_mps_reader_reclaim" );

    if( paused != NULL )
        *paused = 0;

    frag = rd->frag;
    MBEDTLS_MPS_STATE_VALIDATE_RAW( frag != NULL,
           "mbedtls_mps_reader_reclaim() requires reader to be in consuming mode" );

    acc     = rd->acc;
    pending = rd->pending;
    commit  = rd->commit;
    fl      = rd->frag_len;

    if( acc == NULL )
        fo = 0;
    else
        fo = rd->acc_share.frag_offset;

    if( pending == 0 )
    {
        MBEDTLS_MPS_TRACE( mbedtls_mps_trace_comment,
                           "No unsatisfied read-request has been logged." );

        /* Check if there's data left to be consumed. */
        if( commit < fo || commit - fo < fl )
        {
            MBEDTLS_MPS_TRACE( mbedtls_mps_trace_comment,
                               "There is data left to be consumed." );
            rd->end = commit;
            MBEDTLS_MPS_TRACE_RETURN( MBEDTLS_ERR_MPS_READER_DATA_LEFT );
        }

        rd->acc_avail = 0;
        rd->acc_share.acc_remaining = 0;

        MBEDTLS_MPS_TRACE( mbedtls_mps_trace_comment,
                           "Fragment has been fully processed and committed." );
    }
    else
    {
        int overflow;

        mbedtls_mps_size_t acc_backup_offset;
        mbedtls_mps_size_t acc_backup_len;
        mbedtls_mps_size_t frag_backup_offset;
        mbedtls_mps_size_t frag_backup_len;

        mbedtls_mps_size_t backup_len;
        mbedtls_mps_size_t acc_len_needed;

        MBEDTLS_MPS_TRACE( mbedtls_mps_trace_comment,
               "There has been an unsatisfied read with %u bytes overhead.",
               (unsigned) pending );

        if( acc == NULL )
        {
            MBEDTLS_MPS_TRACE( mbedtls_mps_trace_comment,
                               "No accumulator present" );
            MBEDTLS_MPS_TRACE_RETURN(
                MBEDTLS_ERR_MPS_READER_NEED_ACCUMULATOR );
        }
        al = rd->acc_len;

        /* Check if the upper layer has already fetched
         * and committed the contents of the accumulator. */
        if( commit < fo )
        {
            /* No, accumulator is still being processed. */
            frag_backup_offset = 0;
            frag_backup_len = fl;
            acc_backup_offset = commit;
            acc_backup_len = fo - commit;
        }
        else
        {
            /* Yes, the accumulator is already processed. */
            frag_backup_offset = commit - fo;
            frag_backup_len = fl - frag_backup_offset;
            acc_backup_offset = 0;
            acc_backup_len = 0;
        }

        backup_len = acc_backup_len + frag_backup_len;
        acc_len_needed = backup_len + pending;

        overflow  = 0;
        overflow |= ( backup_len     < acc_backup_len );
        overflow |= ( acc_len_needed < backup_len );

        if( overflow || al < acc_len_needed )
        {
            /* Except for the different return code, we behave as if
             * there hadn't been a call to mbedtls_mps_reader_get()
             * since the last commit. */
            rd->end = commit;
            rd->pending = 0;
            MBEDTLS_MPS_TRACE( mbedtls_mps_trace_error,
                               "The accumulator is too small to handle the backup." );
            MBEDTLS_MPS_TRACE( mbedtls_mps_trace_error,
                               "* Size: %u", (unsigned) al );
            MBEDTLS_MPS_TRACE( mbedtls_mps_trace_error,
                               "* Needed: %u (%u + %u)",
                               (unsigned) acc_len_needed,
                               (unsigned) backup_len, (unsigned) pending );
            MBEDTLS_MPS_TRACE_RETURN(
                MBEDTLS_ERR_MPS_READER_ACCUMULATOR_TOO_SMALL );
        }

        MBEDTLS_MPS_TRACE( mbedtls_mps_trace_comment,
                         "Fragment backup: %u", (unsigned) frag_backup_len );
        MBEDTLS_MPS_TRACE( mbedtls_mps_trace_comment,
                         "Accumulator backup: %u", (unsigned) acc_backup_len );

        /* Move uncommitted parts from the accumulator to the front
         * of the accumulator. */
        memmove( acc, acc + acc_backup_offset, acc_backup_len );

        /* Copy uncmmitted parts of the current fragment to the
         * accumulator. */
        memcpy( acc + acc_backup_len,
                frag + frag_backup_offset, frag_backup_len );

        rd->acc_avail = backup_len;
        rd->acc_share.acc_remaining = pending;

        if( paused != NULL )
            *paused = 1;
    }

    rd->frag     = NULL;
    rd->frag_len = 0;

    rd->commit  = 0;
    rd->end     = 0;
    rd->pending = 0;

    MBEDTLS_MPS_TRACE( mbedtls_mps_trace_comment,
                       "Final state: aa %u, al %u, ar %u",
                       (unsigned) rd->acc_avail, (unsigned) rd->acc_len,
                       (unsigned) rd->acc_share.acc_remaining );
    MBEDTLS_MPS_TRACE_RETURN( 0 );
}
