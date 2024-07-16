#ifndef PAT_H
#define PAT_H
#include "msr.h"

#define PAT_MEM_TYPE_UC         0x00 // Uncacheable
#define PAT_MEM_TYPE_WC         0x01 // Write Combining
#define PAT_MEM_TYPE_WT         0x04 // Write Through
#define PAT_MEM_TYPE_WP         0x05 // Write Protected
#define PAT_MEM_TYPE_WB         0x06 // Write Back
#define PAT_MEM_TYPE_UC_        0x07 // Uncached but can be overriden by MTRRs

typedef struct pat_attrib {
    union {
        struct {
            uint8_t type : 3;
            uint8_t rsvd : 5;
        };
        uint8_t raw;
    };
} pat_attrib_t;

typedef struct page_attribute_table {
    union {
        struct {
            pat_attrib_t pa0;
            pat_attrib_t pa1;
            pat_attrib_t pa2;
            pat_attrib_t pa3;
            pat_attrib_t pa4;
            pat_attrib_t pa5;
            pat_attrib_t pa6;
            pat_attrib_t pa7;
        };
        uint64_t raw;
    };
} pat_t;

__PRIVILEGED_CODE
pat_t readPatMsr();

__PRIVILEGED_CODE
void writePatMsr(pat_t pat);

void debugPat(pat_t pat);

#endif
