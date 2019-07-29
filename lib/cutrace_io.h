#ifndef __CUDA_TRACE_READER_H__
#define __CUDA_TRACE_READER_H__

#ifdef __cplusplus__
extern "C" {
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "Common.h"

/** Currently two variants of the trace format exist:
 * Version 2: Uncompressed. Each access made by a GPU thread corresponds to an
 *   individual record.
 * Version 3: Compressed. Consecutive accesses of the same size made by the same
 *   CTA are compressed into a single record with the field "count" set to the
 *   number of consecutive accesses.
 *
 * Both versions share identical headers:
 * 10 Byte: magic numbers as identifier
 *
 * 0x00 signals "new kernel"
 *   2 Byte: length of kernel name
 *   n Byte: kernel name
 *
 * 0xFF signals "uncompressed record" (Version 2, Version 3)
 *   4 Byte: SM Id
 *   4 Byte: <4 bit: type of access> <28 bit: size of access>
 *   8 Byte: Address of access
 *   4 Byte: CTA Id X
 *   2 Byte: CTA Id Y
 *   2 Byte: CTA Id Z
 */

static const uint8_t v2[] = {0x19, 0x43, 0x55, 0x44, 0x41, 0x54, 0x52, 0x41, 0x43, 0x45};
static const uint8_t v3[] = {0x1a, 0x43, 0x55, 0x44, 0x41, 0x54, 0x52, 0x41, 0x43, 0x45};

typedef struct trace_record_addr_t {
  uint64_t addr;
  int32_t offset;
  int8_t count;
} trace_record_addr_t;
    
typedef struct trace_record_t {
  struct {
    uint32_t x;
    uint16_t y;
    uint16_t z;
  } ctaid;
  uint64_t clock;
  
  uint32_t warp;
  uint32_t size;
  uint32_t debug;
  
  uint8_t type;
  uint8_t addr_len;
  uint8_t smid;
  
  trace_record_addr_t addr_unit[];
} trace_record_t;

typedef struct trace_t {
  FILE *file;
  uint16_t block_size;
  uint8_t version;
  char new_kernel;
  char *kernel_name;
  trace_record_t record;
} trace_t;

const char *trace_last_error = NULL;

/******************************************************************************
 * reader
 *****************************************************************************/

#define TRACE_RECORD_SIZE(addr_len) (offsetof(trace_record_t, addr_unit) + sizeof(trace_record_addr_t) * (addr_len))

  
trace_t *trace_open(FILE *f) {
  uint8_t versionbuf[10];
  uint8_t version;

  if (fread(versionbuf, 10, 1, f) != 1) {
    trace_last_error = "unable to read version";
    return NULL;
  }

  if (memcmp(versionbuf, v2, 10) == 0) {
    version = 2;
  } else if (memcmp(versionbuf, v3, 10) == 0) {
    version = 3;
  } else {
    trace_last_error = "invalid version";
    return NULL;
  }

  trace_t *res = (trace_t*)malloc(offsetof(trace_t, record) + TRACE_RECORD_SIZE(32));
  res->file = f;
  res->version = version;
  res->kernel_name = NULL;
  res->new_kernel = 0;
  return res;
}

void __trace_unpack(const record_t *buf, trace_record_t *record) {
  record->addr_len = RECORD_GET_ALEN(buf);
  record->type = RECORD_GET_TYPE(buf);
  record->smid = RECORD_GET_SMID(buf);
  record->warp = RECORD_GET_WARP(buf);
  
  record->ctaid.x = RECORD_GET_CTAX(buf);
  record->ctaid.y = RECORD_GET_CTAY(buf);
  record->ctaid.z = RECORD_GET_CTAZ(buf);

  record->clock = RECORD_GET_CLOCK(buf);
  
  record->size = RECORD_GET_SIZE(buf);

  for (uint8_t i = 0; i < record->addr_len; i++) {
    record->addr_unit[i].addr = RECORD_ADDR(buf, i);
    record->addr_unit[i].offset = RECORD_GET_OFFSET(buf, i);
    record->addr_unit[i].count = RECORD_GET_COUNT(buf, i);
  }
}

void __trace_pack(const trace_record_t *record, record_t *buf) {
  static int a = 0;
  //if (a < 10 ) printf("record->addr_len: %u\n", record->addr_len);

  *buf = RECORD_SET_INIT(record->addr_len, record->type, record->smid, record->warp,
                         record->ctaid.x, record->ctaid.y, record->ctaid.z, record->clock, record->size);
  
  /*
  buf[0] = 0;
  buf[0] |= (uint64_t)(record->addr_len & 0xFF) << 56;
  buf[0] |= (uint64_t)(record->smid & 0xFF) << 48;
  buf[0] |= record->warp & 0xFFFFFFFF;

  buf[1] = 0;
  buf[1] |= (uint64_t)(record->ctaid.x & 0xFFFFFFFF) << 32;
  buf[1] |= (uint64_t)(record->ctaid.y & 0xFFFF) << 16;
  buf[1] |= record->ctaid.z & 0xFFFF;

  buf[2] = 0;
  buf[2] |= (uint64_t)(record->type_size) << 32;
  buf[2] |= record->clock & 0xFFFFFFFF;
  */
  //if (a++ < 10 ) printf("record->addr_len: %u\n", record->addr_len);
  for (uint8_t i = 0; i < record->addr_len; i++) {
    RECORD_ADDR(buf, i) = record->addr_unit[i].addr;
    RECORD_ADDR_META(buf, i) = (record->addr_unit[i].offset << 8) | ((int64_t)record->addr_unit[i].count & 0xFF);
    //printf("%lu, %d, %d\n", RECORD_ADDR(buf, i), RECORD_GET_OFFSET(buf, i), RECORD_GET_COUNT(buf, i));

    //printf("%lx\n", RECORD_ADDR(buf, i));
  }
}

// returns 0 on success
int trace_next(trace_t *t) {
  uint64_t buf[TRACE_RECORD_SIZE(32)/8+1]; // mem space for addr_len == threads per warp
  //int ch = fgetc(t->file);
  // end of file, this is not an error
  if (fread(buf, 8 * sizeof(char), 1, t->file) != 1) {
    trace_last_error = NULL;
    return 1;
  }
  uint8_t ch = (buf[0] >> 56) & 0xFF;
  //printf("ch: %d\n", ch);

  // Entry is a kernel
  if (ch == 0x00) {
    uint8_t name_len = (buf[0] >> 48) & 0xFF;
    uint16_t block_size = (buf[0] >> 32) & 0xFFFF;
    /*if (fread(&name_len, 2, 1, t->file) != 1) {
      trace_last_error = "unable to read kernel name length";
      return 1;
      }*/
    if (t->kernel_name != NULL) {
      free(t->kernel_name);
    }
    t->kernel_name = (char*)malloc(name_len+1);
    if (fread(t->kernel_name, name_len, 1, t->file) != 1) {
      trace_last_error = "unable to read kernel name length";
      return 1;
    }
    t->kernel_name[name_len] = '\0';
    t->new_kernel = 1;
    t->block_size = block_size;
    trace_last_error = NULL;
    return 0;
  }

  // Entry is an record
  else {
    //*buf = (char)ch;
    /*
    char *buf_cur = (char *) buf;
    printf("BEFORE\n");
    for (int i = 0; i < (int)RECORD_RAW_SIZE(ch); i++)
    {
      
      printf("%1x ", buf_cur[i]);

      if ( (i & 0xF) == 0xF )
        printf("\n");
    }
    printf("\n");
    */
    t->new_kernel = 0;
    if (fread(buf+1, RECORD_RAW_SIZE(ch) - 8, 1, t->file) != 1) {
      trace_last_error = "unable to read record";
      return 1;
    }
    /*
    buf_cur = (char *) buf;
    printf("AFTER\n");
    for (int i = 0; i < (int)RECORD_RAW_SIZE(ch); i++)
    {
      
      printf("%1x ", buf_cur[i]);

      if ( (i & 0xF) == 0xF )
        printf("\n");
    }
    printf("\n");
    */
    __trace_unpack((record_t *)buf, &t->record);
    trace_last_error = NULL;
    /*
    printf("\tACC    type: %u, size: %u, smid: %u, "
           "ctaid: (%u, %u, %u), warp: %u, clock: %u, addr_len: %u, addr: %lx\n",
           TRACE_GETTYPE(t->record.type_size), TRACE_GETSIZE(t->record.type_size),
           t->record.smid, t->record.ctaid.x, t->record.ctaid.y, t->record.ctaid.z,
           t->record.warp, t->record.clock, t->record.addr_len, t->record.addr->addr);
    buf_cur = (char *) buf;
    printf("FINAL\n");
    for (int i = 0; i < (int)RECORD_RAW_SIZE(ch); i++)
    {
      
      printf("%1x ", buf_cur[i]);

      if ( (i & 0xF) == 0xF )
        printf("\n");
    }
    printf("\n");
    */
    return 0;
  }

  
/*
  if (ch == 0xFF) {
    t->new_kernel = 0;
    uint64_t buf[4];
    if (fread(buf, 32, 1, t->file) != 1) {
      trace_last_error = "unable to read uncompressed record";
      return 1;
    }
    __trace_unpack(buf, &t->record);
    t->record.count = 1;
    trace_last_error = NULL;
    return 0;
  }

  // Entry is a compressed record
  if (ch == 0xFE) {
    t->new_kernel = 0;
    uint64_t buf[4];
    if (fread(buf, 32, 1, t->file) != 1) {
      trace_last_error = "unable to read compressed record";
      return 1;
    }
    __trace_unpack(buf, &t->record);

    // read count
    uint16_t countbuf;
    if (fread(&countbuf, 2, 1, t->file) != 1) {
      trace_last_error = "unable to read repetition count";
      return 1;
    }

    t->record.count = countbuf;
    trace_last_error = NULL;
    return 0;
  }
  trace_last_error = "invalid entry marker";
  return 1;
*/
}

int trace_eof(trace_t *t) {
  return feof(t->file);
}

void trace_close(trace_t *t) {
  if (t->kernel_name) {
    free(t->kernel_name);
  }
  fclose(t->file);
  free(t);
}

/******************************************************************************
 * writer
 *****************************************************************************/

int trace_write_header(FILE *f, int version) {
  if (version == 2) {
    if (fwrite(v2, 10, 1, f) < 1) {
      trace_last_error = "write error";
      return 1;
    }
  } else if (version == 3) {
    if (fwrite(v3, 10, 1, f) < 1) {
      trace_last_error = "write error";
      return 1;
    }
  } else {
    trace_last_error = "invalid version";
    return 1;
  }
  trace_last_error = NULL;
  return 0;
}

int trace_write_kernel(FILE *f, const char* name, uint16_t block_size) {
  //uint8_t bufmarker = 0x00;
  uint8_t name_len = strlen(name) & 0xFF;
  //printf("Write kernel - %s, %d, %d\n", name, name_len, block_size);
  
  //printf("trace_write_kernel()\n");
  uint64_t header = ((uint64_t)name_len << 48) | ((uint64_t)block_size << 32);
  //if (fwrite(&bufmarker, 1, 1, f) < 1) {
  //trace_last_error = "write error";
  //return 1;
  //}
  //printf("%016lx\n", header);
  if (fwrite(&header, 8, 1, f) < 1) {
    trace_last_error = "write error";
    return 1;
  }
  //printf("%s\n", name);
  if (fwrite(name, name_len, 1, f) < 1) {
    trace_last_error = "write error";
    return 1;
  }
  trace_last_error = NULL;
  return 0;
}

int trace_write_record(FILE *f, const trace_record_t *record) {
  /*uint8_t marker = record->count == 1 ? 0xFF : 0xFE;
  //uint8_t marker = 0xFF;
  if (fwrite(&marker, 1, 1, f) < 1) {
    trace_last_error = "write error";
    return 1;
  }
  */
  //printf("Write record\n");
  char buf[TRACE_RECORD_SIZE(32)]; // mem space for addr_len == threads per warp
  __trace_pack(record, (record_t *)buf);
  if (fwrite(buf, RECORD_RAW_SIZE(record->addr_len), 1, f) < 1) {
    trace_last_error = "write error";
    return 1;
  }
/*
  if (record->count > 1) {
    if (fwrite(&record->count, 2, 1, f) < 1) {
      trace_last_error = "write error";
      return 1;
    }
  }
  trace_last_error = NULL;
*/
  return 0;
}

#ifdef __cplusplus__
}
#endif

#endif
