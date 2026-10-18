// QRCode1C — внешняя компонента 1С:Предприятия 8.1 (технология COM) для генерации QR-кодов.
//
// В 1С: ЗагрузитьВнешнююКомпоненту(ИмяФайла) вызывает DllRegisterServer, которая
// регистрирует компоненту в HKCU (права администратора не нужны), затем
// Новый("AddIn.QRCode1C") создаёт объект с методами и свойствами ниже.

#include <windows.h>
#include <ole2.h>
#include <olectl.h>
#include <shlwapi.h>
#include <stdlib.h>

#include <new>

#include "addin.h"
#include "png.h"
#include "qrcodegen.h"
#include "version.h"

#define WIDEN2(s) L##s
#define WIDEN(s) WIDEN2(s)

// {FE7F2501-F9F2-4B32-AA3F-50DD8C65BDF9}
static const CLSID CLSID_QRCode1C =
    {0xFE7F2501, 0xF9F2, 0x4B32, {0xAA, 0x3F, 0x50, 0xDD, 0x8C, 0x65, 0xBD, 0xF9}};
static const wchar_t kClsid[] = L"{FE7F2501-F9F2-4B32-AA3F-50DD8C65BDF9}";
// Библиотека типов, зашитая в ресурсы DLL (qrcode1c.idl).
static const wchar_t kLibid[] = L"{E6289C53-FA61-4380-8D3E-DE91503BCB7D}";
static const wchar_t kProgId[] = L"AddIn.QRCode1C";
static const wchar_t kExtensionName[] = L"QRCode1C";

// DLL собирается без C++-рантайма (libstdc++): он тянет за собой импорт GetThreadId,
// которого нет в Windows XP. Поэтому объекты создаются через malloc, а заглушку для
// чисто виртуальных методов определяем сами.
extern "C" void __cxa_pure_virtual() {
    abort();
}

static HMODULE g_module;
static LONG g_objects;
static LONG g_locks;

struct Name {
    const wchar_t *en;
    const wchar_t *ru;
};

enum { PROP_VERSION, PROP_ERROR, PROP_ENCODING, PROP_COUNT };
static const Name kProps[PROP_COUNT] = {
    {L"Version", L"Версия"},
    {L"ErrorDescription", L"ОписаниеОшибки"},
    {L"Encoding", L"Кодировка"},
};

enum { METH_PNG_FILE, METH_PNG_BASE64, METH_COUNT };
static const Name kMethods[METH_COUNT] = {
    {L"GeneratePNG", L"СформироватьPNG"},
    {L"GeneratePNGBase64", L"СформироватьPNGBase64"},
};
// СформироватьPNG(Текст, ИмяФайла, РазмерМодуля = 4, Поле = 4, УровеньКоррекции = "M") -> Булево
// СформироватьPNGBase64(Текст, РазмерМодуля = 4, Поле = 4, УровеньКоррекции = "M") -> Строка
static const long kMethodParams[METH_COUNT] = {5, 4};

static const int kDefaultScale = 4;
static const int kDefaultBorder = 4;  // стандарт QR требует белое поле не меньше 4 модулей
static const wchar_t kDefaultLevel[] = L"M";
static const wchar_t kTooLong[] =
    L"Текст не помещается в QR-код: сократите его или выберите УровеньКоррекции \"L\"";

static long FindName(const Name *names, long count, const wchar_t *name) {
    if (!name)
        return -1;
    for (long i = 0; i < count; i++)
        if (lstrcmpiW(name, names[i].en) == 0 || lstrcmpiW(name, names[i].ru) == 0)
            return i;
    return -1;
}

static HRESULT GetName(const Name *names, long count, long num, long alias, BSTR *out) {
    if (!out)
        return E_POINTER;
    if (num < 0 || num >= count)
        return S_FALSE;
    *out = SysAllocString(alias == 0 ? names[num].en : names[num].ru);
    return *out ? S_OK : E_OUTOFMEMORY;
}

// Склеивает строки в dst, не выходя за cap символов.
static void Concat(wchar_t *dst, size_t cap, const wchar_t *a, const wchar_t *b = L"",
                   const wchar_t *c = L"", const wchar_t *d = L"") {
    const wchar_t *parts[4] = {a, b, c, d};
    size_t n = 0;
    for (int i = 0; i < 4; i++)
        for (const wchar_t *p = parts[i]; *p && n + 1 < cap; p++)
            dst[n++] = *p;
    dst[n] = 0;
}

// Достаёт i-й параметр вызова. Непереданный параметр остаётся VT_EMPTY.
static void GetParam(SAFEARRAY *params, long i, VARIANT *out) {
    VariantInit(out);
    long lb = 0, ub = -1;
    if (!params || FAILED(SafeArrayGetLBound(params, 1, &lb)) ||
        FAILED(SafeArrayGetUBound(params, 1, &ub)))
        return;
    long index = lb + i;
    if (index > ub)
        return;
    VARIANT v;
    VariantInit(&v);
    if (SUCCEEDED(SafeArrayGetElement(params, &index, &v))) {
        VariantCopyInd(out, &v);
        VariantClear(&v);
    }
}

static bool IsMissing(const VARIANT *v) {
    return V_VT(v) == VT_EMPTY || V_VT(v) == VT_NULL ||
           (V_VT(v) == VT_ERROR && V_ERROR(v) == DISP_E_PARAMNOTFOUND);
}

// Приводит значение к типу vt (со снятием VT_BYREF); dst надо очистить VariantClear.
static bool ToType(const VARIANT *src, VARTYPE vt, VARIANT *dst) {
    VariantInit(dst);
    if (FAILED(VariantCopyInd(dst, const_cast<VARIANT *>(src))))
        return false;
    if (FAILED(VariantChangeType(dst, dst, 0, vt))) {
        VariantClear(dst);
        return false;
    }
    return true;
}

static UINT ParseCodepage(const wchar_t *s) {
    static const wchar_t *const kUtf8[] = {L"UTF-8", L"UTF8", L"65001"};
    static const wchar_t *const k1251[] = {L"windows-1251", L"cp1251", L"1251"};
    for (size_t i = 0; i < sizeof kUtf8 / sizeof kUtf8[0]; i++)
        if (lstrcmpiW(s, kUtf8[i]) == 0)
            return CP_UTF8;
    for (size_t i = 0; i < sizeof k1251 / sizeof k1251[0]; i++)
        if (lstrcmpiW(s, k1251[i]) == 0)
            return 1251;
    return 0;
}

static DWORD WriteWholeFile(const wchar_t *path, const uint8_t *data, size_t size) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError();
    DWORD written = 0;
    DWORD err = 0;
    if (!WriteFile(h, data, (DWORD)size, &written, NULL))
        err = GetLastError();
    else if (written != size)
        err = ERROR_WRITE_FAULT;
    CloseHandle(h);
    if (err)
        DeleteFileW(path);
    return err;
}

class QRCode1C final : public IInitDone, public ILanguageExtender {
public:
    QRCode1C() : m_refs(1), m_codepage(CP_UTF8) {
        m_error[0] = 0;
        InterlockedIncrement(&g_objects);
    }

    // IUnknown

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) {
        if (!ppv)
            return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IInitDone)) {
            *ppv = static_cast<IInitDone *>(this);
        } else if (IsEqualIID(riid, IID_ILanguageExtender)) {
            *ppv = static_cast<ILanguageExtender *>(this);
        } else {
            *ppv = NULL;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    STDMETHODIMP_(ULONG) AddRef() {
        return InterlockedIncrement(&m_refs);
    }

    STDMETHODIMP_(ULONG) Release() {
        LONG refs = InterlockedDecrement(&m_refs);
        if (refs == 0) {
            this->~QRCode1C();
            free(this);
        }
        return refs;
    }

    // IInitDone

    STDMETHODIMP Init(IDispatch *) {
        return S_OK;
    }

    STDMETHODIMP Done() {
        return S_OK;
    }

    STDMETHODIMP GetInfo(SAFEARRAY **info) {
        // Версия технологии 2.0: можно создавать несколько объектов AddIn.QRCode1C.
        if (info && *info) {
            long index = 0;
            VARIANT v;
            VariantInit(&v);
            V_VT(&v) = VT_I4;
            V_I4(&v) = 2000;
            SafeArrayPutElement(*info, &index, &v);
        }
        return S_OK;
    }

    // ILanguageExtender: свойства

    STDMETHODIMP RegisterExtensionAs(BSTR *name) {
        if (!name)
            return E_POINTER;
        *name = SysAllocString(kExtensionName);
        return *name ? S_OK : E_OUTOFMEMORY;
    }

    STDMETHODIMP GetNProps(long *count) {
        if (!count)
            return E_POINTER;
        *count = PROP_COUNT;
        return S_OK;
    }

    STDMETHODIMP FindProp(BSTR name, long *num) {
        if (!num)
            return E_POINTER;
        *num = FindName(kProps, PROP_COUNT, name);
        return *num < 0 ? S_FALSE : S_OK;
    }

    STDMETHODIMP GetPropName(long num, long alias, BSTR *name) {
        return GetName(kProps, PROP_COUNT, num, alias, name);
    }

    STDMETHODIMP GetPropVal(long num, VARIANT *value) {
        if (!value)
            return E_POINTER;
        const wchar_t *s;
        switch (num) {
        case PROP_VERSION:
            s = WIDEN(QRCODE1C_VERSION_STR);
            break;
        case PROP_ERROR:
            s = m_error;
            break;
        case PROP_ENCODING:
            s = m_codepage == CP_UTF8 ? L"UTF-8" : L"windows-1251";
            break;
        default:
            return S_FALSE;
        }
        V_VT(value) = VT_BSTR;
        V_BSTR(value) = SysAllocString(s);
        return S_OK;
    }

    STDMETHODIMP SetPropVal(long num, VARIANT *value) {
        if (num != PROP_ENCODING)
            return S_FALSE;
        VARIANT s;
        UINT codepage = 0;
        if (value && ToType(value, VT_BSTR, &s)) {
            codepage = ParseCodepage(V_BSTR(&s));
            VariantClear(&s);
        }
        if (!codepage) {
            SetError(L"Кодировка должна быть \"UTF-8\" или \"windows-1251\"");
            return E_FAIL;
        }
        m_codepage = codepage;
        return S_OK;
    }

    STDMETHODIMP IsPropReadable(long num, BOOL *readable) {
        if (!readable)
            return E_POINTER;
        if (num < 0 || num >= PROP_COUNT)
            return S_FALSE;
        *readable = TRUE;
        return S_OK;
    }

    STDMETHODIMP IsPropWritable(long num, BOOL *writable) {
        if (!writable)
            return E_POINTER;
        if (num < 0 || num >= PROP_COUNT)
            return S_FALSE;
        *writable = num == PROP_ENCODING;
        return S_OK;
    }

    // ILanguageExtender: методы

    STDMETHODIMP GetNMethods(long *count) {
        if (!count)
            return E_POINTER;
        *count = METH_COUNT;
        return S_OK;
    }

    STDMETHODIMP FindMethod(BSTR name, long *num) {
        if (!num)
            return E_POINTER;
        *num = FindName(kMethods, METH_COUNT, name);
        return *num < 0 ? S_FALSE : S_OK;
    }

    STDMETHODIMP GetMethodName(long num, long alias, BSTR *name) {
        return GetName(kMethods, METH_COUNT, num, alias, name);
    }

    STDMETHODIMP GetNParams(long num, long *count) {
        if (!count)
            return E_POINTER;
        if (num < 0 || num >= METH_COUNT) {
            *count = 0;
            return S_FALSE;
        }
        *count = kMethodParams[num];
        return S_OK;
    }

    STDMETHODIMP GetParamDefValue(long method, long param, VARIANT *value) {
        if (!value)
            return E_POINTER;
        VariantInit(value);
        if (method < 0 || method >= METH_COUNT || param < 0 || param >= kMethodParams[method])
            return S_FALSE;
        // Обязательные параметры: Текст, а у СформироватьPNG ещё ИмяФайла.
        switch (param - FirstOption(method)) {
        case 0:
            V_VT(value) = VT_I4;
            V_I4(value) = kDefaultScale;
            break;
        case 1:
            V_VT(value) = VT_I4;
            V_I4(value) = kDefaultBorder;
            break;
        case 2:
            V_VT(value) = VT_BSTR;
            V_BSTR(value) = SysAllocString(kDefaultLevel);
            break;
        }
        return S_OK;
    }

    STDMETHODIMP HasRetVal(long method, BOOL *has) {
        if (!has)
            return E_POINTER;
        if (method < 0 || method >= METH_COUNT)
            return S_FALSE;
        *has = TRUE;
        return S_OK;
    }

    STDMETHODIMP CallAsProc(long method, SAFEARRAY **params) {
        VARIANT ret;
        VariantInit(&ret);
        HRESULT hr = CallAsFunc(method, &ret, params);
        VariantClear(&ret);
        return hr;
    }

    STDMETHODIMP CallAsFunc(long method, VARIANT *ret, SAFEARRAY **params) {
        if (!ret)
            return E_POINTER;
        SAFEARRAY *args = params ? *params : NULL;
        m_error[0] = 0;
        Buffer png;
        BufInit(&png);

        switch (method) {
        case METH_PNG_FILE: {
            BSTR path = NULL;
            bool ok = GetString(args, 1, L"ИмяФайла", &path) && Generate(args, method, &png);
            if (ok) {
                DWORD err = WriteWholeFile(path, png.data, png.size);
                if (err) {
                    SetSystemError(L"Не удалось записать файл ", path, err);
                    ok = false;
                }
            }
            SysFreeString(path);
            V_VT(ret) = VT_BOOL;
            V_BOOL(ret) = ok ? VARIANT_TRUE : VARIANT_FALSE;
            break;
        }
        case METH_PNG_BASE64: {
            BSTR result = NULL;
            if (Generate(args, method, &png)) {
                result = SysAllocStringLen(NULL, (UINT)Base64Length(png.size));
                if (result)
                    Base64Encode(png.data, png.size, result);
                else
                    SetError(L"Недостаточно памяти");
            }
            V_VT(ret) = VT_BSTR;
            V_BSTR(ret) = result ? result : SysAllocString(L"");
            break;
        }
        default:
            BufFree(&png);
            return S_FALSE;
        }

        BufFree(&png);
        return S_OK;
    }

private:
    ~QRCode1C() {
        InterlockedDecrement(&g_objects);
    }

    static long FirstOption(long method) {
        return method == METH_PNG_FILE ? 2 : 1;
    }

    void SetError(const wchar_t *a, const wchar_t *b = L"", const wchar_t *c = L"") {
        Concat(m_error, sizeof m_error / sizeof m_error[0], a, b, c);
    }

    void SetSystemError(const wchar_t *what, const wchar_t *path, DWORD err) {
        wchar_t msg[512];
        DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                                 err, 0, msg, sizeof msg / sizeof msg[0], NULL);
        while (n > 0 && (msg[n - 1] == L'\r' || msg[n - 1] == L'\n' || msg[n - 1] == L' '))
            n--;
        msg[n] = 0;
        Concat(m_error, sizeof m_error / sizeof m_error[0], what, path, L": ", n ? msg : L"ошибка Windows");
    }

    bool GetString(SAFEARRAY *args, long i, const wchar_t *param, BSTR *out) {
        VARIANT v, s;
        GetParam(args, i, &v);
        bool missing = IsMissing(&v);
        bool ok = !missing && ToType(&v, VT_BSTR, &s);
        VariantClear(&v);
        if (!ok) {
            SetError(L"Параметр ", param, missing ? L" не задан" : L" должен быть строкой");
            return false;
        }
        *out = V_BSTR(&s);  // владение строкой переходит вызывающему
        return true;
    }

    bool GetInt(SAFEARRAY *args, long i, const wchar_t *param, int def, int *out) {
        VARIANT v, n;
        GetParam(args, i, &v);
        if (IsMissing(&v)) {
            *out = def;
            return true;
        }
        bool ok = ToType(&v, VT_I4, &n);
        VariantClear(&v);
        if (!ok) {
            SetError(L"Параметр ", param, L" должен быть числом");
            return false;
        }
        *out = V_I4(&n);
        return true;
    }

    bool GetLevel(SAFEARRAY *args, long i, qrcodegen_Ecc *out) {
        VARIANT v, s;
        GetParam(args, i, &v);
        if (IsMissing(&v)) {
            *out = qrcodegen_Ecc_MEDIUM;
            return true;
        }
        wchar_t c = 0;
        if (ToType(&v, VT_BSTR, &s)) {
            if (SysStringLen(V_BSTR(&s)) == 1)
                c = V_BSTR(&s)[0];
            VariantClear(&s);
        }
        VariantClear(&v);
        switch (c) {
        case L'L': case L'l': *out = qrcodegen_Ecc_LOW; return true;
        case L'M': case L'm': *out = qrcodegen_Ecc_MEDIUM; return true;
        case L'Q': case L'q': *out = qrcodegen_Ecc_QUARTILE; return true;
        case L'H': case L'h': *out = qrcodegen_Ecc_HIGH; return true;
        }
        SetError(L"УровеньКоррекции должен быть одной из латинских букв L, M, Q, H");
        return false;
    }

    // Разбирает Текст и параметры оформления метода и рисует PNG.
    bool Generate(SAFEARRAY *args, long method, Buffer *png) {
        long first = FirstOption(method);
        BSTR text = NULL;
        int scale = 0, border = 0;
        qrcodegen_Ecc ecc = qrcodegen_Ecc_MEDIUM;
        bool ok = GetString(args, 0, L"Текст", &text) &&
                  GetInt(args, first, L"РазмерМодуля", kDefaultScale, &scale) &&
                  GetInt(args, first + 1, L"Поле", kDefaultBorder, &border) &&
                  GetLevel(args, first + 2, &ecc) && Encode(text, scale, border, ecc, png);
        SysFreeString(text);
        return ok;
    }

    bool Encode(BSTR text, int scale, int border, qrcodegen_Ecc ecc, Buffer *png) {
        int len = (int)SysStringLen(text);
        if (len == 0) {
            SetError(L"Пустой текст для QR-кода");
            return false;
        }
        if (scale < 1 || scale > 100) {
            SetError(L"РазмерМодуля должен быть от 1 до 100");
            return false;
        }
        if (border < 0 || border > 100) {
            SetError(L"Поле должно быть от 0 до 100");
            return false;
        }

        uint8_t data[qrcodegen_BUFFER_LEN_MAX];
        uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
        int n = WideCharToMultiByte(m_codepage, 0, text, len, NULL, 0, NULL, NULL);
        if (n <= 0) {
            SetError(L"Не удалось перекодировать текст");
            return false;
        }
        if (n > (int)sizeof data) {
            SetError(kTooLong);
            return false;
        }
        WideCharToMultiByte(m_codepage, 0, text, len, (char *)data, n, NULL, NULL);
        if (!qrcodegen_encodeBinary(data, (size_t)n, qr, ecc, qrcodegen_VERSION_MIN,
                                    qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
            SetError(kTooLong);
            return false;
        }
        if ((qrcodegen_getSize(qr) + 2 * border) * scale > kMaxImageSide) {
            SetError(L"Слишком большая картинка: уменьшите РазмерМодуля или Поле");
            return false;
        }
        if (!RenderQrPng(qr, scale, border, png)) {
            SetError(L"Недостаточно памяти для картинки");
            return false;
        }
        return true;
    }

    LONG m_refs;
    UINT m_codepage;
    wchar_t m_error[1024];
};

class ClassFactory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) {
        if (!ppv)
            return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory *>(this);
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    // Фабрика статическая, счётчик ссылок ей не нужен.
    STDMETHODIMP_(ULONG) AddRef() {
        return 2;
    }

    STDMETHODIMP_(ULONG) Release() {
        return 1;
    }

    STDMETHODIMP CreateInstance(IUnknown *outer, REFIID riid, void **ppv) {
        if (!ppv)
            return E_POINTER;
        *ppv = NULL;
        if (outer)
            return CLASS_E_NOAGGREGATION;
        void *mem = malloc(sizeof(QRCode1C));
        if (!mem)
            return E_OUTOFMEMORY;
        QRCode1C *obj = new (mem) QRCode1C();
        HRESULT hr = obj->QueryInterface(riid, ppv);
        obj->Release();
        return hr;
    }

    STDMETHODIMP LockServer(BOOL lock) {
        if (lock)
            InterlockedIncrement(&g_locks);
        else
            InterlockedDecrement(&g_locks);
        return S_OK;
    }
};

static ClassFactory g_factory;

// Регистрация: ProgID, CLSID и библиотека типов в root\Software\Classes.

static bool SetValue(HKEY root, const wchar_t *subkey, const wchar_t *name, const wchar_t *value) {
    HKEY key;
    if (RegCreateKeyExW(root, subkey, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &key,
                        NULL) != ERROR_SUCCESS)
        return false;
    LONG rc = RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value,
                             (DWORD)((lstrlenW(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

static void UnregisterServer(HKEY root) {
    wchar_t key[256];
    Concat(key, 256, L"Software\\Classes\\", kProgId);
    SHDeleteKeyW(root, key);
    Concat(key, 256, L"Software\\Classes\\CLSID\\", kClsid);
    SHDeleteKeyW(root, key);
    Concat(key, 256, L"Software\\Classes\\TypeLib\\", kLibid);
    SHDeleteKeyW(root, key);
}

static bool RegisterServer(HKEY root) {
    wchar_t path[MAX_PATH * 4], dir[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(g_module, path, sizeof path / sizeof path[0]);
    if (n == 0 || n >= sizeof path / sizeof path[0])
        return false;
    lstrcpyW(dir, path);
    PathRemoveFileSpecW(dir);

    wchar_t progId[256], clsid[256], inproc[256], typelib[256], typelibWin32[256];
    Concat(progId, 256, L"Software\\Classes\\", kProgId);
    Concat(clsid, 256, L"Software\\Classes\\CLSID\\", kClsid);
    Concat(inproc, 256, clsid, L"\\InprocServer32");
    Concat(typelib, 256, L"Software\\Classes\\TypeLib\\", kLibid, L"\\1.0");
    Concat(typelibWin32, 256, typelib, L"\\0\\win32");

    // Первая запись проверяет права: в HKLM без администратора дальше не идём.
    if (!SetValue(root, progId, NULL, kExtensionName))
        return false;
    wchar_t progIdClsid[256], clsidProgId[256], clsidTypelib[256], typelibFlags[256], typelibHelp[256];
    Concat(progIdClsid, 256, progId, L"\\CLSID");
    Concat(clsidProgId, 256, clsid, L"\\ProgID");
    Concat(clsidTypelib, 256, clsid, L"\\TypeLib");
    Concat(typelibFlags, 256, typelib, L"\\FLAGS");
    Concat(typelibHelp, 256, typelib, L"\\HELPDIR");

    bool ok = SetValue(root, progIdClsid, NULL, kClsid) &&
              SetValue(root, clsid, NULL, kExtensionName) &&
              SetValue(root, clsidProgId, NULL, kProgId) &&
              SetValue(root, inproc, NULL, path) &&
              SetValue(root, inproc, L"ThreadingModel", L"Apartment") &&
              SetValue(root, clsidTypelib, NULL, kLibid) &&
              SetValue(root, typelib, NULL, L"QRCode1C " WIDEN(QRCODE1C_VERSION_STR)) &&
              SetValue(root, typelibWin32, NULL, path) &&
              SetValue(root, typelibFlags, NULL, L"0") &&
              SetValue(root, typelibHelp, NULL, dir);
    if (!ok)
        UnregisterServer(root);
    return ok;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv) {
    if (!ppv)
        return E_POINTER;
    *ppv = NULL;
    if (!IsEqualCLSID(rclsid, CLSID_QRCode1C))
        return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() {
    return g_objects == 0 && g_locks == 0 ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer() {
    if (!RegisterServer(HKEY_CURRENT_USER))
        return SELFREG_E_CLASS;
    // 1С, запущенная «от имени администратора», не видит регистрацию в HKCU,
    // поэтому при наличии прав дублируем её в HKLM. Без прав молча пропускаем.
    RegisterServer(HKEY_LOCAL_MACHINE);
    return S_OK;
}

STDAPI DllUnregisterServer() {
    UnregisterServer(HKEY_CURRENT_USER);
    UnregisterServer(HKEY_LOCAL_MACHINE);
    return S_OK;
}
