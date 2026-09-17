/*
 * pan_gen_shader -- gera binarios de shader Valhall (arch 9) para o teste de
 * job computado kbase_compute_test.
 *
 * Pipeline espelhado do driver GL (pan_shader.c):
 *   pan_preprocess_nir -> pan_postprocess_nir -> pan_shader_compile
 *
 * Modes:
 *   --addr <hex>  : escreve 0x2A2A2A2A em 4 bytes no endereco dado (u64)
 *                   via nir_store_global (sem descritor/ABI).
 *   --nop         : shader sem side-effect (bloco terminal = NOP).
 *
 * gpu_id padrao: 0x90930010 (Mali-G57 MC2).
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/nir/nir_builder.h"
#include "compiler/nir/nir.h"
#include "panfrost/compiler/pan_compiler.h"
#include "panfrost/compiler/bifrost/valhall/disassemble.h"
#include "panfrost/lib/pan_props.h"
#include "util/u_dynarray.h"
#include "util/macros.h"

int __android_log_write(int prio, const char *tag, const char *msg);
int __android_log_write(int prio, const char *tag, const char *msg)
{
   (void)prio; (void)tag; (void)msg;
   return 0;
}

static void
check(const char *what, bool ok)
{
   if (!ok) {
      fprintf(stderr, "FALHOU: %s\n", what);
      exit(1);
   }
}

int
main(int argc, char **argv)
{
   uint64_t gpu_id = UINT64_C(0x90930010);
   uint64_t addr = 0;
   uint32_t data = 0x2A2A2A2A;
   const char *out = NULL;
   bool want_store = false;
   bool want_fau = false;
   bool want_disasm = false;

   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--addr") && i + 1 < argc) {
         addr = strtoull(argv[++i], NULL, 0);
         want_store = true;
      } else if (!strcmp(argv[i], "--data") && i + 1 < argc) {
         data = (uint32_t)strtoul(argv[++i], NULL, 0);
      } else if (!strcmp(argv[i], "--nop")) {
         want_store = false;
      } else if (!strcmp(argv[i], "--fau")) {
         want_fau = true;
      } else if (!strcmp(argv[i], "--disasm")) {
         want_disasm = true;
      } else if (!strcmp(argv[i], "--gpu") && i + 1 < argc) {
         gpu_id = strtoull(argv[++i], NULL, 0);
      } else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
         out = argv[++i];
      } else {
         fprintf(stderr,
                 "uso: %s [--addr <hex> | --nop] [--fau] [--gpu <hex>] [--out <file>]\n",
                 argv[0]);
         return 1;
      }
   }

   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, NULL, "main");
   nir_shader *s = b.shader;

   if (want_store) {
      nir_def *val = nir_imm_intN_t(&b, data, 32);
      nir_def *dst = nir_imm_int64(&b, (int64_t)addr);
      nir_store_global(&b, val, dst, .align_mul = 4);
   }

   nir_shader *done = nir_shader_clone(NULL, s);
   ralloc_free(s);
   done->options = pan_get_nir_shader_compiler_options(
      pan_arch(gpu_id), MESA_SHADER_COMPUTE, false);

   struct pan_compile_inputs inputs = {
      .gpu_id = gpu_id,
      .gpu_variant = 0x4000,
      .trust_varying_flat_highp_types = false,
   };
   inputs.fau.reserved = 0;
   /* --fau: deixa o endereco virar constante promovida (slot do uniforme) e
    * o binario referencia o SLOT (nao o valor). O descritor carrega o FAU
    * (uniform plane) em runtime via campos FAU/FAU count do Shader Env. */
   inputs.fau.promote_immediates = want_fau;

   struct pan_shader_info info;

   pan_preprocess_nir(done, gpu_id);
   pan_postprocess_nir(done, &inputs, &info);

   struct util_dynarray binary = UTIL_DYNARRAY_INIT;
   pan_shader_compile(done, &inputs, &binary, &info);

   check("compile produziu binario", binary.size > 0);

   const uint32_t *words = (const uint32_t *)binary.data;
   size_t n = binary.size / 4;

   if (want_fau) {
      printf("// FAU layout: max=%u reserved=%u count=%u\n",
             info.fau.max, info.fau.reserved, info.fau.count);
      for (unsigned i = info.fau.reserved; i < info.fau.count; i++) {
         bool k = BITSET_TEST(info.fau.is_const, i);
         if (k)
            printf("//   fau[%u] = const 0x%08x\n", i, info.fau.words[i].constant);
         else
            printf("//   fau[%u] = reloc ubo=%u offs=%u\n", i,
                   info.fau.words[i].relocation.ubo,
                   info.fau.words[i].relocation.offset);
      }
      printf("//   fau_count_byte_fld = %u (palavras 32-bit)\n", info.fau.count);
   }

   if (want_disasm) {
      printf("// disasm:\n");
      disassemble_valhall(stdout, binary.data, binary.size, true);
   }

   if (out) {
      FILE *f = fopen(out, "wb");
      if (!f) {
         fprintf(stderr, "nao abriu %s\n", out);
         return 1;
      }
      fwrite(binary.data, 1, binary.size, f);
      fclose(f);
   }

   printf("// shader binario Valhall arch=%u  %u bytes / %u words\n",
          (unsigned)pan_arch(gpu_id), (unsigned)binary.size, (unsigned)n);
   for (size_t i = 0; i < n; i++) {
      if (i % 4 == 0)
         printf("  ");
      printf("0x%08x,", words[i]);
      if (i % 4 == 3 || i + 1 == n)
         printf("\n");
   }

   return 0;
}