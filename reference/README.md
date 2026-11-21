# Справочные материалы

Чужой код для чтения и как основа. Наш драйвер живёт вне этого каталога.

| Каталог | Что внутри | Откуда | Лицензия | Зачем |
|---|---|---|---|---|
| `mesa-7.11-mach64/` | 3D-драйвер mach64 из Mesa 7.11 | `mesa_7.11.orig.tar.gz` (Ubuntu old-releases), `src/mesa/drivers/dri/mach64` | MIT | главный источник по 3D: регистры, формат вершин, состояния, текстуры |
| `mach64-drm/` (submodule) | модуль ядра DRM для mach64 | github.com/trya/mach64-drm, `0f76ec0` | MIT (заголовки файлов) | DMA: пакеты, буферы, прерывание |
| `xf86-video-mach64-6.9.5/` | драйвер X.org | `xserver-xorg-video-mach64_6.9.5.orig.tar.gz` (Ubuntu) | MIT, см. `COPYING` | режимы, LCD-панели, PLL, инициализация DRI (`atidri.c`) |
| `linux-atyfb/` | драйвер кадрового буфера Linux, только файлы mach64 | torvalds/linux `7b7fe30`, `drivers/video/fbdev/aty`, `include/video/mach64.h` | GPL-2.0 | режимы и LCD на Mobility; **только читать, код не копировать** |
| `vmdisp9x/` (submodule) | открытый драйвер дисплея Win9x под Open Watcom | github.com/JHRobotics/vmdisp9x, `4bf7e8a` | MIT | основа для `.drv` и `.vxd`, режимы через VBE, заголовки DDK |
| `vmhal9x/` (submodule) | открытый HAL DirectDraw/Direct3D для Win9x | github.com/JHRobotics/vmhal9x, `c018826` | MIT | каркас 32-битного HAL, разбор DrawPrimitives2 |
| `proprietary/` | закрытые файлы, в git не попадают | см. `proprietary/README.md` | — | штатный драйвер, DDK, документация ATI |

Контрольные суммы архивов (SHA-256):

- `mesa_7.11.orig.tar.gz` — `071a675d5631c47b9e6870a8b54eaf0c15702ccc1d5073a6103a2fe3d87c8f50`
- `xserver-xorg-video-mach64_6.9.5.orig.tar.gz` — `38672f863f4a08c20f30c248555e12dca38fc25793af5214b9e229e6c4fa460d`

После клонирования: `git submodule update --init`.

## С чего начинать чтение

- Список чипов и флаги Mobility: `linux-atyfb/atyfb_base.c` (`ATI_CHIP_MOBILITY`).
- Регистры: `linux-atyfb/mach64.h`, `mach64-drm/mach64_drv.h`.
- Подача вершин и состояния: `mesa-7.11-mach64/mach64_tris.c`,
  `mach64_state.c`, `mach64_ioctl.h`.
- Текстуры: `mesa-7.11-mach64/mach64_tex.c`, `mach64_texmem.c`.
- DMA: `mach64-drm/mach64_dma.c`.
