# PanVK v9 / kbase JM: triângulo offscreen validado

Data: 2026-09-14. Dispositivo reportado: Mali-G57 MC2, shader_present=0x5.

## Resultado observado nesta sessão

O teste Vulkan cria uma imagem RGBA8 linear 256×256, limpa para
(0.1, 0.2, 0.3, 1), desenha três vértices com cores RGB, espera a submissão,
invalida a memória mapeada e compara os pixels com interpolação baricêntrica.
Inclui barreira COLOR_ATTACHMENT_WRITE → HOST_READ.

- VTC: átomo 1, `core_req=0x16`, evento `0x01`.
- Fragmento: átomo 2, `core_req=0x01`, evento `0x01`.
- `vkQueueSubmit` e `vkWaitForFences`: `VK_SUCCESS`.
- Centro (128,128): RGB **127,65,63**.
- Fundo (8,8), (248,8), (8,248): RGB **26,51,76**.
- Pixels diferentes do fundo: **8192**.
- Comparação RGBA: **63008 pixels**, **zero divergências**, tolerância de
  3 unidades por componente. Uma faixa estreita das bordas é excluída para
  evitar ambiguidades nas regras de cobertura.
- Limpeza sem desenho: **65536 pixels**, **zero divergências**, job DONE.
- O teste final não usa PANVK_DEBUG=linear nem overrides de core_req.

## Causa e correções

### Uniforms do blend preparados antes do descritor

Em `src/panfrost/vulkan/jm/panvk_vX_cmd_draw.c`, `panvk_v9_draw` preparava
os sysvals e os push uniforms antes de chamar `panvk_v9_prepare_blend`.
`blend_emit_descs` preenche `gfx.fs.blend_descs`, usados pelo shader através
dos sysvals. No dump original, o BLEND consumia `u2`, mas `u2` era zero.

A preparação dos descritores de blend foi movida para antes dos sysvals e
uniforms. Isso mudou o fragmento de evento `0x04` para `0x01`, com cores e
geometria corretas. Não foi necessário misturar os jobs num único átomo ou
alterar o core_req do fragmento.

### Limpeza omitida sem desenho

Em `jm/panvk_vX_cmd_buffer.c`, o caminho v9 não emitia fragmento para um
framebuffer sem job de tiler. O caminho v9 já aloca um FBD em EndRendering;
agora esse framebuffer gera o job necessário para os loads/stores mesmo sem
draw. A condição anterior para v6/v7 foi preservada.

### Eventos e erros de submissão

Em `jm/panvk_vX_gpu_queue_kbase.c`, a espera agora acompanha os números dos
átomos do lote, sem drenar e descartar conclusões adicionais. Falhas,
timeouts e leituras inválidas chegam ao Vulkan como perda do dispositivo,
em vez de sinalizar sucesso. Antes da correção do blend, isso foi verificado
no aparelho: evento `0x04` produziu `vkQueueSubmit => -4`.

O job MALLOC_VERTEX também foi incluído em `batch->jobs`, para participar
da limpeza de status ao reutilizar o lote e dos diagnósticos de falha.

### Validação do teste

A validação antiga exigia soma RGB >400, incompatível com a interpolação
dos três vértices RGB (soma ~255), e consultava um pixel fora do triângulo.
Foi substituída por comparação geométrica/RGBA. O modo SKIP_DRAW tem sua
própria expectativa de fundo uniforme. O teste agora declara tiling LINEAR
explicitamente, seleciona memória compatível com memoryTypeBits e verifica
a invalidação da memória.

## Build e reprodução neste Termux

Fonte: `/data/user/0/com.termux/files/home/mesapvk/mesasrc`.

```sh
ninja -C build-bionic -j2 src/panfrost/vulkan/libvulkan_panfrost.so
```

Diretório do teste:
`/data/user/0/com.termux/files/usr/tmp/opencode/pvktest/gfx`.
Dentro dele:

```sh
clang panvk_gfx_test.c -o panvk_gfx_test -lvulkan -lm
VK_ICD_FILENAMES="$PWD/local_icd.json" ./panvk_gfx_test
python frame_to_png.py
SKIP_DRAW=1 VK_ICD_FILENAMES="$PWD/local_icd.json" ./panvk_gfx_test
```

Use um ambiente sem overrides PANVK de experimentos anteriores. O ICD local
aponta para a biblioteca de `build-bionic`; a biblioteca instalada globalmente
no Termux não foi substituída nesta sessão.

`frame.png` contém o triângulo validado. `frame.raw` é sobrescrito em cada
execução e, após o último teste de limpeza, contém o fundo uniforme.

O resultado comprova esse desenho offscreen com readback; apresentação em
janela/swapchain ainda não foi validada aqui.
