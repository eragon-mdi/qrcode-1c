// Растеризация QR-кода в монохромный PNG и base64, без внешних библиотек.
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
const int kMaxImageSide = 8192;

// Рисует QR-код (буфер qrcodegen) в PNG 1 бит/пиксель:
// scale пикселей на модуль, border модулей белого поля с каждой стороны.
bool RenderQrPng(const uint8_t qrcode[], int scale, int border, Buffer *out);

// Длина base64-представления n байт (с выравниванием '=').
size_t Base64Length(size_t n);
// Пишет base64 от data в out (ровно Base64Length(n) символов, без завершающего нуля).
void Base64Encode(const uint8_t *data, size_t n, wchar_t *out);
