// Растеризация QR-кода в PNG (RGB, 8 бит на канал) и base64, без внешних библиотек.
#pragma once

#include <stddef.h>
#include <stdint.h>

// Растущий байтовый буфер. При нехватке памяти выставляет failed и дальше ничего не пишет.
struct Buffer {
    uint8_t *data;
    size_t size;
    size_t cap;
    bool failed;
};

void BufInit(Buffer *b);
void BufFree(Buffer *b);
void BufPut(Buffer *b, const void *p, size_t n);

// Самая большая сторона картинки в пикселях, которую согласны рисовать.
// RGB без сжатия: 4096 px — около 50 МБ на картинку, это ещё влезает в 32-битный процесс 1С.
// Самый большой QR (версия 40) с полем 4 — это 185 модулей, то есть до 22 px на модуль.
const int kMaxImageSide = 4096;

// Рисует QR-код (буфер qrcodegen) в PNG RGB 8 бит на канал:
// scale пикселей на модуль, border модулей белого поля с каждой стороны.
bool RenderQrPng(const uint8_t qrcode[], int scale, int border, Buffer *out);

// Длина base64-представления n байт (с выравниванием '=').
size_t Base64Length(size_t n);
// Пишет base64 от data в out (ровно Base64Length(n) символов, без завершающего нуля).
void Base64Encode(const uint8_t *data, size_t n, wchar_t *out);
