# PanVK v9 JM: cópia WSI e apresentação X11 validadas

## Causa encontrada

`src/panfrost/vulkan/jm/panvk_vX_cmd_dispatch.c` preparava os push descriptors
e a resource table, mas não o driver set reservado na tabela 0.

Em `panvk_vX_nir_lower_descriptors.c`, um texture fetch sem sampler da
aplicação usa `desc_info.dummy_sampler_handle`. Para compute v9, esse handle
é tabela 0, descritor 0. O dump anterior tinha essa entrada vazia, embora
SRC na textura e DST no FAU estivessem corretos.

O caminho CSF tem `prepare_driver_set`, que emite o dummy sampler e os
dynamic buffers. A mesma preparação foi adicionada ao caminho JM v9,
antes de `cmd_prepare_shader_res_table`. A correção não altera flags de
cache ou shaders da cópia.

## Experimentos e resultados

1. **Cópia original:** SRC correto e colorido; DST correto, mas zerado após DONE.
2. **Store constante:** mantendo dispatch, bounds e destino, substituímos
   temporariamente o resultado da leitura por uma constante. O shader escreveu
   `5a 00 00 00` tanto no começo quanto no centro do USER_BUFFER. Esse probe
   foi removido após a execução.
3. **Cópia por textura com driver set:** sem constant probe, CPU0/GPU0/CPUC/GPUC
   do SHM passaram a conter `d9 33 14 ff`, igual à origem.
4. **Raw load solicitado:** com `PANVK_COPY_PROBE_RAW=1`, a imagem LINEAR
   foi exposta internamente como um buffer por endereço, sem nova alocação,
   e copiada por `vk_meta_copy_buffer`. O dump confirmou `LOAD.i128` e
   `STORE.i128`, sem `TEX`. O destino recebeu `d9 33 14 ff` no início e centro.
   O experimento usa outro kernel de cópia; comprova leitura global do mesmo
   endereço, não identidade de todas as condições do shader de textura.
5. **Apresentação normal verificada no servidor X11:** com todos os probes
   desligados, `XGetImage` leu os pixels da própria janela após o present:

```
X11 screen: 800x600
vkQueuePresentKHR = 0
X11_READBACK pixels=307200 mismatches=0 => PASS
```

A imagem apresentada é uma limpeza azul 640×480, RGB esperado `(20,51,217)`.
A comparação verifica todos os pixels com tolerância de 2 por componente.
Uma primeira captura apresentou áreas encobertas por outra janela. Para
evitar essa interferência, o teste agora cria uma janela override-redirect
no canto superior esquerdo e a eleva antes do readback.

O resultado não depende apenas de `vkQueuePresentKHR=0`: há bytes corretos
no SHM e confirmação dos pixels retornados pelo servidor X11.

## Escopo

- Mantidos os patches existentes WSI LINEAR e external resources diagnósticos.
- Mantido o CSYNC diagnóstico existente que lê os BOs nativos após fragmento.
- Nenhum patch novo de cache foi necessário para este resultado.
- A falha separada do vkmark/reutilização de batch não foi investigada aqui.
- Raw probe é opt-in e limitado a região LINEAR 32bpp, tightly packed,
  mip/layer zero, sem offset de imagem. O caminho normal continua por textura.
- Há probes anteriores do usuário (`PANVK_PROBE_USERBUF_FILL` e
  `PANVK_COPY_PROBE_RAW_TEXEL`); ambos estavam desligados na validação final.

## Arquivos e reprodução

Fonte corrigida: `src/panfrost/vulkan/jm/panvk_vX_cmd_dispatch.c`.

Build e instalação, no diretório Mesa:

```sh
ninja -C build-bionic -j2 src/panfrost/vulkan/libvulkan_panfrost.so
cp -f build-bionic/src/panfrost/vulkan/libvulkan_panfrost.so "$PREFIX/lib/libvulkan_panfrost.so"
```

No diretório `/data/user/0/com.termux/files/usr/tmp/opencode/pvktest/gfx`:

```sh
clang x11_clear.c -o x11_clear_verified -lvulkan -lX11
env -u PANVK_COPY_PROBE_RAW -u PANVK_COPY_PROBE_RAW_TEXEL \
    -u PANVK_PROBE_USERBUF_FILL -u PANVK_COPY_PROBE_CONSTANT \
    DISPLAY=:0 PANVK_DEBUG=linear \
    VK_ICD_FILENAMES="$PWD/local_icd.json" ./x11_clear_verified
python window_to_png.py
```

Logs em `/data/user/0/com.termux/files/usr/tmp/opencode/`:

- `wsi_constant_probe.log`
- `wsi_sampler_fix.log`
- `wsi_raw_load_probe.log`
- `wsi_x11_verified.log` (primeira captura com oclusão)
- `wsi_x11_unobscured.log` (validação final PASS)

Imagem extraída da janela: `pvktest/gfx/wsi-window.png`.
