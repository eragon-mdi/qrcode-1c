// Интерфейсы технологии внешних компонент 1С:Предприятия (COM, версия 2.0).
// Порядок методов в интерфейсах менять нельзя: 1С вызывает их по таблице виртуальных функций.
#pragma once

#include <windows.h>
#include <oaidl.h>

// {AB634001-F13D-11D0-A459-004095E1DAEA}
static const IID IID_IInitDone =
    {0xAB634001, 0xF13D, 0x11D0, {0xA4, 0x59, 0x00, 0x40, 0x95, 0xE1, 0xDA, 0xEA}};

// {AB634003-F13D-11D0-A459-004095E1DAEA}
static const IID IID_ILanguageExtender =
    {0xAB634003, 0xF13D, 0x11D0, {0xA4, 0x59, 0x00, 0x40, 0x95, 0xE1, 0xDA, 0xEA}};

struct IInitDone : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Init(IDispatch *pConnection) = 0;
    virtual HRESULT STDMETHODCALLTYPE Done() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInfo(SAFEARRAY **pInfo) = 0;
};

struct ILanguageExtender : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE RegisterExtensionAs(BSTR *pExtensionName) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNProps(long *plProps) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindProp(BSTR bstrPropName, long *plPropNum) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropName(long lPropNum, long lAliasNum, BSTR *pbstrPropName) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropVal(long lPropNum, VARIANT *pvarPropVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropVal(long lPropNum, VARIANT *pvarPropVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsPropReadable(long lPropNum, BOOL *pboolPropRead) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsPropWritable(long lPropNum, BOOL *pboolPropWrite) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNMethods(long *plMethods) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindMethod(BSTR bstrMethodName, long *plMethodNum) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMethodName(long lMethodNum, long lAliasNum, BSTR *pbstrMethodName) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNParams(long lMethodNum, long *plParams) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetParamDefValue(long lMethodNum, long lParamNum, VARIANT *pvarParamDefValue) = 0;
    virtual HRESULT STDMETHODCALLTYPE HasRetVal(long lMethodNum, BOOL *pboolRetValue) = 0;
    virtual HRESULT STDMETHODCALLTYPE CallAsProc(long lMethodNum, SAFEARRAY **paParams) = 0;
    virtual HRESULT STDMETHODCALLTYPE CallAsFunc(long lMethodNum, VARIANT *pvarRetValue, SAFEARRAY **paParams) = 0;
};
