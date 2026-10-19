#include "png.h"

#include <stdlib.h>
#include <string.h>

#include "qrcodegen.h"

void BufInit(Buffer *b) {
    b->data = NULL;
    b->size = 0;
    b->cap = 0;
    b->failed = false;
}

void BufFree(Buffer *b) {
    free(b->data);
    BufInit(b);
}

static bool BufReserve(Buffer *b, size_t extra) {
    if (b->failed)
        return false;
    if (b->size + extra <= b->cap)
        return true;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->size + extra)
        cap *= 2;
    uint8_t *p = (uint8_t *)realloc(b->data, cap);
    if (!p) {
        b->failed = true;
        return false;
    }
    b->data = p;
    b->cap = cap;
    return true;
}

void BufPut(Buffer *b, const void *p, size_t n) {
    if (!BufReserve(b, n))
        return;
    memcpy(b->data + b->size, p, n);
    b->size += n;
}

static void BufByte(Buffer *b, uint8_t v) {
    BufPut(b, &v, 1);
}

static void BufBE32(Buffer *b, uint32_t v) {
    uint8_t t[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    BufPut(b, t, 4);
}

static uint32_t Crc32(const uint8_t *p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static uint32_t Adler32(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static void PutChunk(Buffer *out, const char *type, const uint8_t *data, size_t len) {
    BufBE32(out, (uint32_t)len);
    size_t start = out->size;
    BufPut(out, type, 4);
    BufPut(out, data, len);
    BufBE32(out, out->failed ? 0 : Crc32(out->data + start, len + 4));
}

// zlib-поток из несжатых (stored) блоков deflate: картинка и так маленькая,
// а без компрессора нечему ломаться.
static void ZlibStored(const uint8_t *data, size_t n, Buffer *out) {
    BufByte(out, 0x78);
    BufByte(out, 0x01);
    size_t pos = 0;
    do {
        size_t len = n - pos > 65535 ? 65535 : n - pos;
        bool last = pos + len == n;
        uint8_t hdr[5] = {(uint8_t)(last ? 1 : 0), (uint8_t)len, (uint8_t)(len >> 8),
                          (uint8_t)~len, (uint8_t)(~len >> 8)};
        BufPut(out, hdr, 5);
        BufPut(out, data + pos, len);
        pos += len;
    } while (pos < n);
    BufBE32(out, Adler32(data, n));
}

bool RenderQrPng(const uint8_t qrcode[], int scale, int border, Buffer *out) {
    int modules = qrcodegen_getSize(qrcode) + 2 * border;
    int side = modules * scale;
    // Обычный truecolor RGB, 8 бит на канал. Монохромный PNG (1 бит, оттенки серого)
    // был бы в 24 раза меньше, но 1С:Предприятие 8.1 показывает его пустой картинкой,
    // хотя файл корректный и открывается в Windows.
    size_t rowBytes = (size_t)side * 3;
    size_t stride = rowBytes + 1;  // байт фильтра PNG перед каждой строкой

    Buffer raw;
    BufInit(&raw);
    if (!BufReserve(&raw, stride * side)) {
        out->failed = true;
        return false;
    }
    raw.size = stride * side;

    // Каждая строка модулей рисуется один раз и копируется scale раз.
    // Белый — FF FF FF, чёрный — 00 00 00.
    for (int my = 0; my < modules; my++) {
        uint8_t *row = raw.data + (size_t)my * scale * stride;
        memset(row, 0xFF, stride);
        row[0] = 0;
        for (int px = 0; px < side; px++) {
            if (qrcodegen_getModule(qrcode, px / scale - border, my - border))
                memset(row + 1 + (size_t)px * 3, 0x00, 3);
        }
        for (int k = 1; k < scale; k++)
            memcpy(row + k * stride, row, stride);
    }

    static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    uint8_t ihdr[13] = {
        (uint8_t)(side >> 24), (uint8_t)(side >> 16), (uint8_t)(side >> 8), (uint8_t)side,
        (uint8_t)(side >> 24), (uint8_t)(side >> 16), (uint8_t)(side >> 8), (uint8_t)side,
        8,  // бит на канал
        2,  // truecolor RGB
        0, 0, 0};

    // Размер zlib-потока известен заранее: заголовок 2 байта, по 5 байт на каждый
    // stored-блок до 65535 байт, Adler-32 4 байта. Резервируем сразу, без удвоений.
    size_t blocks = raw.size / 65535 + 1;
    size_t zSize = 2 + raw.size + 5 * blocks + 4;
    Buffer z;
    BufInit(&z);
    BufReserve(&z, zSize);
    ZlibStored(raw.data, raw.size, &z);
    BufFree(&raw);

    BufReserve(out, sizeof kSignature + (12 + sizeof ihdr) + (12 + z.size) + 12);
    BufPut(out, kSignature, sizeof kSignature);
    PutChunk(out, "IHDR", ihdr, sizeof ihdr);
    PutChunk(out, "IDAT", z.data, z.size);
    PutChunk(out, "IEND", NULL, 0);
    bool ok = !z.failed && !out->failed;
    BufFree(&z);
    return ok;
}

size_t Base64Length(size_t n) {
    return (n + 2) / 3 * 4;
}

void Base64Encode(const uint8_t *data, size_t n, wchar_t *out) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16 | (uint32_t)data[i + 1] << 8 | data[i + 2];
        *out++ = kAlphabet[v >> 18];
        *out++ = kAlphabet[(v >> 12) & 63];
        *out++ = kAlphabet[(v >> 6) & 63];
        *out++ = kAlphabet[v & 63];
    }
    if (i < n) {
        uint32_t v = (uint32_t)data[i] << 16 | (i + 1 < n ? (uint32_t)data[i + 1] << 8 : 0);
        *out++ = kAlphabet[v >> 18];
        *out++ = kAlphabet[(v >> 12) & 63];
        *out++ = i + 1 < n ? kAlphabet[(v >> 6) & 63] : L'=';
        *out++ = L'=';
    }
}
