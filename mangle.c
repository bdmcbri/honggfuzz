/*
 *
 * honggfuzz - run->dynamicFilefer mangling routines
 * -----------------------------------------
 *
 * Author:
 * Robert Swiecki <swiecki@google.com>
 *
 * Copyright 2010-2018 by Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * permissions and limitations under the License.
 *
 */

#include "mangle.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "input.h"
#include "libhfcommon/common.h"
#include "libhfcommon/log.h"
#include "libhfcommon/util.h"

static inline void mangle_Overwrite(run_t* run, const uint8_t* src, size_t off, size_t sz) {
    size_t maxToCopy = run->dynamicFileSz - off;
    if (sz > maxToCopy) {
        sz = maxToCopy;
    }

    memmove(&run->dynamicFile[off], src, sz);
}

static inline void mangle_Move(run_t* run, size_t off_from, size_t off_to, size_t len) {
    if (off_from >= run->dynamicFileSz) {
        return;
    }
    if (off_to >= run->dynamicFileSz) {
        return;
    }

    ssize_t len_from = (ssize_t)run->dynamicFileSz - off_from - 1;
    ssize_t len_to = (ssize_t)run->dynamicFileSz - off_to - 1;

    if ((ssize_t)len > len_from) {
        len = len_from;
    }
    if ((ssize_t)len > len_to) {
        len = len_to;
    }

    memmove(&run->dynamicFile[off_to], &run->dynamicFile[off_from], len);
}

static void mangle_Inflate(run_t* run, size_t off, size_t len, bool printable) {
    if (run->dynamicFileSz >= run->global->mutate.maxFileSz) {
        return;
    }
    if (len > (run->global->mutate.maxFileSz - run->dynamicFileSz)) {
        len = run->global->mutate.maxFileSz - run->dynamicFileSz;
    }

    input_setSize(run, run->dynamicFileSz + len);
    mangle_Move(run, off, off + len, run->dynamicFileSz);
    if (printable) {
        util_rndBufPrintable(&run->dynamicFile[off], len);
    } else {
        util_rndBuf(&run->dynamicFile[off], len);
    }
}

static void mangle_MemMove(run_t* run, bool printable HF_ATTR_UNUSED) {
    size_t off_from = util_rndGet(0, run->dynamicFileSz - 1);
    size_t off_to = util_rndGet(0, run->dynamicFileSz - 1);
    size_t len = util_rndGet(0, run->dynamicFileSz);

    mangle_Move(run, off_from, off_to, len);
}

static void mangle_Bytes(run_t* run, bool printable) {
    size_t off = util_rndGet(0, run->dynamicFileSz - 1);

    uint64_t buf;
    if (printable) {
        util_rndBufPrintable((uint8_t*)&buf, sizeof(buf));
    } else {
        util_rndBuf((uint8_t*)&buf, sizeof(buf));
    }

    /* Overwrite with random 1-8-byte values */
    size_t toCopy = util_rndGet(1, 8);
    mangle_Overwrite(run, (uint8_t*)&buf, off, toCopy);
}

static void mangle_Bit(run_t* run, bool printable) {
    size_t off = util_rndGet(0, run->dynamicFileSz - 1);
    run->dynamicFile[off] ^= (uint8_t)(1U << util_rndGet(0, 7));
    if (printable) {
        util_turnToPrintable(&(run->dynamicFile[off]), 1);
    }
}

static void mangle_DictionaryInsertNoCheck(run_t* run, bool printable) {
    uint64_t choice = util_rndGet(0, run->global->mutate.dictionaryCnt - 1);
    struct strings_t* str = TAILQ_FIRST(&run->global->mutate.dictq);
    for (uint64_t i = 0; i < choice; i++) {
        str = TAILQ_NEXT(str, pointers);
    }

    size_t off = util_rndGet(0, run->dynamicFileSz - 1);
    mangle_Inflate(run, off, str->len, printable);
    mangle_Overwrite(run, (uint8_t*)str->s, off, str->len);
}

static void mangle_DictionaryInsert(run_t* run, bool printable) {
    if (run->global->mutate.dictionaryCnt == 0) {
        mangle_Bit(run, printable);
        return;
    }
    mangle_DictionaryInsertNoCheck(run, printable);
}

static void mangle_DictionaryNoCheck(run_t* run) {
    size_t off = util_rndGet(0, run->dynamicFileSz - 1);

    uint64_t choice = util_rndGet(0, run->global->mutate.dictionaryCnt - 1);
    struct strings_t* str = TAILQ_FIRST(&run->global->mutate.dictq);
    for (uint64_t i = 0; i < choice; i++) {
        str = TAILQ_NEXT(str, pointers);
    }

    mangle_Overwrite(run, (uint8_t*)str->s, off, str->len);
}

static void mangle_Dictionary(run_t* run, bool printable) {
    if (run->global->mutate.dictionaryCnt == 0) {
        mangle_Bit(run, printable);
        return;
    }

    mangle_DictionaryNoCheck(run);
}

static void mangle_Magic(run_t* run, bool printable) {
    static const struct {
        const uint8_t val[8];
        const size_t size;
    } mangleMagicVals[] = {
        /* 1B - No endianness */
        {"\x00\x00\x00\x00\x00\x00\x00\x00", 1},
        {"\x01\x00\x00\x00\x00\x00\x00\x00", 1},
        {"\x02\x00\x00\x00\x00\x00\x00\x00", 1},
        {"\x03\x00\x00\x00\x00\x00\x00\x00", 1},
        {"\x04\x00\x00\x00\x00\x00\x00\x00", 1},
        {"\x05\x00\x00\x00\x00\x00\x00\x00", 1},
        {"\x06\x00\x00\x00\x00\x00\x00\x00", 1},
        {"\x07\x00\x00\x00\x00\x00\x00\x00", 1},
        {"\x08\x00\x00\x00\x00\x00\x00\x00", 1},
        {"\x09\x00\x00\x00\x00\x00\x00\x00", 1},
        {"\x0A\x00\x00\x00\x00\x00\x00\x00", 1},
        {"\x0B\x00\x00\x00\x00\x00\x00\x00", 1},
        {"\x0C\x00\x00\x00\x00\x00\x00\x00", 1},
        {"\x0D\x00\x00\x00\x00\x00\x00\x00", 1},
        {"\x0E\x00\x00\x00\x00\x00\x00\x00", 1},
        {"\x0F\x00\x00\x00\x00\x00\x00\x00", 1},
        {"\x10\x00\x00\x00\x00\x00\x00\x00", 1},
        {"\x20\x00\x00\x00\x00\x00\x00\x00", 1},
        {"\x40\x00\x00\x00\x00\x00\x00\x00", 1},
        {"\x7E\x00\x00\x00\x00\x00\x00\x00", 1},
        {"\x7F\x00\x00\x00\x00\x00\x00\x00", 1},
        {"\x80\x00\x00\x00\x00\x00\x00\x00", 1},
        {"\x81\x00\x00\x00\x00\x00\x00\x00", 1},
        {"\xC0\x00\x00\x00\x00\x00\x00\x00", 1},
        {"\xFE\x00\x00\x00\x00\x00\x00\x00", 1},
        {"\xFF\x00\x00\x00\x00\x00\x00\x00", 1},
        /* 2B - NE */
        {"\x00\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\x01\x01\x00\x00\x00\x00\x00\x00", 2},
        {"\x80\x80\x00\x00\x00\x00\x00\x00", 2},
        {"\xFF\xFF\x00\x00\x00\x00\x00\x00", 2},
        /* 2B - BE */
        {"\x00\x01\x00\x00\x00\x00\x00\x00", 2},
        {"\x00\x02\x00\x00\x00\x00\x00\x00", 2},
        {"\x00\x03\x00\x00\x00\x00\x00\x00", 2},
        {"\x00\x04\x00\x00\x00\x00\x00\x00", 2},
        {"\x00\x05\x00\x00\x00\x00\x00\x00", 2},
        {"\x00\x06\x00\x00\x00\x00\x00\x00", 2},
        {"\x00\x07\x00\x00\x00\x00\x00\x00", 2},
        {"\x00\x08\x00\x00\x00\x00\x00\x00", 2},
        {"\x00\x09\x00\x00\x00\x00\x00\x00", 2},
        {"\x00\x0A\x00\x00\x00\x00\x00\x00", 2},
        {"\x00\x0B\x00\x00\x00\x00\x00\x00", 2},
        {"\x00\x0C\x00\x00\x00\x00\x00\x00", 2},
        {"\x00\x0D\x00\x00\x00\x00\x00\x00", 2},
        {"\x00\x0E\x00\x00\x00\x00\x00\x00", 2},
        {"\x00\x0F\x00\x00\x00\x00\x00\x00", 2},
        {"\x00\x10\x00\x00\x00\x00\x00\x00", 2},
        {"\x00\x20\x00\x00\x00\x00\x00\x00", 2},
        {"\x00\x40\x00\x00\x00\x00\x00\x00", 2},
        {"\x00\x7E\x00\x00\x00\x00\x00\x00", 2},
        {"\x00\x7F\x00\x00\x00\x00\x00\x00", 2},
        {"\x00\x80\x00\x00\x00\x00\x00\x00", 2},
        {"\x00\x81\x00\x00\x00\x00\x00\x00", 2},
        {"\x00\xC0\x00\x00\x00\x00\x00\x00", 2},
        {"\x00\xFE\x00\x00\x00\x00\x00\x00", 2},
        {"\x00\xFF\x00\x00\x00\x00\x00\x00", 2},
        {"\x7E\xFF\x00\x00\x00\x00\x00\x00", 2},
        {"\x7F\xFF\x00\x00\x00\x00\x00\x00", 2},
        {"\x80\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\x80\x01\x00\x00\x00\x00\x00\x00", 2},
        {"\xFF\xFE\x00\x00\x00\x00\x00\x00", 2},
        /* 2B - LE */
        {"\x00\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\x01\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\x02\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\x03\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\x04\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\x05\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\x06\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\x07\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\x08\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\x09\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\x0A\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\x0B\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\x0C\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\x0D\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\x0E\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\x0F\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\x10\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\x20\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\x40\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\x7E\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\x7F\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\x80\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\x81\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\xC0\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\xFE\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\xFF\x00\x00\x00\x00\x00\x00\x00", 2},
        {"\xFF\x7E\x00\x00\x00\x00\x00\x00", 2},
        {"\xFF\x7F\x00\x00\x00\x00\x00\x00", 2},
        {"\x00\x80\x00\x00\x00\x00\x00\x00", 2},
        {"\x01\x80\x00\x00\x00\x00\x00\x00", 2},
        {"\xFE\xFF\x00\x00\x00\x00\x00\x00", 2},
        /* 4B - NE */
        {"\x00\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\x01\x01\x01\x01\x00\x00\x00\x00", 4},
        {"\x80\x80\x80\x80\x00\x00\x00\x00", 4},
        {"\xFF\xFF\xFF\xFF\x00\x00\x00\x00", 4},
        /* 4B - BE */
        {"\x00\x00\x00\x01\x00\x00\x00\x00", 4},
        {"\x00\x00\x00\x02\x00\x00\x00\x00", 4},
        {"\x00\x00\x00\x03\x00\x00\x00\x00", 4},
        {"\x00\x00\x00\x04\x00\x00\x00\x00", 4},
        {"\x00\x00\x00\x05\x00\x00\x00\x00", 4},
        {"\x00\x00\x00\x06\x00\x00\x00\x00", 4},
        {"\x00\x00\x00\x07\x00\x00\x00\x00", 4},
        {"\x00\x00\x00\x08\x00\x00\x00\x00", 4},
        {"\x00\x00\x00\x09\x00\x00\x00\x00", 4},
        {"\x00\x00\x00\x0A\x00\x00\x00\x00", 4},
        {"\x00\x00\x00\x0B\x00\x00\x00\x00", 4},
        {"\x00\x00\x00\x0C\x00\x00\x00\x00", 4},
        {"\x00\x00\x00\x0D\x00\x00\x00\x00", 4},
        {"\x00\x00\x00\x0E\x00\x00\x00\x00", 4},
        {"\x00\x00\x00\x0F\x00\x00\x00\x00", 4},
        {"\x00\x00\x00\x10\x00\x00\x00\x00", 4},
        {"\x00\x00\x00\x20\x00\x00\x00\x00", 4},
        {"\x00\x00\x00\x40\x00\x00\x00\x00", 4},
        {"\x00\x00\x00\x7E\x00\x00\x00\x00", 4},
        {"\x00\x00\x00\x7F\x00\x00\x00\x00", 4},
        {"\x00\x00\x00\x80\x00\x00\x00\x00", 4},
        {"\x00\x00\x00\x81\x00\x00\x00\x00", 4},
        {"\x00\x00\x00\xC0\x00\x00\x00\x00", 4},
        {"\x00\x00\x00\xFE\x00\x00\x00\x00", 4},
        {"\x00\x00\x00\xFF\x00\x00\x00\x00", 4},
        {"\x7E\xFF\xFF\xFF\x00\x00\x00\x00", 4},
        {"\x7F\xFF\xFF\xFF\x00\x00\x00\x00", 4},
        {"\x80\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\x80\x00\x00\x01\x00\x00\x00\x00", 4},
        {"\xFF\xFF\xFF\xFE\x00\x00\x00\x00", 4},
        /* 4B - LE */
        {"\x00\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\x01\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\x02\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\x03\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\x04\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\x05\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\x06\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\x07\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\x08\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\x09\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\x0A\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\x0B\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\x0C\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\x0D\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\x0E\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\x0F\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\x10\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\x20\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\x40\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\x7E\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\x7F\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\x80\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\x81\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\xC0\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\xFE\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\xFF\x00\x00\x00\x00\x00\x00\x00", 4},
        {"\xFF\xFF\xFF\x7E\x00\x00\x00\x00", 4},
        {"\xFF\xFF\xFF\x7F\x00\x00\x00\x00", 4},
        {"\x00\x00\x00\x80\x00\x00\x00\x00", 4},
        {"\x01\x00\x00\x80\x00\x00\x00\x00", 4},
        {"\xFE\xFF\xFF\xFF\x00\x00\x00\x00", 4},
        /* 8B - NE */
        {"\x00\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\x01\x01\x01\x01\x01\x01\x01\x01", 8},
        {"\x80\x80\x80\x80\x80\x80\x80\x80", 8},
        {"\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8},
        /* 8B - BE */
        {"\x00\x00\x00\x00\x00\x00\x00\x01", 8},
        {"\x00\x00\x00\x00\x00\x00\x00\x02", 8},
        {"\x00\x00\x00\x00\x00\x00\x00\x03", 8},
        {"\x00\x00\x00\x00\x00\x00\x00\x04", 8},
        {"\x00\x00\x00\x00\x00\x00\x00\x05", 8},
        {"\x00\x00\x00\x00\x00\x00\x00\x06", 8},
        {"\x00\x00\x00\x00\x00\x00\x00\x07", 8},
        {"\x00\x00\x00\x00\x00\x00\x00\x08", 8},
        {"\x00\x00\x00\x00\x00\x00\x00\x09", 8},
        {"\x00\x00\x00\x00\x00\x00\x00\x0A", 8},
        {"\x00\x00\x00\x00\x00\x00\x00\x0B", 8},
        {"\x00\x00\x00\x00\x00\x00\x00\x0C", 8},
        {"\x00\x00\x00\x00\x00\x00\x00\x0D", 8},
        {"\x00\x00\x00\x00\x00\x00\x00\x0E", 8},
        {"\x00\x00\x00\x00\x00\x00\x00\x0F", 8},
        {"\x00\x00\x00\x00\x00\x00\x00\x10", 8},
        {"\x00\x00\x00\x00\x00\x00\x00\x20", 8},
        {"\x00\x00\x00\x00\x00\x00\x00\x40", 8},
        {"\x00\x00\x00\x00\x00\x00\x00\x7E", 8},
        {"\x00\x00\x00\x00\x00\x00\x00\x7F", 8},
        {"\x00\x00\x00\x00\x00\x00\x00\x80", 8},
        {"\x00\x00\x00\x00\x00\x00\x00\x81", 8},
        {"\x00\x00\x00\x00\x00\x00\x00\xC0", 8},
        {"\x00\x00\x00\x00\x00\x00\x00\xFE", 8},
        {"\x00\x00\x00\x00\x00\x00\x00\xFF", 8},
        {"\x7E\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8},
        {"\x7F\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8},
        {"\x80\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\x80\x00\x00\x00\x00\x00\x00\x01", 8},
        {"\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFE", 8},
        /* 8B - LE */
        {"\x00\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\x01\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\x02\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\x03\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\x04\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\x05\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\x06\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\x07\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\x08\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\x09\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\x0A\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\x0B\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\x0C\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\x0D\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\x0E\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\x0F\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\x10\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\x20\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\x40\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\x7E\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\x7F\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\x80\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\x81\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\xC0\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\xFE\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\xFF\x00\x00\x00\x00\x00\x00\x00", 8},
        {"\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x7E", 8},
        {"\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x7F", 8},
        {"\x00\x00\x00\x00\x00\x00\x00\x80", 8},
        {"\x01\x00\x00\x00\x00\x00\x00\x80", 8},
        {"\xFE\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8},
    };

    size_t off = util_rndGet(0, run->dynamicFileSz - 1);
    uint64_t choice = util_rndGet(0, ARRAYSIZE(mangleMagicVals) - 1);
    mangle_Overwrite(run, mangleMagicVals[choice].val, off, mangleMagicVals[choice].size);

    if (printable) {
        util_turnToPrintable(&run->dynamicFile[off], mangleMagicVals[choice].size);
    }
}

static void mangle_MemSetWithVal(run_t* run, int val) {
    size_t off = util_rndGet(0, run->dynamicFileSz - 1);
    size_t sz = util_rndGet(1, run->dynamicFileSz - off);

    memset(&run->dynamicFile[off], val, sz);
}

static void mangle_MemSet(run_t* run, bool printable) {
    mangle_MemSetWithVal(
        run, printable ? (int)util_rndPrintable() : (int)util_rndGet(0, UINT8_MAX));
}

static void mangle_Random(run_t* run, bool printable) {
    size_t off = util_rndGet(0, run->dynamicFileSz - 1);
    size_t len = util_rndGet(1, run->dynamicFileSz - off);
    if (printable) {
        util_rndBufPrintable(&run->dynamicFile[off], len);
    } else {
        util_rndBuf(&run->dynamicFile[off], len);
    }
}

static void mangle_AddSubWithRange(run_t* run, size_t off, uint64_t varLen) {
    int delta = (int)util_rndGet(0, 8192);
    delta -= 4096;

    switch (varLen) {
        case 1: {
            run->dynamicFile[off] += delta;
            return;
            break;
        }
        case 2: {
            int16_t val;
            memcpy(&val, &run->dynamicFile[off], sizeof(val));
            if (util_rnd64() & 0x1) {
                val += delta;
            } else {
                /* Foreign endianess */
                val = __builtin_bswap16(val);
                val += delta;
                val = __builtin_bswap16(val);
            }
            mangle_Overwrite(run, (uint8_t*)&val, off, varLen);
            return;
            break;
        }
        case 4: {
            int32_t val;
            memcpy(&val, &run->dynamicFile[off], sizeof(val));
            if (util_rnd64() & 0x1) {
                val += delta;
            } else {
                /* Foreign endianess */
                val = __builtin_bswap32(val);
                val += delta;
                val = __builtin_bswap32(val);
            }
            mangle_Overwrite(run, (uint8_t*)&val, off, varLen);
            return;
            break;
        }
        case 8: {
            int64_t val;
            memcpy(&val, &run->dynamicFile[off], sizeof(val));
            if (util_rnd64() & 0x1) {
                val += delta;
            } else {
                /* Foreign endianess */
                val = __builtin_bswap64(val);
                val += delta;
                val = __builtin_bswap64(val);
            }
            mangle_Overwrite(run, (uint8_t*)&val, off, varLen);
            return;
            break;
        }
        default: {
            LOG_F("Unknown variable length size: %" PRIu64, varLen);
            break;
        }
    }
}

static void mangle_AddSub(run_t* run, bool printable) {
    size_t off = util_rndGet(0, run->dynamicFileSz - 1);

    /* 1,2,4,8 */
    uint64_t varLen = 1U << util_rndGet(0, 3);
    if ((run->dynamicFileSz - off) < varLen) {
        varLen = 1;
    }

    mangle_AddSubWithRange(run, off, varLen);
    if (printable) {
        util_turnToPrintable((uint8_t*)&run->dynamicFile[off], varLen);
    }
}

static void mangle_IncByte(run_t* run, bool printable) {
    size_t off = util_rndGet(0, run->dynamicFileSz - 1);
    if (printable) {
        run->dynamicFile[off] = (run->dynamicFile[off] - 32 + 1) % 95 + 32;
    } else {
        run->dynamicFile[off] += (uint8_t)1UL;
    }
}

static void mangle_DecByte(run_t* run, bool printable) {
    size_t off = util_rndGet(0, run->dynamicFileSz - 1);
    if (printable) {
        run->dynamicFile[off] = (run->dynamicFile[off] - 32 + 94) % 95 + 32;
    } else {
        run->dynamicFile[off] -= (uint8_t)1UL;
    }
}

static void mangle_NegByte(run_t* run, bool printable) {
    size_t off = util_rndGet(0, run->dynamicFileSz - 1);
    if (printable) {
        run->dynamicFile[off] = 94 - (run->dynamicFile[off] - 32) + 32;
    } else {
        run->dynamicFile[off] = ~(run->dynamicFile[off]);
    }
}

static void mangle_CloneByte(run_t* run, bool printable HF_ATTR_UNUSED) {
    size_t off1 = util_rndGet(0, run->dynamicFileSz - 1);
    size_t off2 = util_rndGet(0, run->dynamicFileSz - 1);

    uint8_t tmp = run->dynamicFile[off1];
    run->dynamicFile[off1] = run->dynamicFile[off2];
    run->dynamicFile[off2] = tmp;
}

static void mangle_Expand(run_t* run, bool printable) {
    size_t off = util_rndGet(0, run->dynamicFileSz - 1);
    size_t len = util_rndGet(1, run->dynamicFileSz - off);

    mangle_Inflate(run, off, len, printable);
}

static void mangle_Shrink(run_t* run, bool printable HF_ATTR_UNUSED) {
    if (run->dynamicFileSz <= 1U) {
        return;
    }

    size_t len = util_rndGet(1, run->dynamicFileSz - 1);
    size_t off = util_rndGet(0, len);

    input_setSize(run, run->dynamicFileSz - len);
    mangle_Move(run, off + len, off, run->dynamicFileSz);
}

static void mangle_Resize(run_t* run, bool printable) {
    size_t oldsz = run->dynamicFileSz;
    uint64_t v = util_rndGet(0, 16);
    ssize_t newsz = 0;

    switch (v) {
        case 0:
            newsz = (ssize_t)util_rndGet(1, run->global->mutate.maxFileSz);
            break;
        case 1 ... 8:
            newsz = oldsz + v;
            break;
        case 9 ... 16:
            newsz = oldsz + 8 - v;
            break;
        default:
            LOG_F("Illegal value from util_rndGet: %" PRIu64, v);
            break;
    }
    if (newsz < 1) {
        newsz = 1;
    }
    if (newsz > (ssize_t)run->global->mutate.maxFileSz) {
        newsz = run->global->mutate.maxFileSz;
    }

    input_setSize(run, (size_t)newsz);
    if (newsz > (ssize_t)oldsz) {
        if (printable) {
            util_rndBufPrintable(&run->dynamicFile[oldsz], newsz - oldsz);
        } else {
            util_rndBuf(&run->dynamicFile[oldsz], newsz - oldsz);
        }
    }
}

static void mangle_ASCIIVal(run_t* run, bool printable HF_ATTR_UNUSED) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%" PRId64, (int64_t)util_rnd64());
    size_t off = util_rndGet(0, run->dynamicFileSz - 1);

    mangle_Overwrite(run, (uint8_t*)buf, off, strlen(buf));
}

void mangle_mangleContent(run_t* run) {
    static void (*const mangleFuncs[])(run_t * run, bool printable) = {
        mangle_Bit,
        mangle_Bytes,
        mangle_Magic,
        mangle_IncByte,
        mangle_DecByte,
        mangle_NegByte,
        mangle_AddSub,
        mangle_Dictionary,
        mangle_DictionaryInsert,
        mangle_MemMove,
        mangle_MemSet,
        mangle_Random,
        mangle_CloneByte,
        mangle_Expand,
        mangle_Shrink,
        mangle_ASCIIVal,
    };

    if (run->mutationsPerRun == 0U) {
        return;
    }

    mangle_Resize(run, /* printable= */ run->global->cfg.only_printable);

    /* Max number of stacked changes is, by default, 6 */
    uint64_t changesCnt = util_rndGet(1, run->global->mutate.mutationsPerRun);
    for (uint64_t x = 0; x < changesCnt; x++) {
        uint64_t choice = util_rndGet(0, ARRAYSIZE(mangleFuncs) - 1);
        mangleFuncs[choice](run, /* printable= */ run->global->cfg.only_printable);
    }
}
