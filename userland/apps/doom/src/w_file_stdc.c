//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	WAD I/O functions.
//

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "m_misc.h"
#include "w_file.h"
#include "z_zone.h"

/*
 * Stellux WAD file I/O - uses raw POSIX read/lseek instead of stdio.
 *
 * musl's stdio buffered I/O (fread/fseek) has issues on Stellux where
 * fread returns 0 bytes after fseek operations. Using raw fd I/O with
 * lseek+read bypasses the stdio buffer and works reliably.
 */

typedef struct
{
    wad_file_t wad;
    int fd;
} stdc_wad_file_t;

extern wad_file_class_t stdc_wad_file;

static wad_file_t *W_StdC_OpenFile(char *path)
{
    stdc_wad_file_t *result;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        return NULL;
    }

    // Get file length via lseek
    off_t len = lseek(fd, 0, SEEK_END);
    if (len < 0) {
        // Fallback: try stat
        struct stat st;
        if (fstat(fd, &st) == 0) {
            len = st.st_size;
        } else {
            len = 0;
        }
    }
    lseek(fd, 0, SEEK_SET);

    result = Z_Malloc(sizeof(stdc_wad_file_t), PU_STATIC, 0);
    result->wad.file_class = &stdc_wad_file;
    result->wad.mapped = NULL;
    result->wad.length = (long)len;
    result->fd = fd;

    return &result->wad;
}

static void W_StdC_CloseFile(wad_file_t *wad)
{
    stdc_wad_file_t *stdc_wad;

    stdc_wad = (stdc_wad_file_t *) wad;

    close(stdc_wad->fd);
    Z_Free(stdc_wad);
}

// Read data from the specified position in the file into the
// provided buffer.  Returns the number of bytes read.

size_t W_StdC_Read(wad_file_t *wad, unsigned int offset,
                   void *buffer, size_t buffer_len)
{
    stdc_wad_file_t *stdc_wad;

    stdc_wad = (stdc_wad_file_t *) wad;

    // Seek to the specified position
    lseek(stdc_wad->fd, (off_t)offset, SEEK_SET);

    // Read into the buffer (handle partial reads)
    size_t total = 0;
    while (total < buffer_len) {
        ssize_t n = read(stdc_wad->fd, (char*)buffer + total,
                         buffer_len - total);
        if (n <= 0) break;
        total += (size_t)n;
    }

    return total;
}


wad_file_class_t stdc_wad_file = 
{
    W_StdC_OpenFile,
    W_StdC_CloseFile,
    W_StdC_Read,
};


