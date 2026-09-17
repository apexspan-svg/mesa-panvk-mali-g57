#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "genxml/v9_pack.h"

static void
hexdump(const char *name, const void *p, size_t n)
{
   const uint8_t *b = p;
   printf("%s (%zu bytes):\n", name, n);
   for (size_t i = 0; i < n; i += 8) {
      printf("  %02zu:", i);
      for (size_t j = 0; j < 8 && i + j < n; j++)
         printf(" %02x", b[i + j]);
      printf("\n");
   }
}

int
main(void)
{
   printf("sizeof job_header_packed = %zu\n", sizeof(struct mali_job_header_packed));
   printf("sizeof write_value_job_packed = %zu\n",
          sizeof(struct mali_write_value_job_packed));

   struct mali_write_value_job_packed wv;
   pan_pack(&wv, WRITE_VALUE_JOB, cfg) {
      pan_section_pack(cfg, WRITE_VALUE_JOB, HEADER, header) {
         header.type = MALI_JOB_TYPE_WRITE_VALUE;
         header.index = 1;
         header.is_64b = true;
         header.next = 0;
      }
      pan_section_pack(cfg, WRITE_VALUE_JOB, PAYLOAD, payload) {
         payload.address = UINT64_C(0x7f00000000);
         payload.type = MALI_WRITE_VALUE_TYPE_IMMEDIATE_32;
         payload.immediate_value = UINT64_C(0x2a2a2a2a);
      }
   }
   hexdump("write_value_job", &wv, sizeof(wv));

   struct mali_job_header_packed hdr;
   void *zero = calloc(1, sizeof(hdr));
   memcpy(&hdr, zero, sizeof(hdr));
   free(zero);
   printf("job_header must be all-zero: ok\n");
   return 0;
}