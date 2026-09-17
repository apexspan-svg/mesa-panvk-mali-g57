# WSI image-to-buffer: endereços confirmados no present

**Atualização: o bloqueio foi corrigido.** O compute JM v9 não preparava o
driver set com o dummy sampler. A cópia normal agora entrega os bytes corretos
ao SHM e a validação X11 passou nos 307200 pixels. Resultados completos em
`panvk-wsi-dummy-sampler-fix.md`. As observações abaixo registram o diagnóstico
anterior à correção.

Teste executado: `x11_clear`, Termux:X11 `DISPLAY=:0`, biblioteca de
`build-bionic`, com `PANVK_TRACE_WSI_COPY=1 PANVK_DEBUG=linear,trace`.
Nenhuma alteração de cache foi aplicada nesta investigação.

## Caminho confirmado no código

1. `src/vulkan/wsi/wsi_common.c`: `wsi_cmd_blit_image_to_buffer` grava
   `CmdCopyImageToBuffer(image->image, ..., image->blit.buffer, ...)`.
2. `src/panfrost/vulkan/panvk_vX_cmd_meta.c`:
   `panvk_per_arch(CmdCopyImageToBuffer2)` chama `vk_meta_copy_image_to_buffer`
   entre `meta_compute_start` e `meta_compute_end`.
3. `src/vulkan/runtime/vk_meta_copy_fill_update.c`:
   `copy_image_to_buffer_region` cria a view amostrada, envia o descritor para
   set 0 / binding 0, prepara os push constants e emite `CmdDispatch`.
4. `copy_buffer_image_prepare_compute_push_const` calcula `info.buf.addr`
   com `vk_meta_buffer_address`. O destino é um endereço nos push constants,
   não um storage-buffer descriptor.
5. `src/panfrost/vulkan/panvk_vX_image_view.c`: `prepare_tex_descs` chama
   `pan_sampled_texture_emit`. A instrumentação desempacota TEXTURE e
   GENERIC_PLANE no ponto de criação, usando os tipos gerados para v9.
6. `jm/panvk_vX_cmd_dispatch.c`: `cmd_dispatch` prepara resource table,
   sysvals/FAU e o payload do COMPUTE_JOB.

## Execução com instrumentação completa

Imagem 0:

```
WSI_COPY     SRC=0000007cd487c000 DST=0000007cd49a8000
COPY_TEXTURE image=0xb400007d6c47c400 view=0xb400007d6c58c200
             surfaces=0000007d70063040 payload_gpu=0000007d70063040
             SRC=0000007cd487c000 size=1228800 row_stride=2560 mip=0 layer=0
COPY_BIND    cmd=0xb400007d6c51b800 view=0xb400007d6c58c200 set=0 binding=0
COPY_PUSH    cmd=0xb400007d6c51b800 DST=0000007cd49a8000
             row_stride=2560 image_stride=1228800
             range=0,0,0..640,480,1 wg=10,480,1
```

O pandecode do compute submetido confirma a ligação final:

```
Resources resource table @7d7000c040
  Entry 1: Address=0x7d7000c020, Contains descriptors=true, Size=0x20
  Texture: RGBA8UI RGBA, 640x480, Surfaces=0x7d70063040
  Plane 0: Linear, RAW32, Pointer=0x7cd487c000
           Size=0x12c000, Row stride=0xa00
FAU @7d7000c080:
  u2 D49A8000 0000007C
  u3 00000A00 0012C000
  u7 00000280 000001E0
  u8 00000001 00000000
```

Portanto, o endereço usado para o destino no FAU é `0x7cd49a8000` e a
textura referenciada pela tabela de recursos aponta para `0x7cd487c000`.
Os endereços mudam entre processos; a comparação deve ser feita no mesmo run.

Depois do fragmento (átomo 2 DONE), o CSYNC existente reporta `ret=0` e a
origem contém `d9 33 14 ff` no começo e no centro. Depois do compute (átomo
1 DONE), CPU0/GPU0/CPUC/GPUC do SHM[0] continuam zero. O teste imprime
`vkQueuePresentKHR = 0`, mas isso não comprova exibição visual correta.

## Próxima fronteira de diagnóstico

`jm/panvk_vX_cmd_buffer.c:CmdPipelineBarrier2` apenas fecha/abre batches.
Seu comentário pressupõe flush/invalidate nas fronteiras de batch. É preciso
verificar essa garantia no backend kbase/kernel antes de mudar flags ou
introduzir jobs de cache. A confirmação dos endereços acima não prova que
o compute lê os writes recentes nem que suas stores chegam ao SHM.

Não foram alterados o patch WSI LINEAR, os external resources diagnósticos,
o fragmento/RT ou o caminho de reutilização de batch do vkmark.

## Artefatos

- `/data/user/0/com.termux/files/usr/tmp/opencode/wsi_copy_trace.log`
- `/data/user/0/com.termux/files/usr/tmp/opencode/wsi_descriptor_trace.log`
- `/data/user/0/com.termux/files/usr/tmp/opencode/pvktest/gfx/pandecode.dump..ctx-0.0000`

Build usado: `ninja -C build-bionic -j2 src/panfrost/vulkan/libvulkan_panfrost.so`.
Instalada somente `libvulkan_panfrost.so` em `$PREFIX/lib/`.
