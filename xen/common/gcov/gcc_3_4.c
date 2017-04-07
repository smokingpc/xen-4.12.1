/*
 *  This code provides functions to handle gcc's profiling data format
 *  introduced with gcc 3.4. Future versions of gcc may change the gcov
 *  format (as happened before), so all format-specific information needs
 *  to be kept modular and easily exchangeable.
 *
 *  This file is based on gcc-internal definitions. Functions and data
 *  structures are defined to be compatible with gcc counterparts.
 *  For a better understanding, refer to gcc source: gcc/gcov-io.h.
 *
 *    Copyright IBM Corp. 2009
 *    Author(s): Peter Oberparleiter <oberpar@linux.vnet.ibm.com>
 *
 *    Uses gcc-internal data definitions.
 *
 *  Imported from Linux and modified for Xen by
 *    Wei Liu <wei.liu2@citrix.com>
 */


#include <xen/lib.h>

#include "gcov.h"

#if !(GCC_VERSION >= 30400 && GCC_VERSION < 40700)
#error "Wrong version of GCC used to compile gcov"
#endif

#define GCOV_COUNTERS 5

static struct gcov_info *gcov_info_head;

/**
 * struct gcov_fn_info - profiling meta data per function
 * @ident: object file-unique function identifier
 * @checksum: function checksum
 * @n_ctrs: number of values per counter type belonging to this function
 *
 * This data is generated by gcc during compilation and doesn't change
 * at run-time.
 */
struct gcov_fn_info
{
    unsigned int ident;
    unsigned int checksum;
    unsigned int n_ctrs[0];
};

/**
 * struct gcov_ctr_info - profiling data per counter type
 * @num: number of counter values for this type
 * @values: array of counter values for this type
 * @merge: merge function for counter values of this type (unused)
 *
 * This data is generated by gcc during compilation and doesn't change
 * at run-time with the exception of the values array.
 */
struct gcov_ctr_info
{
    unsigned int num;
    gcov_type *values;
    void (*merge)(gcov_type *, unsigned int);
};

/**
 * struct gcov_info - profiling data per object file
 * @version: gcov version magic indicating the gcc version used for compilation
 * @next: list head for a singly-linked list
 * @stamp: time stamp
 * @filename: name of the associated gcov data file
 * @n_functions: number of instrumented functions
 * @functions: function data
 * @ctr_mask: mask specifying which counter types are active
 * @counts: counter data per counter type
 *
 * This data is generated by gcc during compilation and doesn't change
 * at run-time with the exception of the next pointer.
 */
struct gcov_info
{
    unsigned int              version;
    struct gcov_info          *next;
    unsigned int              stamp;
    const char                *filename;
    unsigned int              n_functions;
    const struct gcov_fn_info *functions;
    unsigned int              ctr_mask;
    struct gcov_ctr_info      counts[0];
};

/**
 * struct type_info - iterator helper array
 * @ctr_type: counter type
 * @offset: index of the first value of the current function for this type
 *
 * This array is needed to convert the in-memory data format into the in-file
 * data format:
 *
 * In-memory:
 *   for each counter type
 *     for each function
 *       values
 *
 * In-file:
 *   for each function
 *     for each counter type
 *       values
 *
 * See gcc source gcc/gcov-io.h for more information on data organization.
 */
struct type_info {
    int ctr_type;
    unsigned int offset;
};

/**
 * struct gcov_iterator - specifies current file position in logical records
 * @info: associated profiling data
 * @record: record type
 * @function: function number
 * @type: counter type
 * @count: index into values array
 * @num_types: number of counter types
 * @type_info: helper array to get values-array offset for current function
 */
struct gcov_iterator {
    const struct gcov_info *info;

    int record;
    unsigned int function;
    unsigned int type;
    unsigned int count;

    int num_types;
    struct type_info type_info[GCOV_COUNTERS];
};

/* Mapping of logical record number to actual file content. */
#define RECORD_FILE_MAGIC       0
#define RECORD_GCOV_VERSION     1
#define RECORD_TIME_STAMP       2
#define RECORD_FUNCTION_TAG     3
#define RECORD_FUNCTON_TAG_LEN  4
#define RECORD_FUNCTION_IDENT   5
#define RECORD_FUNCTION_CHECK   6
#define RECORD_COUNT_TAG        7
#define RECORD_COUNT_LEN        8
#define RECORD_COUNT            9

static int counter_active(const struct gcov_info *info, unsigned int type)
{
    return (1 << type) & info->ctr_mask;
}

static unsigned int num_counter_active(const struct gcov_info *info)
{
    unsigned int i;
    unsigned int result = 0;

    for ( i = 0; i < GCOV_COUNTERS; i++ )
        if ( counter_active(info, i) )
            result++;

    return result;
}

void gcov_info_link(struct gcov_info *info)
{
    info->next = gcov_info_head;
    gcov_info_head = info;
}

struct gcov_info *gcov_info_next(const struct gcov_info *info)
{
    if ( !info )
        return gcov_info_head;

    return info->next;
}

const char *gcov_info_filename(const struct gcov_info *info)
{
    return info->filename;
}

void gcov_info_reset(struct gcov_info *info)
{
    unsigned int active = num_counter_active(info);
    unsigned int i;

    for ( i = 0; i < active; i++ )
        memset(info->counts[i].values, 0,
               info->counts[i].num * sizeof(gcov_type));
}

static size_t get_fn_size(const struct gcov_info *info)
{
    size_t size;

    size = sizeof(struct gcov_fn_info) + num_counter_active(info) *
        sizeof(unsigned int);
    if ( __alignof__(struct gcov_fn_info) > sizeof(unsigned int) )
        size = ROUNDUP(size, __alignof__(struct gcov_fn_info));
    return size;
}

static struct gcov_fn_info *get_fn_info(const struct gcov_info *info,
                                        unsigned int fn)
{
    return (struct gcov_fn_info *)
        ((char *) info->functions + fn * get_fn_size(info));
}

static struct gcov_fn_info *get_func(struct gcov_iterator *iter)
{
    return get_fn_info(iter->info, iter->function);
}

static struct type_info *get_type(struct gcov_iterator *iter)
{
    return &iter->type_info[iter->type];
}

/**
 * gcov_iter_next - advance file iterator to next logical record
 * @iter: file iterator
 *
 * Return zero if new position is valid, non-zero if iterator has reached end.
 */
static int gcov_iter_next(struct gcov_iterator *iter)
{
    switch ( iter->record )
    {
    case RECORD_FILE_MAGIC:
    case RECORD_GCOV_VERSION:
    case RECORD_FUNCTION_TAG:
    case RECORD_FUNCTON_TAG_LEN:
    case RECORD_FUNCTION_IDENT:
    case RECORD_COUNT_TAG:
        /* Advance to next record */
        iter->record++;
        break;
    case RECORD_COUNT:
        /* Advance to next count */
        iter->count++;
        /* fall through */
    case RECORD_COUNT_LEN:
        if ( iter->count < get_func(iter)->n_ctrs[iter->type] )
        {
            iter->record = 9;
            break;
        }
        /* Advance to next counter type */
        get_type(iter)->offset += iter->count;
        iter->count = 0;
        iter->type++;
        /* fall through */
    case RECORD_FUNCTION_CHECK:
        if ( iter->type < iter->num_types )
        {
            iter->record = 7;
            break;
        }
        /* Advance to next function */
        iter->type = 0;
        iter->function++;
        /* fall through */
    case RECORD_TIME_STAMP:
        if ( iter->function < iter->info->n_functions )
            iter->record = 3;
        else
            iter->record = -1;
        break;
    }
    /* Check for EOF. */
    if ( iter->record == -1 )
        return -EINVAL;
    else
        return 0;
}

/**
 * gcov_iter_write - write data to buffer
 * @iter: file iterator
 * @buf: buffer to write to, if it is NULL, nothing is written
 * @pos: position inside buffer to start writing
 *
 * Return number of bytes written into buffer.
 */
static size_t gcov_iter_write(struct gcov_iterator *iter, char *buf,
                              size_t pos)
{
    size_t ret = 0;

    switch ( iter->record )
    {
    case RECORD_FILE_MAGIC:
        ret = gcov_store_uint32(buf, pos, GCOV_DATA_MAGIC);
        break;
    case RECORD_GCOV_VERSION:
        ret = gcov_store_uint32(buf, pos, iter->info->version);
        break;
    case RECORD_TIME_STAMP:
        ret = gcov_store_uint32(buf, pos, iter->info->stamp);
        break;
    case RECORD_FUNCTION_TAG:
        ret = gcov_store_uint32(buf, pos, GCOV_TAG_FUNCTION);
        break;
    case RECORD_FUNCTON_TAG_LEN:
        ret = gcov_store_uint32(buf, pos, 2);
        break;
    case RECORD_FUNCTION_IDENT:
        ret = gcov_store_uint32(buf, pos, get_func(iter)->ident);
        break;
    case RECORD_FUNCTION_CHECK:
        ret = gcov_store_uint32(buf, pos, get_func(iter)->checksum);
        break;
    case RECORD_COUNT_TAG:
        ret = gcov_store_uint32(buf, pos,
                                GCOV_TAG_FOR_COUNTER(get_type(iter)->ctr_type));
        break;
    case RECORD_COUNT_LEN:
        ret = gcov_store_uint32(buf, pos,
                                get_func(iter)->n_ctrs[iter->type] * 2);
        break;
    case RECORD_COUNT:
        ret = gcov_store_uint64(buf, pos, iter->info->counts[iter->type].
                                values[iter->count + get_type(iter)->offset]);
        break;
    }

    return ret;
}

/* If buffer is NULL, no data is written. */
size_t gcov_info_to_gcda(char *buffer, const struct gcov_info *info)
{
    struct gcov_iterator iter = { .info = info };
    unsigned int i;
    size_t pos = 0;

    for ( i = 0; i < GCOV_COUNTERS; i++ )
    {
        if ( counter_active(info, i) )
        {
            iter.type_info[iter.num_types].ctr_type = i;
            iter.type_info[iter.num_types].offset = 0;
            iter.num_types++;
        }
    }

    do {
        pos += gcov_iter_write(&iter, buffer, pos);
    } while ( gcov_iter_next(&iter) == 0 );

    return pos;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */