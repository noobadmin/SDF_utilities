/*
 * sdfdiff - compare numerical differences between two SDF files
 * Copyright (C) 2016 SDF Development Team
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <sys/stat.h>
#include <math.h>
#include <time.h>
#include <float.h>
#include <limits.h>
#include "sdf.h"
#include "sdf_list_type.h"
#include "sdf_helper.h"
#include "stack_allocator.h"
#include "commit_info.h"

#ifdef PARALLEL
#include <mpi.h>
#endif

#define VERSION "0.1.0"

#define MIN(a,b) (((a) < (b)) ? (a) : (b))
#define ABS(a) (((a) < 0.0) ? -(a) : (a))

int metadata, debug, ignore_summary, ascii_header;
int exclude_variables, derived, extension_info, index_offset;
int just_id, verbose_metadata, special_format, scale_factor;
int format_rowindex, format_index, format_number;
int purge_duplicate, ignore_nblocks;
int64_t array_ndims, *array_starts, *array_ends, *array_strides;
char *format_float, *format_int, *format_space;
double relerr = 1.0e-15;
//static char *default_float = "%9.6fE%+2.2d1p";
static char *default_float = "%13.6E";
static char *default_int   = "%" PRIi64;
static char *default_space = "    ";
static char *default_indent = "  ";
static char indent[64];

struct id_list {
    char *id;
    struct id_list *next;
} *variable_ids, *variable_last_id;

int nrange;

struct range_type {
    int start, end;
} *range_list;

static char width_fmt[16];
#define SET_WIDTH_LEN(len) do { \
        snprintf(width_fmt, 16, "%%-%is", (len)); \
    } while(0)

#define SET_WIDTH(string) do { \
        int _l = strlen((string)); \
        SET_WIDTH_LEN(_l); \
    } while(0)

#define PRINTC(name,variable,fmt) do { \
        printf(indent, 1); \
        printf(width_fmt, (name)); \
        printf(" " fmt, (variable)); \
        printf("\n"); \
    } while(0)

#define PRINT(name,variable,fmt) do { \
        PRINTC(name,variable,fmt); \
    } while(0)

#define PRINTAR(name,array,fmt,len) do { \
        int _i; \
        if (!(array)) break; \
        printf(indent, 1); \
        printf(width_fmt, (name)); \
        printf(" (" fmt, (array)[0]); \
        for (_i = 1; _i < (len); _i++) printf("," fmt, (array)[_i]); \
        printf(")\n"); \
    } while(0)

#define PRINTDAR(name,array,fmt,len) do { \
        int _i; \
        if (!(array)) break; \
        printf(indent, 1); \
        printf(width_fmt, (name)); \
        printf(" (" fmt, sdf_datatype_c[(array)[0]]); \
        for (_i = 1; _i < (len); _i++) \
            printf("," fmt, sdf_datatype_c[(array)[_i]]); \
        printf(")\n"); \
    } while(0)


int close_files(sdf_file_t **handles);


void usage(int err)
{
    fprintf(stderr, "usage: sdfdiff [options] <sdf_filename1> <sdf_filename2>\n");
    fprintf(stderr, "\noptions:\n\
  -h --help            Show this usage message\n\
  -m --metadata        Show metadata blocks (not shown by default)\n\
  -j --just-id         Only show ID and number for metadata blocks\n\
  -l --less-verbose    Print metadata less verbosely\n\
  -r --relerr          Relative error for numerical difference\n\
  -v --variable=id     Find the block with id matching 'id'\n\
  -x --exclude=id      Exclude the block with id matching 'id'\n\
  -i --no-summary      Ignore the metadata summary\n\
  -b --no-nblocks      Ignore the header value for nblocks\n\
  -d --derived         Add derived blocks\n\
  -e --extension-info  Print information about any loaded extension module\n\
  -I --c-indexing      Array indexing starts from 1 by default. If this flag\n\
                       is used then the indexing starts from 0.\n\
  -H --no-ascii-header When writing multi-column ascii data, a header is\n\
                       included for use by gnuplot or other plotting\n\
                       utilities. This flag disables the header.\n\
  -F --format-float=f  Use specified format for printing floating-point array\n\
                       contents.\n\
  -N --format-int=f    Use specified format for printing integer array\n\
                       contents.\n\
  -S --format-space=f  Use specified spacing between array elements.\n\
  -K --format-number   Show block number before each row of array elements.\n\
  -R --format-rowindex Show array indices before each row of array elements.\n\
  -J --format-index    Show array indices before each array element.\n\
  -p --purge-duplicate Delete duplicated block IDs\n\
  -V --version         Print version information and exit\n\
");
/*
  -o --output          Output filename\n\
  -D --debug           Show the contents of the debug buffer\n\
*/

    exit(err);
}


int range_sort(const void *v1, const void *v2)
{
    struct range_type *a = (struct range_type *)v1;
    struct range_type *b = (struct range_type *)v2;

    return (a->start - b->start);
}


void parse_format(void)
{
    char cc;
    int len = strlen(format_float) - 1;

    scale_factor = 1;
    if (format_float[len] == 'p') {
        format_float[len] = '\0';
        for (; len > 1; len--) {
            cc = format_float[len-1];
            if (cc < '0' || cc > '9') break;
        }
        scale_factor = (int)strtol(&format_float[len], NULL, 10);
        format_float[len--] = '\0';
    }

    special_format = 0;
    if (format_float[len] == 'd')
        special_format = 1;
}


char **parse_args(int *argc, char ***argv)
{
    char *ptr, **files;
    int c, i, err, range, sz, nrange_max, got_include, got_exclude;
    struct range_type *range_tmp;
    struct stat statbuf;
    static struct option longopts[] = {
        { "no-nblocks",      no_argument,       NULL, 'b' },
        { "derived",         no_argument,       NULL, 'd' },
        { "extension-info",  no_argument,       NULL, 'e' },
        { "format-float",    required_argument, NULL, 'F' },
        { "help",            no_argument,       NULL, 'h' },
        { "no-ascii-header", no_argument,       NULL, 'H' },
        { "no-summary",      no_argument,       NULL, 'i' },
        { "c-indexing",      no_argument,       NULL, 'I' },
        { "just-id",         no_argument,       NULL, 'j' },
        { "format-index",    no_argument,       NULL, 'J' },
        { "format-number",   no_argument,       NULL, 'K' },
        { "less-verbose",    no_argument,       NULL, 'l' },
        { "metadata",        no_argument,       NULL, 'm' },
        { "format-int",      required_argument, NULL, 'N' },
        { "relerr",          required_argument, NULL, 'r' },
        { "format-rowindex", no_argument,       NULL, 'R' },
        { "format-space",    required_argument, NULL, 'S' },
        { "variable",        required_argument, NULL, 'v' },
        { "exclude",         required_argument, NULL, 'x' },
        { "purge-duplicate", no_argument,       NULL, 'p' },
        { "version",         no_argument,       NULL, 'V' },
        { NULL,              0,                 NULL,  0  }
        //{ "debug",           no_argument,       NULL, 'D' },
        //{ "output",          required_argument, NULL, 'o' },
    };

    debug = index_offset = verbose_metadata = 1;
    ascii_header = 1;
    metadata = ignore_summary = exclude_variables = 0;
    derived = format_rowindex = format_index = format_number = just_id = 0;
    purge_duplicate = ignore_nblocks = extension_info = 0;
    variable_ids = NULL;
    variable_last_id = NULL;
    array_starts = array_ends = array_strides = NULL;
    array_ndims = nrange_max = nrange = 0;
    sz = sizeof(struct range_type);

    format_int = malloc(strlen(default_int)+1);
    memcpy(format_int, default_int, strlen(default_int)+1);

    format_float = malloc(strlen(default_float)+1);
    memcpy(format_float, default_float, strlen(default_float)+1);

    format_space = malloc(strlen(default_space)+1);
    memcpy(format_space, default_space, strlen(default_space)+1);

    got_include = got_exclude = 0;

    while ((c = getopt_long(*argc, *argv,
            "bdeF:hHiIjJKlmN:r:RS:v:x:pV", longopts, NULL)) != -1) {
        switch (c) {
        case 'b':
            ignore_nblocks = 1;
            break;
        case 'd':
            derived = 1;
            break;
        case 'e':
            extension_info = 1;
            break;
        case 'F':
            free(format_float);
            format_float = malloc(strlen(optarg)+1);
            memcpy(format_float, optarg, strlen(optarg)+1);
            break;
        case 'h':
            usage(0);
            break;
        case 'H':
            ascii_header = 0;
            break;
        case 'i':
            ignore_summary = 1;
            break;
        case 'I':
            index_offset = 0;
            break;
        case 'j':
            just_id = 1;
            break;
        case 'J':
            format_index = 1;
            break;
        case 'K':
            format_number = 1;
            break;
        case 'l':
            verbose_metadata = 0;
            break;
        case 'm':
            metadata = 1;
            break;
        case 'N':
            free(format_int);
            format_int = malloc(strlen(optarg)+1);
            memcpy(format_int, optarg, strlen(optarg)+1);
            break;
        case 'r':
            relerr = atof(optarg);
            break;
        case 'R':
            format_rowindex = 1;
            break;
        case 'p':
            purge_duplicate = 1;
            break;
        case 'S':
            free(format_space);
            format_space = malloc(strlen(optarg)+1);
            memcpy(format_space, optarg, strlen(optarg)+1);
            break;
        case 'V':
            printf("sdfdiff version %s\n", VERSION);
            printf("commit info: %s, %s\n", SDF_COMMIT_ID, SDF_COMMIT_DATE);
            printf("library commit info: %s, %s\n",
                   sdf_get_library_commit_id(), sdf_get_library_commit_date());
            exit(0);
            break;
        case 'v':
        case 'x':
            err = 0;
            if (c == 'v') {
                if (got_exclude) err = 1;
                got_include = 1;
            } else {
                if (got_include) err = 1;
                got_exclude = 1;
                exclude_variables = 1;
            }
            if (err) {
                fprintf(stderr, "ERROR: cannot both include and "
                        "exclude variables.\n");
                exit(1);
            }
            if ((*optarg >= '0' && *optarg <= '9') || *optarg == '-') {
                ptr = optarg;
                range = 0;
                while (ptr < optarg + strlen(optarg) + 1) {
                    if (range) {
                        i = (int)strtol(ptr, &ptr, 10);
                        if (i == 0)
                            range_list[nrange-1].end = INT_MAX;
                        else if (i < range_list[nrange-1].start)
                            nrange--;
                        else
                            range_list[nrange-1].end = i;
                        range = 0;
                    } else {
                        nrange++;
                        // Grow array if necessary
                        if (nrange > nrange_max) {
                            if (nrange_max == 0) {
                                nrange_max = 128;
                                range_list = calloc(nrange_max, sz);
                            } else {
                                i = 2 * nrange_max;

                                range_tmp = calloc(i, sz);
                                memcpy(range_tmp, range_list, nrange_max * sz);
                                free(range_list);
                                range_list = range_tmp;

                                nrange_max = i;
                            }
                        }

                        if (*ptr == '-') {
                            range = 1;
                            range_list[nrange-1].end = INT_MAX;
                        } else {
                            i = (int)strtol(ptr, &ptr, 10);
                            range_list[nrange-1].start = i;
                            range_list[nrange-1].end = i;
                            if (*ptr == '-') range = 1;
                        }
                    }

                    ptr++;
                }
            } else {
                if (!variable_ids) {
                    variable_last_id =
                            variable_ids = malloc(sizeof(*variable_ids));
                } else {
                    variable_last_id->next = malloc(sizeof(*variable_ids));
                    variable_last_id = variable_last_id->next;
                }
                variable_last_id->next = NULL;
                variable_last_id->id = malloc(strlen(optarg)+1);
                memcpy(variable_last_id->id, optarg, strlen(optarg)+1);
            }
            break;
        default:
            usage(1);
        }
    }

    if ((optind+2) == *argc) {
        files = calloc(2, sizeof(*files));
        for (i=0; i<2; i++) {
            files[i] = (*argv)[optind+i];
            err = lstat(files[i], &statbuf);
            if (err) {
                fprintf(stderr, "Error opening file %s\n", files[i]);
                exit(1);
            }
        }
    } else {
        fprintf(stderr, "Must specify two files\n");
        usage(1);
    }

    if (nrange > 0) {
        // Sanitize range list
        qsort(range_list, nrange, sz, &range_sort);
        for (i=1; i < nrange; ) {
            if (range_list[i].start <= range_list[i-1].end+1) {
                if (range_list[i].end > range_list[i-1].end)
                    range_list[i-1].end = range_list[i].end;
                memcpy(range_list+i, range_list+i+1, (nrange-i) * sz);
                nrange--;
            } else
                i++;
        }

        // Shrink array
        range_tmp = malloc(nrange * sz);
        memcpy(range_tmp, range_list, nrange * sz);
        free(range_list);
        range_list = range_tmp;
    }

    parse_format();

    return files;
}


void free_memory(sdf_file_t **handles)
{

    if (format_int) free(format_int);
    if (format_float) free(format_float);
    if (format_space) free(format_space);
    sdf_stack_destroy(handles[0]);
    sdf_stack_destroy(handles[1]);
}


static void print_value(void *data, int datatype)
{
    int exponent;
    int64_t i64;
    double r8;

    switch (datatype) {
    case SDF_DATATYPE_INTEGER4:
        i64 = *((int32_t*)data);
        printf(format_int, i64);
        break;
    case SDF_DATATYPE_INTEGER8:
        printf(format_int, *((int64_t*)data));
        break;
    case SDF_DATATYPE_REAL4:
        if (special_format) {
            r8 = *((float*)data);
            if (r8 == 0)
                exponent = 0;
            else {
                exponent = (int)floor(log10(fabs(r8))+FLT_EPSILON) + 1;
                exponent -= scale_factor;
                r8 *= pow(10, -1.0 * exponent);
            }
            if (r8 == INFINITY)
                printf("Infinity");
            else
                printf(format_float, r8, exponent);
        } else
            printf(format_float, *((float*)data));
        break;
    case SDF_DATATYPE_REAL8:
        if (special_format) {
            r8 = *((double*)data);
            if (r8 == 0)
                exponent = 0;
            else {
                exponent = (int)floor(log10(fabs(r8))+FLT_EPSILON) + 1;
                exponent -= scale_factor;
                r8 *= pow(10, -1.0 * exponent);
            }
            if (r8 == INFINITY)
                printf("Infinity");
            else
                printf(format_float, r8, exponent);
        } else
            printf(format_float, *((double*)data));
        break;
    //case SDF_DATATYPE_REAL16:
    //    printf("%g", (double)b->const_value);
    //    break;
    case SDF_DATATYPE_CHARACTER:
        printf("%c", *((char*)data));
        break;
    case SDF_DATATYPE_LOGICAL:
        if (*((char*)data))
            printf("T");
        else
            printf("F");
        break;
    }
}


static void print_value_element(char *data, int datatype, int n)
{
    switch (datatype) {
    case SDF_DATATYPE_INTEGER4:
    case SDF_DATATYPE_REAL4:
        print_value(data + 4*n, datatype);
        break;
    case SDF_DATATYPE_INTEGER8:
    case SDF_DATATYPE_REAL8:
        print_value(data + 8*n, datatype);
        break;
    case SDF_DATATYPE_CHARACTER:
    case SDF_DATATYPE_LOGICAL:
        print_value(data + n, datatype);
        break;
    }
}


static void pretty_print_mesh(sdf_file_t *h, sdf_block_t *b, int idnum)
{
    int *idx, *fac;
    int i, n, rem, sz, left, digit, ncount, dim;
    char *ptr;
    static const int fmtlen = 32;
    char **fmt;

    idx = calloc(b->ndims, sizeof(*idx));
    fac = calloc(b->ndims, sizeof(*fac));
    fmt = calloc(b->ndims, sizeof(*fmt));

    rem = 1;
    for (i = 0; i < b->ndims; i++) {
        if (b->array_starts)
            left = b->array_ends[i] - b->array_starts[i];
        else
            left = b->local_dims[i];
        fac[i] = rem;
        rem *= left;
        digit = 0;
        if (b->array_ends)
            left = b->array_ends[i] + index_offset - 1;
        while (left) {
            left /= 10;
            digit++;
        }
        if (!digit) digit = 1;
        if (format_rowindex || format_index) {
            ptr = fmt[i] = malloc(fmtlen * sizeof(**fmt));
            if (i != 0) *ptr++ = ',';
            sz = snprintf(ptr, fmtlen-2, "%%%i.%ii", digit, digit);
            if (i == b->ndims-1) {
                ptr += sz;
                *ptr++ = ')';
                *ptr++ = '\0';
            }
        } else
            fmt[i] = calloc(1, sizeof(**fmt));
    }

    sz = SDF_TYPE_SIZES[b->datatype_out];

    ncount = 0;
    dim = 0;
    if (b->array_starts) {
        for (i = 0; i < b->ndims; i++)
            idx[i] = b->array_starts[i];
        for (i = 0; i < b->ndims; i++) {
            if (b->array_ends[i] > b->array_starts[i])
                break;
            dim++;
        }
    }
    ptr = b->grids[dim];

    for (n = 0; n < b->nelements_local; n++) {
        ncount++;
        if (ncount == 1) {
            if (format_number) printf("%i ", idnum);
        } else {
            if (format_index) printf(" ");
        }

        if ((ncount ==1 && format_rowindex) || format_index) {
            for (i = 0; i < b->ndims; i++) {
                if (i == dim) {
                    printf(fmt[i], idx[i]+index_offset);
                } else {
                    if (i != 0) printf(",");
                    printf("0");
                    if (i == b->ndims-1) printf(")");
                }
            }
        }

        if (ncount != 1 && format_index) printf(format_space,1);

        print_value(ptr, b->datatype_out);

        idx[dim]++;
        ptr += sz;
        if (b->array_ends && idx[dim] >= b->array_ends[dim]) {
            idx[dim] = 0;
            dim++;
            ptr = b->grids[dim];
            ncount = 1;
        } else if (idx[dim] >= b->local_dims[dim]) {
            idx[dim] = 0;
            dim++;
            ptr = b->grids[dim];
            ncount = 1;
        }
        if (ncount == 1) {
            printf("\n");
            ncount = 0;
        }
    }
    if (ncount) printf("\n");

    free(idx);
    free(fac);

    for (i = 0; i < b->ndims; i++) free(fmt[i]);
    free(fmt);
}


static void pretty_print(sdf_file_t *h, sdf_block_t *b, int idnum)
{
    int *idx, *fac;
    int i, n, rem, sz, left, digit, ncount, idx0;
    char *ptr;
    static const int fmtlen = 32;
    char **fmt;

    if (b->blocktype == SDF_BLOCKTYPE_PLAIN_MESH ||
            b->blocktype == SDF_BLOCKTYPE_POINT_MESH) {
        pretty_print_mesh(h, b, idnum);
        return;
    }

    idx = malloc(b->ndims * sizeof(*idx));
    fac = malloc(b->ndims * sizeof(*fac));
    fmt = malloc(b->ndims * sizeof(*fmt));

    rem = 1;
    for (i = 0; i < b->ndims; i++) {
        if (b->array_starts)
            left = b->array_ends[i] - b->array_starts[i];
        else
            left = b->local_dims[i];
        fac[i] = rem;
        rem *= left;
        digit = 0;
        if (b->array_ends)
            left = b->array_ends[i] + index_offset - 1;
        while (left) {
            left /= 10;
            digit++;
        }
        if (!digit) digit = 1;
        if (format_rowindex || format_index) {
            fmt[i] = malloc(fmtlen * sizeof(**fmt));
            if (i == 0)
                snprintf(fmt[i], fmtlen, "%%%i.%ii", digit, digit);
            else if (i == b->ndims-1)
                snprintf(fmt[i], fmtlen, ",%%%i.%ii)", digit, digit);
            else
                snprintf(fmt[i], fmtlen, ",%%%i.%ii", digit, digit);
        } else
            fmt[i] = calloc(1, sizeof(**fmt));
    }

    sz = SDF_TYPE_SIZES[b->datatype_out];

    ptr = b->data;
    ncount = 0;
    for (n = 0; n < b->nelements_local; n++) {
        rem = n;
        for (i = b->ndims-1; i >= 0; i--) {
            idx0 = idx[i] = rem / fac[i];
            if (b->array_starts) idx[i] += b->array_starts[i];
            rem -= idx0 * fac[i];
        }

        ncount++;
        if (ncount == 1) {
            if (format_number)
                printf("%i ", idnum);
            for (i = 0; i < b->ndims; i++)
                printf(fmt[i], idx[i]+index_offset);
        } else {
            if (format_index) {
                printf(" ");
                for (i = 0; i < b->ndims; i++)
                    printf(fmt[i], idx[i]+index_offset);
            }
            printf(format_space,1);
        }

        print_value(ptr, b->datatype_out);

        if (ncount == 1) {
            printf("\n");
            ncount = 0;
        }
        ptr += sz;
    }
    if (ncount) printf("\n");

    free(idx);
    free(fac);

    for (i = 0; i < b->ndims; i++) free(fmt[i]);
    free(fmt);
}


static void print_header(sdf_file_t *h)
{
    printf("Block 0: File header\n");
    if (just_id) return;

    sprintf(indent, default_indent, 1);

    SET_WIDTH("first_block_location:");
    PRINTC("endianness:", h->endianness, "%#8.8x");
    PRINTC("file_version:", h->file_version, "%i");
    PRINTC("file_revision:", h->file_revision, "%i");
    PRINTC("code_name:", h->code_name, "%s");
    PRINTC("first_block_location:", (long long)h->first_block_location, "%#8.8llx");
    PRINTC("summary_location:", (long long)h->summary_location, "%#8.8llx");
    PRINTC("summary_size:", h->summary_size, "%i");
    PRINTC("nblocks_file:", h->nblocks_file, "%i");
    PRINTC("block_header_length:", h->block_header_length, "%i");
    PRINTC("step:", h->step, "%i");
    PRINTC("time:", h->time, "%g");
    printf(indent, 1);
    printf(width_fmt, "jobid:");
    printf(" %i.%i\n", h->jobid1, h->jobid2);
    PRINTC("string_length:", h->string_length, "%i");
    PRINTC("code_io_version:", h->code_io_version, "%i");
    PRINTC("restart_flag:", h->restart_flag, "%i");
    PRINTC("other_domains:", h->other_domains, "%i");
    printf("\n");
}


static void print_metadata_plain_mesh(sdf_block_t *b)
{
    // Metadata is
    // - mults     REAL(r8), DIMENSION(ndims)
    // - labels    CHARACTER(id_length), DIMENSION(ndims)
    // - units     CHARACTER(id_length), DIMENSION(ndims)
    // - geometry  INTEGER(i4)
    // - minval    REAL(r8), DIMENSION(ndims)
    // - maxval    REAL(r8), DIMENSION(ndims)
    // - dims      INTEGER(i4), DIMENSION(ndims)

    SET_WIDTH("dim_labels:");
    if (verbose_metadata)
        PRINTAR("dim_mults:", b->dim_mults, "%g", b->ndims);
    PRINTAR("dim_labels:", b->dim_labels, "%s", b->ndims);
    PRINTAR("dim_units:", b->dim_units, "%s", b->ndims);
    PRINT("geometry:", sdf_geometry_c[b->geometry], "%s");
    PRINTAR("extents:", b->extents, "%g", 2*b->ndims);
    PRINTAR("dims:", b->dims, "%" PRIi64, b->ndims);
}


static void print_metadata_point_mesh(sdf_block_t *b)
{
    // Metadata is
    // - mults     REAL(r8), DIMENSION(ndims)
    // - labels    CHARACTER(id_length), DIMENSION(ndims)
    // - units     CHARACTER(id_length), DIMENSION(ndims)
    // - geometry  INTEGER(i4)
    // - minval    REAL(r8), DIMENSION(ndims)
    // - maxval    REAL(r8), DIMENSION(ndims)
    // - npoints   INTEGER(i8)
    // - speciesid CHARACTER(id_length)

    SET_WIDTH("dim_labels:");
    if (verbose_metadata)
        PRINTAR("dim_mults", b->dim_mults, "%g", b->ndims);
    PRINTAR("dim_labels", b->dim_labels, "%s", b->ndims);
    PRINTAR("dim_units", b->dim_units, "%s", b->ndims);
    PRINT("geometry:", sdf_geometry_c[b->geometry], "%s");
    PRINTAR("extents", b->extents, "%g", 2*b->ndims);
    //PRINTAR("dims", b->dims, "%" PRIi64, b->ndims);
    PRINT("nelements:", b->nelements, "%" PRIi64);
    if (b->material_id)
        PRINT("species id:", b->material_id, "%s");
}


static void print_metadata_plain_variable(sdf_block_t *b)
{
    // Metadata is
    // - mult      REAL(r8)
    // - units     CHARACTER(id_length)
    // - meshid    CHARACTER(id_length)
    // - dims      INTEGER(i4), DIMENSION(ndims)
    // - stagger   INTEGER(i4)

    SET_WIDTH("mesh id:");
    PRINT("units:", b->units, "%s");
    PRINT("mesh id:", b->mesh_id, "%s");
    PRINTAR("dims:", b->dims, "%" PRIi64, b->ndims);
    if (verbose_metadata) {
        PRINT("mult:", b->mult, "%g");
        PRINT("stagger:", sdf_stagger_c[b->stagger], "%s");
    }
}


static void print_metadata_point_variable(sdf_block_t *b)
{
    // Metadata is
    // - mult      REAL(r8)
    // - units     CHARACTER(id_length)
    // - meshid    CHARACTER(id_length)
    // - npoints   INTEGER(i8)
    // - speciesid CHARACTER(id_length)

    SET_WIDTH("species id:");
    PRINT("units:", b->units, "%s");
    PRINT("mesh id:", b->mesh_id, "%s");
    PRINT("nelements:", b->nelements, "%" PRIi64);
    if (b->material_id)
        PRINT("species id:", b->material_id, "%s");
    if (verbose_metadata)
        PRINT("mult:", b->mult, "%g");
}


static void print_metadata_constant(sdf_block_t *b)
{
    int32_t i4;
    int64_t i8;
    float r4;
    double r8;

    // Metadata is
    // - value     TYPE_SIZE

    printf("%svalue: ", indent);

    switch (b->datatype) {
    case SDF_DATATYPE_INTEGER4:
        memcpy(&i4, b->const_value, sizeof(i4));
        printf("%i", i4);
        break;
    case SDF_DATATYPE_INTEGER8:
        memcpy(&i8, b->const_value, sizeof(i8));
        printf("%" PRIi64, i8);
        break;
    case SDF_DATATYPE_REAL4:
        memcpy(&r4, b->const_value, sizeof(r4));
        printf("%g", r4);
        break;
    case SDF_DATATYPE_REAL8:
        memcpy(&r8, b->const_value, sizeof(r8));
        printf("%g", r8);
        break;
    //case SDF_DATATYPE_REAL16:
    //    printf("%g", (double)b->const_value);
    //    break;
    case SDF_DATATYPE_CHARACTER:
        printf("%c", *b->const_value);
        break;
    case SDF_DATATYPE_LOGICAL:
        if (*b->const_value)
            printf("True");
        else
            printf("False");
        break;
    }

    printf("\n");
}


static void print_metadata_array(sdf_block_t *b)
{
    // Metadata is
    // - dims      INTEGER(i4), DIMENSION(ndims)

    SET_WIDTH("dims:");
    PRINTAR("dims:", b->dims, "%" PRIi64, b->ndims);
}


static void print_metadata_cpu_split(sdf_block_t *b)
{
    // Metadata is
    // - dims      INTEGER(i4), DIMENSION(ndims)

    SET_WIDTH("geometry:");
    PRINT("geometry:", sdf_geometry_c[b->geometry], "%s");
    PRINTAR("dims:", b->dims, "%" PRIi64, b->ndims);
}


static void print_metadata_run(sdf_block_t *b)
{
    struct run_info *run = b->data;
    time_t time;
    char *stime;
    char version[32];

    // Metadata is
    // - version   INTEGER(i4)
    // - revision  INTEGER(i4)
    // - commit_id CHARACTER(string_length)
    // - sha1sum   CHARACTER(string_length)
    // - compmac   CHARACTER(string_length)
    // - compflag  CHARACTER(string_length)
    // - defines   INTEGER(i8)
    // - compdate  INTEGER(i4)
    // - rundate   INTEGER(i4)
    // - iodate    INTEGER(i4)
    // - minor_rev INTEGER(i4)

    SET_WIDTH("compile_machine:");
    snprintf(version, 32, "%i.%i.%i",
             run->version, run->revision, run->minor_rev);
    stime = version;
    PRINT("version:", stime, "%s");
    PRINT("commit id:", run->commit_id, "%s");
    PRINT("sha1sum:", run->sha1sum, "%s");
    PRINT("compile_machine:", run->compile_machine, "%s");
    PRINT("compile_flags:", run->compile_flags, "%s");
    PRINT("defines:", run->defines, "%" PRIi64);
    time = run->compile_date; stime = ctime(&time); stime[strlen(stime)-1]='\0';
    PRINT("compile_date:", stime, "%s");
    time = run->run_date; stime = ctime(&time); stime[strlen(stime)-1] = '\0';
    PRINT("run_date:", stime, "%s");
    time = run->io_date; stime = ctime(&time); stime[strlen(stime)-1] = '\0';
    PRINT("io_date:", stime, "%s");
}


static void print_metadata_stitched(sdf_block_t *b)
{
    // Metadata is
    // - stagger   INTEGER(i4)
    // - meshid    CHARACTER(id_length)
    // - varids    ndims*CHARACTER(id_length)

    SET_WIDTH("variable ids:");
    if (verbose_metadata)
        PRINT("stagger:", sdf_stagger_c[b->stagger], "%s");
    PRINT("mesh id:", b->mesh_id, "%s");
    PRINTAR("variable ids:", b->variable_ids, "%s", b->ndims);
}


static void print_metadata_stitched_material(sdf_block_t *b)
{
    // Metadata is
    // - stagger   INTEGER(i4)
    // - meshid    CHARACTER(id_length)
    // - matnames  ndims*CHARACTER(string_length)
    // - varids    ndims*CHARACTER(id_length)

    SET_WIDTH("material names:");
    if (verbose_metadata)
        PRINT("stagger:", sdf_stagger_c[b->stagger], "%s");
    PRINT("mesh id:", b->mesh_id, "%s");
    PRINTAR("material names:", b->material_names, "%s", b->ndims);
    PRINTAR("variable ids:", b->variable_ids, "%s", b->ndims);
}


static void print_metadata_stitched_matvar(sdf_block_t *b)
{
    // Metadata is
    // - stagger   INTEGER(i4)
    // - meshid    CHARACTER(id_length)
    // - matid     CHARACTER(id_length)
    // - varids    ndims*CHARACTER(id_length)

    SET_WIDTH("variable ids:");
    if (verbose_metadata)
        PRINT("stagger:", sdf_stagger_c[b->stagger], "%s");
    PRINT("mesh id:", b->mesh_id, "%s");
    PRINT("material id:", b->material_id, "%s");
    PRINTAR("variable ids:", b->variable_ids, "%s", b->ndims);
}


static void print_metadata_stitched_species(sdf_block_t *b)
{
    // Metadata is
    // - stagger   INTEGER(i4)
    // - meshid    CHARACTER(id_length)
    // - matid     CHARACTER(id_length)
    // - matname   CHARACTER(string_length)
    // - specnames ndims*CHARACTER(string_length)
    // - varids    ndims*CHARACTER(id_length)

    SET_WIDTH("species names:");
    if (verbose_metadata)
        PRINT("stagger:", sdf_stagger_c[b->stagger], "%s");
    PRINT("mesh id:", b->mesh_id, "%s");
    PRINT("material id:", b->material_id, "%s");
    PRINT("material name:", b->material_name, "%s");
    PRINTAR("species names:", b->material_names, "%s", b->ndims);
    PRINTAR("variable ids:", b->variable_ids, "%s", b->ndims);
}


static void print_metadata_stitched_obstacle_group(sdf_block_t *b)
{
    // Metadata is
    // - stagger         INTEGER(i4)
    // - obstacle_id     CHARACTER(id_length)
    // - vfm_id          CHARACTER(id_length)
    // - obstacle_names  ndims*CHARACTER(string_length)

    SET_WIDTH("volume fraction id:");
    if (verbose_metadata)
        PRINT("stagger:", sdf_stagger_c[b->stagger], "%s");
    PRINT("obstacle id:", b->obstacle_id, "%s");
    PRINT("volume fraction id:", b->vfm_id, "%s");
    PRINTAR("obstacle names:", b->material_names, "%s", b->ndims);
}


static void print_metadata_station(sdf_block_t *b)
{
    // Metadata is
    // - nelements INTEGER(i8)
    // - entry_len INTEGER(i4)
    // - nstations INTEGER(i4)
    // - nvars     INTEGER(i4)
    // - step0     INTEGER(i4)
    // - step_inc  INTEGER(i4)
    // - time0     REAL(r8)
    // - time_inc  REAL(r8)
    // - use_mult  CHARACTER(1)
    // - padding   CHARACTER(3)
    // - statids   CHARACTER(id_length), DIMENSION(nstations)
    // - statnames CHARACTER(string_length), DIMENSION(nstations)
    // - statnvars INTEGER(i4), DIMENSION(nstations)
    // - statmove  INTEGER(i4), DIMENSION(nstations)
    // - statx0    REAL(r8), DIMENSION(nstations*ndims)
    // - varids    CHARACTER(id_length), DIMENSION(nvars)
    // - varnames  CHARACTER(string_length), DIMENSION(nvars)
    // - vartypes  INTEGER(i4), DIMENSION(nvars)
    // - varunits  CHARACTER(id_length), DIMENSION(nvars)
    // - varmults  REAL(r8), DIMENSION(use_mult*nvars)

    SET_WIDTH("time_increment:");
    PRINT("nelements:", b->nelements, "%" PRIi64);
    PRINT("entry_len:", b->type_size, "%i");
    PRINT("nstations:", b->nstations, "%i");
    PRINT("nvariables:", b->nvariables, "%i");
    PRINT("step0:", b->step, "%i");
    PRINT("step_increment:", b->step_increment, "%i");
    PRINT("time0:", b->time, "%g");
    PRINT("time_increment:", b->time_increment, "%g");
    PRINTAR("station_ids:", b->station_ids, "%s", b->nstations);
    PRINTAR("station_names:", b->station_names, "%s", b->nstations);
    PRINTAR("station_nvars:", b->station_nvars, "%i", b->nstations);
    PRINTAR("station_move:", b->station_move, "%i", b->nstations);
    PRINTAR("station_x:", b->station_x, "%g", b->nstations);
    if (b->ndims > 1)
        PRINTAR("station_y:", b->station_y, "%g", b->nstations);
    if (b->ndims > 2)
        PRINTAR("station_z:", b->station_z, "%g", b->nstations);
    PRINTAR("variable_ids:", b->variable_ids, "%s", b->nvariables);
    PRINTAR("variable_names:", b->material_names, "%s", b->nvariables);
    PRINTDAR("variable_types:", b->variable_types, "%s", b->nvariables);
    PRINTAR("variable_units:", b->dim_units, "%s", b->nvariables);
    if (b->dim_mults && verbose_metadata)
        PRINTAR("variable_mults:", b->dim_mults, "%g", b->nvariables);
}


static void print_metadata_datablock(sdf_block_t *b)
{
    // Metadata is
    // - mimetype       CHARACTER(id_length)
    // - checksum_type  CHARACTER(id_length)
    // - checksum       CHARACTER(string_length)

    SET_WIDTH("checksum_type:");
    PRINT("mimetype:", b->mimetype, "%s");
    PRINT("checksum_type:", b->checksum_type, "%s");
    PRINT("checksum:", b->checksum, "%s");
    PRINTAR("species names:", b->material_names, "%s", b->ndims);
}


static void print_metadata_namevalue(sdf_block_t *b)
{
    int i, len, max;
    int32_t *i4;
    int64_t *i8;
    float *r4;
    double *r8;
    char *logical;
    char **string;

    // Metadata is
    // - names     ndims*CHARACTER(string_length)
    // - values    ndims*DATATYPE

    max = 0;
    for (i = 0; i < b->ndims; i++) {
        len = strlen(b->material_names[i]);
        if (len > max) max = len;
    }

    SET_WIDTH_LEN(max);
    switch (b->datatype) {
    case(SDF_DATATYPE_INTEGER4):
        i4 = b->data;
        for (i = 0; i < b->ndims; i++) {
            PRINT(b->material_names[i], i4[i], "%i");
        }
        break;
    case(SDF_DATATYPE_INTEGER8):
        i8 = b->data;
        for (i = 0; i < b->ndims; i++) {
            PRINT(b->material_names[i], i8[i], "%" PRIi64);
        }
        break;
    case(SDF_DATATYPE_REAL4):
        r4 = b->data;
        for (i = 0; i < b->ndims; i++) {
            PRINT(b->material_names[i], r4[i], "%g");
        }
        break;
    case(SDF_DATATYPE_REAL8):
        r8 = b->data;
        for (i = 0; i < b->ndims; i++) {
            PRINT(b->material_names[i], r8[i], "%g");
        }
        break;
    case(SDF_DATATYPE_LOGICAL):
        logical = b->data;
        for (i = 0; i < b->ndims; i++) {
            printf(indent, 1);
            printf(width_fmt, b->material_names[i]);
            if (logical[i])
                printf("True");
            else
                printf("False");
            printf("\n");
        }
        break;
    case(SDF_DATATYPE_CHARACTER):
        string = b->data;
        for (i = 0; i < b->ndims; i++) {
            PRINT(b->material_names[i], string[i], "%s");
        }
        break;
    }
}


static void print_metadata(sdf_block_t *b, int inum, int nblocks)
{
    int digit = 0;
    static const int fmtlen = 64;
    char fmt[fmtlen];

    while (nblocks) {
        nblocks /= 10;
        digit++;
    }

    snprintf(fmt, fmtlen, "Block %%%ii, ID: %%s not found in second file",
             digit);
    printf(fmt, inum, b->id);
    if (!b->in_file)
        printf("  (derived)");
    printf("\n");
    if (just_id) return;

    sprintf(indent, default_indent, 1);

    if (verbose_metadata)
        SET_WIDTH("block_location:");
    else
        SET_WIDTH("blocktype:");

    PRINT("name:", b->name, "%s");
    PRINT("blocktype:", sdf_blocktype_c[b->blocktype], "%s");
    PRINT("datatype:", sdf_datatype_c[b->datatype], "%s");

    if (verbose_metadata) {
        PRINT("ndims:", b->ndims, "%i");
        PRINT("data_length:", b->data_length, "%" PRIi64);
        PRINT("info_length:", b->info_length, "%i");
        PRINT("data_location:", b->data_location, "%" PRIi64);
        PRINT("block_location:", b->block_start, "%" PRIi64);
        PRINT("next_block:", b->next_block_location, "%" PRIi64);
    }

    sprintf(indent+strlen(indent), default_indent, 1);

    switch (b->blocktype) {
    case SDF_BLOCKTYPE_PLAIN_MESH:
    case SDF_BLOCKTYPE_LAGRANGIAN_MESH:
        print_metadata_plain_mesh(b);
        break;
    case SDF_BLOCKTYPE_POINT_MESH:
        print_metadata_point_mesh(b);
        break;
    case SDF_BLOCKTYPE_PLAIN_VARIABLE:
    case SDF_BLOCKTYPE_PLAIN_DERIVED:
        print_metadata_plain_variable(b);
        break;
    case SDF_BLOCKTYPE_POINT_VARIABLE:
    case SDF_BLOCKTYPE_POINT_DERIVED:
        print_metadata_point_variable(b);
        break;
    case SDF_BLOCKTYPE_CONSTANT:
        print_metadata_constant(b);
        break;
    case SDF_BLOCKTYPE_ARRAY:
        print_metadata_array(b);
        break;
    case SDF_BLOCKTYPE_CPU_SPLIT:
        print_metadata_cpu_split(b);
        break;
    case SDF_BLOCKTYPE_RUN_INFO:
        print_metadata_run(b);
        break;
    case SDF_BLOCKTYPE_STITCHED:
    case SDF_BLOCKTYPE_CONTIGUOUS:
    case SDF_BLOCKTYPE_STITCHED_TENSOR:
    case SDF_BLOCKTYPE_CONTIGUOUS_TENSOR:
        print_metadata_stitched(b);
        break;
    case SDF_BLOCKTYPE_STITCHED_MATERIAL:
    case SDF_BLOCKTYPE_CONTIGUOUS_MATERIAL:
        print_metadata_stitched_material(b);
        break;
    case SDF_BLOCKTYPE_STITCHED_MATVAR:
    case SDF_BLOCKTYPE_CONTIGUOUS_MATVAR:
        print_metadata_stitched_matvar(b);
        break;
    case SDF_BLOCKTYPE_STITCHED_SPECIES:
    case SDF_BLOCKTYPE_CONTIGUOUS_SPECIES:
        print_metadata_stitched_species(b);
        break;
    case SDF_BLOCKTYPE_STITCHED_OBSTACLE_GROUP:
        print_metadata_stitched_obstacle_group(b);
        break;
    case SDF_BLOCKTYPE_STATION:
    case SDF_BLOCKTYPE_STATION_DERIVED:
        print_metadata_station(b);
        break;
    case SDF_BLOCKTYPE_DATABLOCK:
        print_metadata_datablock(b);
        break;
    case SDF_BLOCKTYPE_NAMEVALUE:
        print_metadata_namevalue(b);
        break;
    }

    printf("\n");
}


static void print_data(sdf_block_t *b)
{
    if (!b->done_data)
        fprintf(stderr, "Data not read.\n");
    else
        fwrite(b->data, 1, b->data_length, stdout);
}


int diff_block(sdf_file_t **handles, sdf_block_t *b1, sdf_block_t *b2)
{
    int32_t *i4_1, *i4_2;
    int64_t *i8_1, *i8_2;
    float *r4_1, *r4_2;
    double *r8_1, *r8_2;
    char *l_1, *l_2;
    double val1, val2, res;
    int64_t n;

    switch (b1->blocktype) {
    case SDF_BLOCKTYPE_PLAIN_DERIVED:
    case SDF_BLOCKTYPE_PLAIN_VARIABLE:
        break;
    default:
        return 0;
    }

    sdf_helper_read_data(handles[0], b1);
    sdf_helper_read_data(handles[1], b2);

    switch (b1->datatype) {
    case(SDF_DATATYPE_INTEGER4):
        i4_1 = b1->data;
        i4_2 = b2->data;
        for (n = 0; n < b1->nelements_local; n++) {
            val1 = i4_1[n];
            val2 = i4_2[n];
            if (ABS(val1 - val2) / MIN(ABS(val1), ABS(val2)) > relerr)
                printf("%s (%i): %g %g\n", b1->id, n, val1, val2);
        }
        break;
    case(SDF_DATATYPE_INTEGER8):
        i8_1 = b1->data;
        i8_2 = b2->data;
        for (n = 0; n < b1->nelements_local; n++) {
            val1 = i8_1[n];
            val2 = i8_2[n];
            if (ABS(val1 - val2) / MIN(ABS(val1), ABS(val2)) > relerr)
                printf("%s (%i): %g %g\n", b1->id, n, val1, val2);
        }
        break;
    case(SDF_DATATYPE_REAL4):
        r4_1 = b1->data;
        r4_2 = b2->data;
        for (n = 0; n < b1->nelements_local; n++) {
            val1 = r4_1[n];
            val2 = r4_2[n];
            if (ABS(val1 - val2) / MIN(ABS(val1), ABS(val2)) > relerr)
                printf("%s (%i): %g %g\n", b1->id, n, val1, val2);
        }
        break;
    case(SDF_DATATYPE_REAL8):
        r8_1 = b1->data;
        r8_2 = b2->data;
        for (n = 0; n < b1->nelements_local; n++) {
            val1 = r8_1[n];
            val2 = r8_2[n];
            res = ABS(val1 - val2) / MIN(ABS(val1), ABS(val2));
            if (ABS(val1 - val2) / MIN(ABS(val1), ABS(val2)) > relerr)
                printf("%s (%i): %g %g\n", b1->id, n, val1, val2);
        }
        break;
    case(SDF_DATATYPE_LOGICAL):
        l_1 = b1->data;
        l_2 = b2->data;
        for (n = 0; n < b1->nelements_local; n++) {
            val1 = l_1[n];
            val2 = l_2[n];
            if (ABS(val1 - val2) / MIN(ABS(val1), ABS(val2)) > relerr)
                printf("%s (%i): %g %g\n", b1->id, n, val1, val2);
        }
        break;
    }

    return 0;
}


int main(int argc, char **argv)
{
    char **files = NULL;
    int i, n, block, err, found, idx, len, range_start;
    int nelements_max;
    sdf_file_t *h, *h2, **handles;
    sdf_block_t *b, *b2, *next, *mesh, *mesh0;
    list_t *station_blocks;
    comm_t comm;
    char zero[16] = {0};

    files = parse_args(&argc, &argv);

#ifdef PARALLEL
    MPI_Init(&argc, &argv);
    MPI_Comm_dup(MPI_COMM_WORLD, &comm);
#else
    comm = 0;
#endif

    handles = calloc(2, sizeof(*handles));
    for (i=0; i<2; i++) {
        h = handles[i] = sdf_open(files[i], comm, SDF_READ, 0);
        if (!h) {
            fprintf(stderr, "Error opening file %s\n", files[i]);
            return 1;
        }

        h->print = debug;
        if (ignore_summary) h->use_summary = 0;
        if (ignore_nblocks) h->ignore_nblocks = 1;
        sdf_stack_init(h);

        sdf_read_header(h);
        h->current_block = NULL;

        // If nblocks is negative then the file is corrupt
        if (h->nblocks < 0) {
            block = (-h->nblocks) / 64;
            err = -h->nblocks - 64 * block;
            fprintf(stderr, "Error code %s found at block %i\n",
                    sdf_error_codes_c[err], block);
        }

        if (derived && extension_info) sdf_extension_print_version(h);

        h->purge_duplicated_ids = purge_duplicate;

        if (derived)
            sdf_read_blocklist_all(h);
        else
            sdf_read_blocklist(h);
    }
    free(files);

    h  = handles[0];
    h2 = handles[1];

    list_init(&station_blocks);

    nelements_max = 0;
    range_start = 0;
    mesh0 = NULL;
    found = 1;
    next = h->blocklist;
    for (i = 0, idx = 1; next; i++, idx++) {
        h->current_block = b = next;
        next = b->next;

        if (nrange > 0 || variable_ids) found = 0;

        for (n = range_start; n < nrange; n++) {
            if (idx < range_list[n].start)
                break;
            if (idx <= range_list[n].end) {
                found = 1;
                break;
            }
            range_start++;
        }

        if (found == 0 && variable_ids) {
            variable_last_id = variable_ids;
            while (variable_last_id) {
                if (!memcmp(b->id, variable_last_id->id,
                        strlen(variable_last_id->id)+1)) {
                    found = 1;
                    break;
                }
                variable_last_id = variable_last_id->next;
            }
        }

        if (exclude_variables) {
            if (found) continue;
        } else {
            if (!found) continue;
        }

        b2 = sdf_find_block_by_id(h2, b->id);
        if (!b2) {
            if (metadata)
                print_metadata(b, idx, h->nblocks);
            else
                printf("%s not found in second file\n", b->id);
            continue;
        }

        diff_block(handles, b, b2);
/*
        switch (b->blocktype) {
        case SDF_BLOCKTYPE_PLAIN_DERIVED:
            sdf_helper_read_data(h, b);
            if (b->station_id) {
                mesh = sdf_find_block_by_id(h, b->mesh_id);
                if (!mesh) continue;
                if (mesh->nelements > nelements_max) {
                    nelements_max = mesh->nelements;
                    mesh0 = mesh;
                }

                if (!mesh->done_data)
                    sdf_helper_read_data(h, mesh);

                list_append(station_blocks, b);
            } else
                pretty_print(h, b, idx);
            break;
        case SDF_BLOCKTYPE_PLAIN_VARIABLE:
        case SDF_BLOCKTYPE_PLAIN_MESH:
        case SDF_BLOCKTYPE_POINT_VARIABLE:
        case SDF_BLOCKTYPE_POINT_MESH:
            sdf_helper_read_data(h, b);
            pretty_print(h, b, idx);
            break;
        case SDF_BLOCKTYPE_DATABLOCK:
            sdf_helper_read_data(h, b);
            print_data(b);
            break;
        default:
            printf("Unsupported blocktype %s\n",
                   sdf_blocktype_c[b->blocktype]);
        }
*/
    }

    if (mesh0 && (variable_ids || nrange > 0)) {
        if (ascii_header) {
            printf("# Stations Time History File\n#\n");
            // This gives garbage output
            //printf("# %s\t%s\t(%s)\n", mesh0->id, mesh0->name, mesh0->units);
            printf("# time\tTime\t(%s)\n", mesh0->units);
        }

        nelements_max = 0;
        b = list_start(station_blocks);
        for (i = 0; i < station_blocks->count; i++) {
            len = strlen(b->station_id);
            if (ascii_header)
                printf("# %s\t%s\t(%s)\n", &b->id[len+1],
                       &b->name[len+1], b->units);
            idx = b->offset + b->nelements_local - mesh0->offset;
            if (idx > nelements_max)
                nelements_max = idx;
            b = list_next(station_blocks);
        }

        if (ascii_header) printf("#\n");

        for (n = 0; n < nelements_max; n++) {
            print_value_element(mesh0->data, mesh0->datatype, n);

            b = list_start(station_blocks);
            for (i = 0; i < station_blocks->count; i++) {
                idx = n + mesh0->offset - b->offset;
                printf(format_space,1);
                if (idx >= 0 && idx < b->nelements_local)
                    print_value_element(b->data, b->datatype_out, n);
                else
                    print_value(zero, b->datatype_out);
                b = list_next(station_blocks);
            }

            printf("\n");
        }
    }

    list_destroy(&station_blocks);
    if (range_list) free(range_list);

    return close_files(handles);
}


int close_files(sdf_file_t **handles)
{
    free_memory(handles);
    sdf_close(handles[0]);
    sdf_close(handles[1]);
    free(handles);
#ifdef PARALLEL
    MPI_Finalize();
#endif

    return 0;
}
