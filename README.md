# Mesa Panfrost mali_kbase Backend

> Backend experimental para rodar Mesa Panfrost/PanVK em kernels Android OEM que usam `mali_kbase` em vez do driver upstream Panfrost/Panthor.

---

## O que é isso?

O Mesa Panfrost normalmente só funciona em kernels com o driver **Panfrost/Panthor** (upstream Linux mainline). A grande maioria dos celulares Android com GPU Mali usa o driver proprietário da Arm chamado **mali_kbase**, que tem uma interface completamente diferente.

Este projeto adiciona um novo backend `kbase_kmod` ao Mesa que permite o Panfrost/PanVK funcionar diretamente com o `mali_kbase`, sem precisar de kernel mainline.

---

## Dispositivos suportados

| GPU | Arquitetura | SoCs comuns |
|-----|-------------|-------------|
| Mali-G52 | Bifrost | Helio G85, G90T, Dimensity 700 |
| Mali-G57 | Bifrost/Valhall | Dimensity 800, 900 |
| Mali-G72 | Bifrost | Helio P60, P70 |
| Mali-G76 | Bifrost | Kirin 980, Exynos 9820 |
| Mali-G31 | Bifrost | Helio A22, A25 |
| Mali-G51 | Bifrost | Helio P23, P30 |

**Requisitos:**
- Android API 26+
- Kernel OEM com `mali_kbase` (típico em MediaTek, Samsung, Xiaomi)
- Termux para compilar

---

## Arquivos do patch

| Arquivo | Descrição |
|---------|-----------|
| `kbase_kmod.c` | Backend completo mali_kbase |
| `kbase_kmod.h` | Header público com declarações |
| `pan_kmod.c` | Modificado para detectar `/dev/mali0` automaticamente |
| `meson.build` | Integração na build do Mesa |

---

## Como aplicar no Mesa

### Mesa 24.3.4 (recomendado)

```bash
# Baixar o Mesa
wget https://archive.mesa3d.org/mesa-24.3.4.tar.xz
tar -xJf mesa-24.3.4.tar.xz
cd mesa-24.3.4

# Aplicar o patch
cp kbase_kmod.c src/panfrost/lib/kmod/
cp kbase_kmod.h src/panfrost/lib/kmod/
cp pan_kmod.c   src/panfrost/lib/kmod/
cp meson.build  src/panfrost/lib/kmod/
```

### Mesa 26.0.2

```bash
wget https://archive.mesa3d.org/mesa-26.0.2.tar.xz
tar -xJf mesa-26.0.2.tar.xz
cd mesa-26.0.2

cp kbase_kmod.c src/panfrost/lib/kmod/
cp kbase_kmod.h src/panfrost/lib/kmod/
cp pan_kmod.c   src/panfrost/lib/kmod/
cp meson.build  src/panfrost/lib/kmod/
```

---

## Como compilar no Termux (Android)

```bash
# Instalar dependências
pkg install clang cmake ninja python meson libdrm zstd

# Configurar build
meson setup build \
  -Dgallium-drivers=panfrost \
  -Dvulkan-drivers=panfrost \
  -Dplatforms=android \
  -Dbuildtype=release \
  -Dplatform-sdk-version=26

# Compilar
ninja -C build
```

**Arquivos gerados:**
- `build/src/vulkan/panfrost/libvulkan_panfrost.so` — Vulkan ICD
- `build/src/gallium/targets/dri/libgallium_dri.so` — OpenGL ES
- `build/src/egl/libEGL.so` — EGL

---

## O que foi implementado

### 1. bo_wait real
Usa `poll()` no fd do `/dev/mali0` + `KBASE_IOCTL_EVENT_DEQUEUE` para esperar conclusão de jobs GPU. Antes era um stub que retornava imediatamente.

### 2. Job submission (JM)
Submete jobs via `KBASE_IOCTL_JOB_SUBMIT` com tracking de atoms para sincronização correta com `bo_wait`.

### 3. bo_export (dma-buf)
Exporta buffers como dma-buf via `KBASE_IOCTL_MEM_FLAGS_CHANGE` + `KBASE_IOCTL_MEM_SHARE` (DDK r38p0+).

### 4. Eviction hints
`bo_make_evictable` e `bo_make_unevictable` via `BASE_MEM_DONT_NEED` no `KBASE_IOCTL_MEM_FLAGS_CHANGE`.

### 5. Detecção automática
`pan_kmod_dev_create()` agora detecta automaticamente quando está abrindo `/dev/mali0` (drmGetVersion falha) e usa o backend kbase diretamente.

---

## Diferença para o Panfrost upstream

```
Panfrost upstream:          Este patch:
/dev/dri/renderD128    →    /dev/mali0
DRM ioctls             →    KBASE_IOCTL_* (mali_kbase)
Kernel mainline        →    Kernel OEM Android
```

---

## Limitações conhecidas

- **PanVK (Vulkan)**: Features como `multiViewport`, `wideLines`, `dualSrcBlend` não são suportadas pelo hardware Mali-G52 Bifrost — o driver reporta corretamente que não suporta.
- **bo_export**: Requer DDK r38p0 ou superior para `KBASE_IOCTL_MEM_SHARE`.
- **CSF (Mali-G710+)**: Não testado, apenas JM (Job Manager) foi implementado.
- **Sync**: Sem suporte a DRM syncobj — usa mecanismo interno do kbase.

---

## Testado em

| Aparelho | SoC | GPU | Android | Mesa |
|----------|-----|-----|---------|------|
| Xiaomi Redmi Note 9 | Helio G85 | Mali-G52 | API 30 | 24.3.4 |

---

## Contribuindo

Pull Requests são bem-vindos! Especialmente para:
- Suporte a CSF (Panthor-style, Mali-G710+)
- Melhorias no job submission
- Testes em outros dispositivos
- Eviction hints em DDKs mais antigos

---

## Relação com o Mesa upstream

Este patch foi desenvolvido de forma independente com o objetivo de eventualmente ser submetido ao Mesa upstream como Merge Request no [GitLab do Mesa](https://gitlab.freedesktop.org/mesa/mesa).

A interface `pan_kmod` já foi projetada pelos mantenedores do Panfrost (Collabora) para suportar múltiplos backends — este patch aproveita exatamente essa arquitetura.

---

## Licença

MIT — mesma licença do Mesa.

---

## Créditos

- Desenvolvido com base na arquitetura `pan_kmod` do Mesa (Collabora)
- Referência de ioctls: `mali_kbase_ioctl.h` (Arm DDK público)
- Inspirado pelo trabalho do driver **Lima** (Mali-400) e **Turnip** (Adreno)
