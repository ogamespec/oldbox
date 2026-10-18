# 3dfx Voodoo Graphics (SST-1)

Реализация оригинальная, по `specs/voodoo-graphics-*.pdf`. Карта — PCI
`121a:0001`, class `0380`, BAR0 16 MiB. Окно делится на:

- `0x000000–0x3FFFFF` — 4 MiB регистров (алиасинг wrap/chip/register/byte);
- `0x400000–0x7FFFFF` — linear frame buffer;
- `0x800000–0xFFFFFF` — texture memory.

Поддерживается треугольный конвейер: vertex/start/delta регистры,
`triangleCMD`/`ftriangleCMD`, `fbzColorPath`, `fogMode`, `alphaMode`,
`fbzMode`, `lfbMode`, clip, fastfill, swapbuffer, fog table, chroma key,
stipple, счётчики. Растеризация — edge walking по плоскости, с
перспективно-корректными `s/w`, `t/w`, `1/w`, LOD/mip, nearest/bilinear,
clamp/wrap, RGB565/ARGB1555/ARGB4444/8-bit форматами, Z-тестом, alpha
test/blend и dithering. Вывод — XRGB8888 640×480.

Демонстрация:

```bash
./build/oldbox --demo voodoo --headless --screenshot voodoo.ppm
```

Регистровый доступ 32-битный; byte/word к регистрам отклоняются, LFB
поддерживает 16/32-битные packed-форматы. `load_texture_rom()` принимает
дамп, texture memory сохраняется между reset.

## Известные ограничения

- `tDetail` и часть texture color-combine (`tc_*`/`tca_*`) не применяются.
- NCC/YIQ decompression приближена по Y-таблице.
- Gamma CLUT/DAC, alpha planes/destination alpha, triple buffering,
  scan-line interleaving и точные 6-bit fog encoding не реализованы.
- Dither-матрицы — стандартный Bayer, не точные коэффициенты кремния.
- Swap выполняется сразу; отложенный vsync/swap interval не моделируется.

Эти ограничения не мешают проверять регистровую модель, геометрию,
текстурирование и формат кадра; они перечислены честно, а не спрятаны за
хост-рендерером.
