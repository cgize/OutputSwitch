#pragma once

#include <windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <propsys.h>

// CLSID_PolicyConfigClient
// {870AF99C-171D-4F9E-AF0D-E63DF40C2BC9}
static const CLSID CLSID_PolicyConfigClient =
{ 0x870AF99C, 0x171D, 0x4F9E, { 0xAF, 0x0D, 0xE6, 0x3D, 0xF4, 0x0C, 0x2B, 0xC9 } };

// IID_IPolicyConfig
// {F8679F50-850A-41CF-9C72-430F290290C8}
static const IID IID_IPolicyConfig =
{ 0xF8679F50, 0x850A, 0x41CF, { 0x9C, 0x72, 0x43, 0x0F, 0x29, 0x02, 0x90, 0xC8 } };

struct __declspec(uuid("{F8679F50-850A-41CF-9C72-430F290290C8}")) IPolicyConfig : public IUnknown
{
    STDMETHOD(GetMixFormat)(PCWSTR, WAVEFORMATEX**) = 0;
    STDMETHOD(GetDeviceFormat)(PCWSTR, INT, WAVEFORMATEX**) = 0;
    STDMETHOD(ResetDeviceFormat)(PCWSTR) = 0;
    STDMETHOD(SetDeviceFormat)(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    STDMETHOD(GetProcessingPeriod)(PCWSTR, INT, PINT64, PINT64) = 0;
    STDMETHOD(SetProcessingPeriod)(PCWSTR, PINT64) = 0;
    STDMETHOD(GetShareMode)(PCWSTR, INT*) = 0;
    STDMETHOD(SetShareMode)(PCWSTR, INT) = 0;
    STDMETHOD(GetPropertyValue)(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    STDMETHOD(SetPropertyValue)(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    STDMETHOD(SetDefaultEndpoint)(PCWSTR, ERole) = 0;
    STDMETHOD(SetEndpointVisibility)(PCWSTR, INT) = 0;
};
