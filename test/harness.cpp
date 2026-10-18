// Тестовый стенд: работает с qrcode1c.dll так же, как 1С (COM, IInitDone, ILanguageExtender).
// Запуск: harness.exe qrcode1c.dll cases.txt каталог_вывода
// Пишет в stdout строки "PASS|FAIL|WARN\tописание" и по сценариям из cases.txt
// "CASE\tid\tok|fail\tОписаниеОшибки" и "CASE_B64\tid\tsame|diff|empty".

#include <windows.h>
#include <ole2.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../src/addin.h"

static const CLSID kClsid = {0xFE7F2501, 0xF9F2, 0x4B32, {0xAA, 0x3F, 0x50, 0xDD, 0x8C, 0x65, 0xBD, 0xF9}};
static const wchar_t kClsidStr[] = L"{FE7F2501-F9F2-4B32-AA3F-50DD8C65BDF9}";
static const wchar_t kLibidStr[] = L"{E6289C53-FA61-4380-8D3E-DE91503BCB7D}";

static int g_failures;

static void Out(const wchar_t *fmt, ...) {
    static wchar_t buf[65536];
    static char utf8[4 * 65536];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 65535, fmt, ap);
    va_end(ap);
    buf[65535] = 0;
    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof utf8, NULL, NULL);
    DWORD written;
    if (n > 1)
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), utf8, n - 1, &written, NULL);
}

static void Check(bool ok, const wchar_t *what) {
    Out(L"%ls\t%ls\n", ok ? L"PASS" : L"FAIL", what);
    if (!ok)
        g_failures++;
}

static bool RegRead(HKEY root, const wchar_t *key, const wchar_t *name, wchar_t *out, DWORD cap) {
    HKEY h;
    if (RegOpenKeyExW(root, key, 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, size = cap * sizeof(wchar_t);
    LONG rc = RegQueryValueExW(h, name, NULL, &type, (BYTE *)out, &size);
    RegCloseKey(h);
    return rc == ERROR_SUCCESS && type == REG_SZ;
}

static bool RegEquals(const wchar_t *key, const wchar_t *name, const wchar_t *expected) {
    wchar_t value[2048];
    return RegRead(HKEY_CURRENT_USER, key, name, value, 2048) && lstrcmpiW(value, expected) == 0;
}

static uint8_t *ReadAll(const wchar_t *path, DWORD *size) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    *size = GetFileSize(h, NULL);
    uint8_t *data = (uint8_t *)malloc(*size + 1);
    DWORD got = 0;
    ReadFile(h, data, *size, &got, NULL);
    CloseHandle(h);
    data[*size] = 0;
    return data;
}

static uint8_t *Base64Decode(const wchar_t *s, size_t *size) {
    size_t len = wcslen(s);
    uint8_t *out = (uint8_t *)malloc(len / 4 * 3 + 3);
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len && s[i] != L'='; i++) {
        wchar_t c = s[i];
        int v = c >= L'A' && c <= L'Z' ? c - L'A' : c >= L'a' && c <= L'z' ? c - L'a' + 26
              : c >= L'0' && c <= L'9' ? c - L'0' + 52 : c == L'+' ? 62 : c == L'/' ? 63 : -1;
        if (v < 0) {
            free(out);
            return NULL;
        }
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    *size = n;
    return out;
}

static VARIANT Str(const wchar_t *s) {
    VARIANT v;
    VariantInit(&v);
    V_VT(&v) = VT_BSTR;
    V_BSTR(&v) = SysAllocString(s);
    return v;
}

static VARIANT Dbl(double d) {
    VARIANT v;
    VariantInit(&v);
    V_VT(&v) = VT_R8;
    V_R8(&v) = d;
    return v;
}

static VARIANT Int(long i) {
    VARIANT v;
    VariantInit(&v);
    V_VT(&v) = VT_I4;
    V_I4(&v) = i;
    return v;
}

static void ClearAll(VARIANT *v, int n) {
    for (int i = 0; i < n; i++)
        VariantClear(&v[i]);
}

// Обёртка над ILanguageExtender, вызывающая методы так же, как 1С.
struct AddIn {
    ILanguageExtender *le;

    long Method(const wchar_t *name) {
        BSTR b = SysAllocString(name);
        long n = -1;
        le->FindMethod(b, &n);
        SysFreeString(b);
        return n;
    }

    long Prop(const wchar_t *name) {
        BSTR b = SysAllocString(name);
        long n = -1;
        le->FindProp(b, &n);
        SysFreeString(b);
        return n;
    }

    // Строковое значение свойства; освободить SysFreeString.
    BSTR GetProp(const wchar_t *name) {
        VARIANT v;
        VariantInit(&v);
        if (le->GetPropVal(Prop(name), &v) != S_OK || V_VT(&v) != VT_BSTR) {
            VariantClear(&v);
            return SysAllocString(L"<нет значения>");
        }
        return V_BSTR(&v);
    }

    HRESULT SetProp(const wchar_t *name, const wchar_t *value) {
        VARIANT v = Str(value);
        HRESULT hr = le->SetPropVal(Prop(name), &v);
        VariantClear(&v);
        return hr;
    }

    // Недостающие хвостовые параметры 1С добирает через GetParamDefValue.
    HRESULT Call(const wchar_t *name, int argc, VARIANT *argv, VARIANT *ret, bool asProc = false) {
        long m = Method(name);
        if (m < 0)
            return DISP_E_MEMBERNOTFOUND;
        long np = 0;
        le->GetNParams(m, &np);
        SAFEARRAY *sa = SafeArrayCreateVector(VT_VARIANT, 0, np);
        for (long i = 0; i < np; i++) {
            VARIANT v;
            VariantInit(&v);
            if (i < argc)
                VariantCopy(&v, &argv[i]);
            else
                le->GetParamDefValue(m, i, &v);
            SafeArrayPutElement(sa, &i, &v);
            VariantClear(&v);
        }
        VariantInit(ret);
        HRESULT hr = asProc ? le->CallAsProc(m, &sa) : le->CallAsFunc(m, ret, &sa);
        SafeArrayDestroy(sa);
        return hr;
    }
};

static bool IsTrue(const VARIANT *v) {
    return V_VT(v) == VT_BOOL && V_BOOL(v) == VARIANT_TRUE;
}

// Сравнивает файл с base64-строкой.
static const wchar_t *CompareWithBase64(const wchar_t *path, const wchar_t *b64) {
    if (!b64 || !*b64)
        return L"empty";
    DWORD fileSize = 0;
    size_t decodedSize = 0;
    uint8_t *file = ReadAll(path, &fileSize);
    uint8_t *decoded = Base64Decode(b64, &decodedSize);
    bool same = file && decoded && fileSize == decodedSize && memcmp(file, decoded, fileSize) == 0;
    free(file);
    free(decoded);
    return same ? L"same" : L"diff";
}

static void RunCases(AddIn &a, const wchar_t *casesPath, const wchar_t *outDir) {
    DWORD size = 0;
    uint8_t *raw = ReadAll(casesPath, &size);
    if (!raw) {
        Check(false, L"чтение cases.txt");
        return;
    }
    int wlen = MultiByteToWideChar(CP_UTF8, 0, (char *)raw, (int)size, NULL, 0);
    wchar_t *text = (wchar_t *)calloc(wlen + 1, sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, (char *)raw, (int)size, text, wlen);
    free(raw);

    // Строка: id \t кодировка \t уровень \t размер модуля \t поле \t текст
    for (wchar_t *line = text; line && *line;) {
        wchar_t *next = wcschr(line, L'\n');
        if (next)
            *next++ = 0;
        size_t n = wcslen(line);
        if (n && line[n - 1] == L'\r')
            line[n - 1] = 0;
        wchar_t *f[6] = {line};
        int count = 1;
        for (wchar_t *p = line; *p && count < 6; p++)
            if (*p == L'\t') {
                *p = 0;
                f[count++] = p + 1;
            }
        if (count == 6) {
            a.SetProp(L"Кодировка", f[1]);
            wchar_t png[2048];
            _snwprintf(png, 2048, L"%ls\\%ls.png", outDir, f[0]);
            png[2047] = 0;

            VARIANT args[5] = {Str(f[5]), Str(png), Dbl(_wtoi(f[3])), Dbl(_wtoi(f[4])), Str(f[2])};
            VARIANT ret;
            HRESULT hr = a.Call(L"СформироватьPNG", 5, args, &ret);
            BSTR err = a.GetProp(L"ОписаниеОшибки");
            Out(L"CASE\t%ls\t%ls\t%ls\n", f[0], SUCCEEDED(hr) && IsTrue(&ret) ? L"ok" : L"fail", err);
            SysFreeString(err);
            VariantClear(&ret);

            // Тот же QR через base64 должен совпасть с файлом байт в байт.
            VARIANT args64[4] = {Str(f[5]), Dbl(_wtoi(f[3])), Dbl(_wtoi(f[4])), Str(f[2])};
            a.Call(L"СформироватьPNGBase64", 4, args64, &ret);
            Out(L"CASE_B64\t%ls\t%ls\n", f[0], CompareWithBase64(png, V_VT(&ret) == VT_BSTR ? V_BSTR(&ret) : NULL));
            VariantClear(&ret);
            ClearAll(args, 5);
            ClearAll(args64, 4);
        }
        line = next;
    }
    free(text);
}

int wmain(int argc, wchar_t **argv) {
    if (argc < 4) {
        Out(L"usage: harness.exe qrcode1c.dll cases.txt outdir\n");
        return 2;
    }
    const wchar_t *casesPath = argv[2], *outDir = argv[3];
    CoInitialize(NULL);

    HMODULE dll = LoadLibraryW(argv[1]);
    Check(dll != NULL, L"LoadLibrary qrcode1c.dll");
    if (!dll)
        return 1;
    wchar_t dllPath[MAX_PATH * 4];
    GetModuleFileNameW(dll, dllPath, MAX_PATH * 4);

    typedef HRESULT(STDAPICALLTYPE * PlainFn)();
    typedef HRESULT(STDAPICALLTYPE * GetClassFn)(REFCLSID, REFIID, void **);
    PlainFn regFn = (PlainFn)GetProcAddress(dll, "DllRegisterServer");
    PlainFn unregFn = (PlainFn)GetProcAddress(dll, "DllUnregisterServer");
    PlainFn canUnloadFn = (PlainFn)GetProcAddress(dll, "DllCanUnloadNow");
    GetClassFn getClassFn = (GetClassFn)(void *)GetProcAddress(dll, "DllGetClassObject");
    Check(regFn && unregFn && canUnloadFn && getClassFn, L"экспорт четырёх функций COM-сервера без декорирования");
    if (!(regFn && unregFn && canUnloadFn && getClassFn))
        return 1;

    // Регистрация, как при ЗагрузитьВнешнююКомпоненту.
    Check(regFn() == S_OK, L"DllRegisterServer");
    wchar_t key[512];
    Check(RegEquals(L"Software\\Classes\\AddIn.QRCode1C\\CLSID", NULL, kClsidStr), L"HKCU: AddIn.QRCode1C -> CLSID");
    _snwprintf(key, 512, L"Software\\Classes\\CLSID\\%ls\\InprocServer32", kClsidStr);
    Check(RegEquals(key, NULL, dllPath), L"HKCU: InprocServer32 = полный путь к DLL");
    Check(RegEquals(key, L"ThreadingModel", L"Apartment"), L"HKCU: ThreadingModel = Apartment");
    _snwprintf(key, 512, L"Software\\Classes\\CLSID\\%ls\\ProgID", kClsidStr);
    Check(RegEquals(key, NULL, L"AddIn.QRCode1C"), L"HKCU: CLSID -> ProgID");
    _snwprintf(key, 512, L"Software\\Classes\\TypeLib\\%ls\\1.0\\0\\win32", kLibidStr);
    Check(RegEquals(key, NULL, dllPath), L"HKCU: TypeLib win32 = путь к DLL");

    // Новый("AddIn.QRCode1C"): ProgID -> CLSID -> объект.
    IInitDone *init = NULL;
    CLSID clsid;
    HRESULT hr = CLSIDFromProgID(L"AddIn.QRCode1C", &clsid);
    if (SUCCEEDED(hr)) {
        Check(IsEqualCLSID(clsid, kClsid), L"CLSIDFromProgID(AddIn.QRCode1C)");
        hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, IID_IInitDone, (void **)&init);
        Check(SUCCEEDED(hr) && init, L"CoCreateInstance по ProgID");
    } else {
        Out(L"WARN\tCLSIDFromProgID не нашёл AddIn.QRCode1C (hr=%08lx), создаю объект через DllGetClassObject\n", hr);
    }
    if (!init) {
        IClassFactory *cf = NULL;
        hr = getClassFn(kClsid, IID_IClassFactory, (void **)&cf);
        Check(SUCCEEDED(hr) && cf, L"DllGetClassObject");
        if (cf) {
            cf->CreateInstance(NULL, IID_IInitDone, (void **)&init);
            cf->Release();
        }
    }
    Check(init != NULL, L"объект компоненты создан");
    if (!init)
        return 1;

    bool typelibOk = false;
    ITypeLib *tl = NULL;
    if (SUCCEEDED(LoadTypeLib(dllPath, &tl))) {
        ITypeInfo *ti = NULL;
        if (SUCCEEDED(tl->GetTypeInfoOfGuid(kClsid, &ti))) {
            BSTR name = NULL;
            TYPEATTR *attr = NULL;
            ti->GetDocumentation(MEMBERID_NIL, &name, NULL, NULL, NULL);
            if (SUCCEEDED(ti->GetTypeAttr(&attr))) {
                typelibOk = attr->typekind == TKIND_COCLASS && name && lstrcmpW(name, L"QRCode1C") == 0;
                ti->ReleaseTypeAttr(attr);
            }
            SysFreeString(name);
            ti->Release();
        }
        tl->Release();
    }
    Check(typelibOk, L"библиотека типов в ресурсах: coclass QRCode1C с нашим CLSID");

    Check(init->Init(NULL) == S_OK, L"IInitDone::Init");
    SAFEARRAY *info = SafeArrayCreateVector(VT_VARIANT, 0, 1);
    Check(init->GetInfo(&info) == S_OK, L"IInitDone::GetInfo");
    {
        long i = 0;
        VARIANT v;
        VariantInit(&v);
        SafeArrayGetElement(info, &i, &v);
        Check(V_VT(&v) == VT_I4 && V_I4(&v) == 2000, L"GetInfo: версия технологии 2000");
        VariantClear(&v);
    }
    SafeArrayDestroy(info);

    ILanguageExtender *le = NULL;
    Check(SUCCEEDED(init->QueryInterface(IID_ILanguageExtender, (void **)&le)) && le,
          L"QueryInterface(ILanguageExtender)");
    if (!le)
        return 1;
    AddIn a = {le};

    BSTR ext = NULL;
    le->RegisterExtensionAs(&ext);
    Check(ext && lstrcmpW(ext, L"QRCode1C") == 0, L"RegisterExtensionAs = QRCode1C");
    SysFreeString(ext);

    Check(a.Method(L"сформироватьpng") == 0, L"FindMethod: русское имя в другом регистре");
    Check(a.Method(L"GENERATEPNGBASE64") == 1, L"FindMethod: английское имя в другом регистре");
    Check(a.Method(L"НетТакогоМетода") == -1, L"FindMethod: неизвестный метод = -1");
    Check(a.Prop(L"описаниеошибки") == 1, L"FindProp: русское имя в другом регистре");
    BSTR name = NULL;
    le->GetMethodName(0, 1, &name);
    Check(name && lstrcmpW(name, L"СформироватьPNG") == 0, L"GetMethodName(0, рус)");
    SysFreeString(name);
    name = NULL;
    le->GetMethodName(1, 0, &name);
    Check(name && lstrcmpW(name, L"GeneratePNGBase64") == 0, L"GetMethodName(1, англ)");
    SysFreeString(name);
    long np = 0;
    le->GetNParams(0, &np);
    Check(np == 5, L"GetNParams(СформироватьPNG) = 5");
    le->GetNParams(1, &np);
    Check(np == 4, L"GetNParams(СформироватьPNGBase64) = 4");
    BOOL flag = TRUE;
    le->IsPropWritable(a.Prop(L"Версия"), &flag);
    Check(!flag, L"Версия только для чтения");
    le->IsPropWritable(a.Prop(L"Кодировка"), &flag);
    Check(flag, L"Кодировка доступна для записи");
    flag = FALSE;
    le->HasRetVal(0, &flag);
    Check(flag, L"HasRetVal(СформироватьPNG)");

    BSTR version = a.GetProp(L"Версия");
    Out(L"INFO\tВерсия компоненты: %ls\n", version);
    SysFreeString(version);
    BSTR enc = a.GetProp(L"Кодировка");
    Check(lstrcmpW(enc, L"UTF-8") == 0, L"Кодировка по умолчанию UTF-8");
    SysFreeString(enc);
    Check(FAILED(a.SetProp(L"Кодировка", L"KOI8-R")), L"неизвестная кодировка отклоняется (E_FAIL -> исключение в 1С)");
    enc = a.GetProp(L"Кодировка");
    Check(lstrcmpW(enc, L"UTF-8") == 0, L"после ошибки кодировка не изменилась");
    SysFreeString(enc);

    RunCases(a, casesPath, outDir);
    a.SetProp(L"Кодировка", L"UTF-8");

    // Как в 1С: только Текст и ИмяФайла, остальное по умолчанию из GetParamDefValue.
    wchar_t pngDefault[2048], pngInts[2048], pngProc[2048];
    _snwprintf(pngDefault, 2048, L"%ls\\defaults.png", outDir);
    _snwprintf(pngInts, 2048, L"%ls\\ints.png", outDir);
    _snwprintf(pngProc, 2048, L"%ls\\proc.png", outDir);
    VARIANT ret;
    VARIANT defArgs[2] = {Str(L"defaults"), Str(pngDefault)};
    a.Call(L"СформироватьPNG", 2, defArgs, &ret);
    Check(IsTrue(&ret), L"вызов только с обязательными параметрами");
    VariantClear(&ret);
    VARIANT intArgs[5] = {Str(L"defaults"), Str(pngInts), Int(4), Int(4), Str(L"m")};
    a.Call(L"СформироватьPNG", 5, intArgs, &ret);
    Check(IsTrue(&ret), L"числа как VT_I4, уровень строчной буквой");
    VariantClear(&ret);
    DWORD s1 = 0, s2 = 0;
    uint8_t *f1 = ReadAll(pngDefault, &s1), *f2 = ReadAll(pngInts, &s2);
    Check(f1 && f2 && s1 == s2 && memcmp(f1, f2, s1) == 0, L"значения по умолчанию = 4, 4, \"M\"");
    free(f1);
    free(f2);
    VARIANT procArgs[2] = {Str(L"proc"), Str(pngProc)};
    hr = a.Call(L"СформироватьPNG", 2, procArgs, &ret, true);
    DWORD s3 = 0;
    uint8_t *f3 = ReadAll(pngProc, &s3);
    Check(hr == S_OK && f3 && s3 > 0, L"вызов как процедура (CallAsProc)");
    free(f3);
    ClearAll(defArgs, 2);
    ClearAll(intArgs, 5);
    ClearAll(procArgs, 2);

    // Несуществующий каталог: Ложь и понятное описание ошибки, без исключения.
    VARIANT badArgs[2] = {Str(L"x"), Str(L"Z:\\нет\\такого\\каталога\\x.png")};
    hr = a.Call(L"СформироватьPNG", 2, badArgs, &ret);
    BSTR err = a.GetProp(L"ОписаниеОшибки");
    Check(hr == S_OK && V_VT(&ret) == VT_BOOL && !IsTrue(&ret) && SysStringLen(err) > 0,
          L"ошибка записи файла -> Ложь и ОписаниеОшибки");
    Out(L"INFO\tОписаниеОшибки: %ls\n", err);
    SysFreeString(err);
    VariantClear(&ret);
    ClearAll(badArgs, 2);

    // Второй объект в том же сеансе (версия технологии 2.0).
    IInitDone *init2 = NULL;
    IClassFactory *cf = NULL;
    if (SUCCEEDED(getClassFn(kClsid, IID_IClassFactory, (void **)&cf))) {
        cf->CreateInstance(NULL, IID_IInitDone, (void **)&init2);
        cf->Release();
    }
    Check(init2 && init2->Init(NULL) == S_OK, L"второй объект в том же сеансе");
    if (init2) {
        init2->Done();
        init2->Release();
    }

    le->Release();
    init->Done();
    init->Release();
    CoFreeUnusedLibraries();
    Check(canUnloadFn() == S_OK, L"DllCanUnloadNow после освобождения объектов");

    Check(unregFn() == S_OK, L"DllUnregisterServer");
    wchar_t value[256];
    Check(!RegRead(HKEY_CURRENT_USER, L"Software\\Classes\\AddIn.QRCode1C\\CLSID", NULL, value, 256),
          L"после DllUnregisterServer ключей в HKCU нет");

    Out(L"SUMMARY\tfailures=%d\n", g_failures);
    CoUninitialize();
    return g_failures ? 1 : 0;
}
