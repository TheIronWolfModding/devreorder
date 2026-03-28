
#define CINTERFACE
#include "stdafx.h"
#include <dinput.h>
#include <initguid.h>
#include <Cfgmgr32.h>
#include <Devpkey.h>
#include <Shlwapi.h>

#include "dinput8.h"
#include "Common.h"
#include "Logger.h"
#include "Utils.h"
#include "DirectInputModuleManager.h"
#include "SimpleIni.h"

using namespace std;

static string g_enumLog;

static void enumLog(const char *fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	_vsnprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
	va_end(args);
	g_enumLog += buf;
	g_enumLog += "\r\n";
}

static void enumLogW(const wchar_t *fmt, ...)
{
	wchar_t wbuf[512];
	va_list args;
	va_start(args, fmt);
	_vsnwprintf_s(wbuf, _countof(wbuf), _TRUNCATE, fmt, args);
	va_end(args);
	char buf[1024];
	WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, sizeof(buf), NULL, NULL);
	g_enumLog += buf;
	g_enumLog += "\r\n";
}

static string guidStr(const GUID &g)
{
	char buf[64];
	sprintf_s(buf, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
		g.Data1, g.Data2, g.Data3,
		g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
		g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
	return buf;
}

static void writeEnumLog()
{
	char path[MAX_PATH];
	GetModuleFileNameA(NULL, path, MAX_PATH);
	PathRemoveFileSpecA(path);
	PathAppendA(path, "devreorder.log");
	HANDLE hFile = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile != INVALID_HANDLE_VALUE) {
		DWORD written;
		WriteFile(hFile, g_enumLog.c_str(), (DWORD)g_enumLog.size(), &written, NULL);
		FlushFileBuffers(hFile);
		CloseHandle(hFile);
	}
	g_enumLog.clear();
}

enum MatchType {
	kNoMatch,
	kNameMatch,
	kGUIDMatch,
	kDeviceInstanceIDMatch,
};

// Parse a GUID string like "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" into a GUID struct.
// Returns GUID_NULL on failure.
static GUID parseGUID(const string &str)
{
	GUID g;
	StringToGUID(&g, str);
	return g;
}

static GUID parseGUID(const wstring &str)
{
	GUID g;
	StringToGUID(&g, str);
	return g;
}

HRESULT(STDMETHODCALLTYPE *TrueEnumDevicesA) (LPDIRECTINPUT8A This, DWORD dwDevType, LPDIENUMDEVICESCALLBACKA lpCallback, LPVOID pvRef, DWORD dwFlags) = nullptr;
HRESULT(STDMETHODCALLTYPE *TrueEnumDevicesW) (LPDIRECTINPUT8W This, DWORD dwDevType, LPDIENUMDEVICESCALLBACKW lpCallback, LPVOID pvRef, DWORD dwFlags) = nullptr;
HRESULT(STDMETHODCALLTYPE *EnumDevicesA) (LPDIRECTINPUT8A This, DWORD dwDevType, LPDIENUMDEVICESCALLBACKA lpCallback, LPVOID pvRef, DWORD dwFlags) = nullptr;
HRESULT(STDMETHODCALLTYPE *EnumDevicesW) (LPDIRECTINPUT8W This, DWORD dwDevType, LPDIENUMDEVICESCALLBACKW lpCallback, LPVOID pvRef, DWORD dwFlags) = nullptr;

struct EnumCallbackUserData {
	void* di;
	void* enumData;
};

// Forward declarations for string utilities
string trim(const string &str);
wstring trim(const wstring &str);
string toLower(const string &str);
wstring toLower(const wstring &str);

static CSimpleIniW& getCachedIni()
{
	static CSimpleIniW ini;
	static bool loaded = false;

	if (!loaded) {
		loaded = true;
		ini.SetAllowEmptyValues(true);
		wstring inipath(L"devreorder.ini");
		SI_Error err = ini.LoadFile(inipath.c_str());

		if (err < 0) {
			CheckCommonDirectory(&inipath, L"devreorder");
			err = ini.LoadFile(inipath.c_str());

			if (err < 0) {
				PrintLog("devreorder error: devreorder.ini file not found");
			} else {
				PrintLog("devreorder: using system-wide devreorder.ini");
			}
		} else {
			PrintLog("devreorder: using program-specific devreorder.ini");
		}
	}

	return ini;
}

vector<wstring> loadAllKeysFromSectionOfIni(const wstring &section)
{
	vector<wstring> result;
	CSimpleIniW &ini = getCachedIni();

	CSimpleIniW::TNamesDepend keys;
	ini.GetAllKeys(section.c_str(), keys);
	keys.sort(CSimpleIniW::Entry::LoadOrder());

	for (CSimpleIniW::TNamesDepend::iterator it = keys.begin(); it != keys.end(); ++it) {
		result.push_back(it->pItem);
	}

	return result;
}

// Pre-parsed GUID cache: for entries starting with '{', store the parsed GUID.
// GUID_NULL means the entry was not a GUID or failed to parse.
static vector<GUID> & sortedControllersGUIDs()
{
	static vector<GUID> result;
	return result;
}

static vector<GUID> & hiddenControllersGUIDs()
{
	static vector<GUID> result;
	return result;
}

static vector<GUID> & visibleControllersGUIDs()
{
	static vector<GUID> result;
	return result;
}

static void buildGUIDCache(const vector<wstring> &entries, vector<GUID> &guids)
{
	guids.resize(entries.size());
	for (size_t i = 0; i < entries.size(); ++i) {
		if (entries[i].length() > 0 && entries[i][0] == L'{') {
			guids[i] = parseGUID(entries[i]);
		} else {
			guids[i] = GUID_NULL;
		}
	}
}

vector<wstring> & sortedControllersW()
{
	static vector<wstring> result;
	static bool needToInitialize = true;

	if (needToInitialize) {
		needToInitialize = false;
		result = loadAllKeysFromSectionOfIni(L"order");
		for (auto &entry : result) {
			entry = trim(entry);
			if (entry.length() > 0 && (entry[0] == L'{' || entry[0] == L'<')) {
				entry = toLower(entry);
			}
		}
		buildGUIDCache(result, sortedControllersGUIDs());
	}

	return result;
}

vector<string> & sortedControllersA()
{
	static vector<string> result;
	static bool needToInitialize = true;

	if (needToInitialize) {
		vector<wstring> &wideList = sortedControllersW();

		for (unsigned int i = 0; i < wideList.size(); ++i) {
			result.push_back(UTF16ToUTF8(wideList[i]));
		}

		needToInitialize = false;
	}

	return result;
}

vector<wstring> & hiddenControllersW()
{
	static vector<wstring> result;
	static bool needToInitialize = true;

	if (needToInitialize) {
		result = loadAllKeysFromSectionOfIni(L"hidden");
		for (auto &entry : result) {
			entry = trim(entry);
			if (entry.length() > 0 && (entry[0] == L'{' || entry[0] == L'<')) {
				entry = toLower(entry);
			}
		}
		buildGUIDCache(result, hiddenControllersGUIDs());
		needToInitialize = false;
	}

	return result;
}

vector<string> & hiddenControllersA()
{
	static vector<string> result;
	static bool needToInitialize = true;

	if (needToInitialize) {
		vector<wstring> &wideList = hiddenControllersW();

		for(unsigned int i = 0; i < wideList.size(); ++i) {
			result.push_back(UTF16ToUTF8(wideList[i]));
		}

		needToInitialize = false;
	}

	return result;
}

vector<wstring> & visibleControllersW()
{
	static vector<wstring> result;
	static bool needToInitialize = true;

	if (needToInitialize) {
		result = loadAllKeysFromSectionOfIni(L"visible");
		for (auto &entry : result) {
			entry = trim(entry);
			if (entry.length() > 0 && (entry[0] == L'{' || entry[0] == L'<')) {
				entry = toLower(entry);
			}
		}
		buildGUIDCache(result, visibleControllersGUIDs());
		needToInitialize = false;
	}

	return result;
}

vector<string> & visibleControllersA()
{
	static vector<string> result;
	static bool needToInitialize = true;

	if (needToInitialize) {
		vector<wstring> &wideList = visibleControllersW();

		for (unsigned int i = 0; i < wideList.size(); ++i) {
			result.push_back(UTF16ToUTF8(wideList[i]));
		}

		needToInitialize = false;
	}

	return result;
}

vector<wstring> & ignoredProcessesW()
{
	static vector<wstring> result;
	static bool needToInitialize = true;

	if (needToInitialize) {
		result = loadAllKeysFromSectionOfIni(L"ignored processes");
		for (auto &entry : result) {
			entry = trim(toLower(entry));
		}
		needToInitialize = false;
	}

	return result;
}

vector<string> & ignoredProcessesA()
{
	static vector<string> result;
	static bool needToInitialize = true;

	if (needToInitialize) {
		vector<wstring> &wideList = ignoredProcessesW();

		for (unsigned int i = 0; i < wideList.size(); ++i) {
			result.push_back(UTF16ToUTF8(wideList[i]));
		}

		needToInitialize = false;
	}

	return result;
}

template <class T>
struct DeviceEnumData {
	vector<T> nonsorted;
	vector<vector<T> > sorted;
};

string trim(const string &str)
{
	size_t first = str.find_first_not_of(' ');

	// string is empty or nothing but whitespace
	if (string::npos == first) {
		return string();
	}

	size_t last = str.find_last_not_of(' ');
	return str.substr(first, (last - first + 1));
}

wstring trim(const wstring &str)
{
	size_t first = str.find_first_not_of(' ');

	// string is empty or nothing but whitespace
	if (wstring::npos == first) {
		return wstring();
	}

	size_t last = str.find_last_not_of(' ');
	return str.substr(first, (last - first + 1));
}

string toLower(const string &str)
{
	string result = str;
	transform(result.begin(), result.end(), result.begin(), ::tolower);
	return result;
}

wstring toLower(const wstring &str)
{
	wstring result = str;
	transform(result.begin(), result.end(), result.begin(), ::tolower);
	return result;
}


std::wstring getDeviceInstanceIdProperty(const DIPROPGUIDANDPATH& iap)
{
	DEVPROPTYPE propertyType;
	ULONG propertySize = 0;
	CONFIGRET cr = CM_Get_Device_Interface_PropertyW(iap.wszPath, &DEVPKEY_Device_InstanceId, &propertyType, nullptr, &propertySize, 0);

	if (cr != CR_BUFFER_SMALL) {
		return std::wstring();
	}

	std::wstring deviceId;
	deviceId.resize(propertySize);
	cr = ::CM_Get_Device_Interface_PropertyW(iap.wszPath, &DEVPKEY_Device_InstanceId, &propertyType, (PBYTE)deviceId.data(), &propertySize, 0);

	if (cr != CR_SUCCESS) {
		return std::wstring();
	}

	// There may be trailing null characters in the string that we need to remove so we can append to it:
	deviceId.erase(std::find(deviceId.begin(), deviceId.end(), L'\0'), deviceId.end());

	return deviceId;
}

void getDeviceInstanceId(LPCDIDEVICEINSTANCEA deviceInstance, EnumCallbackUserData *userData, string &deviceInstanceId)
{
	if (deviceInstanceId.size() > 0) {
		// Already got the device instance ID
		return;
	}

	LPDIRECTINPUT8A di = (LPDIRECTINPUT8A)userData->di;
	IDirectInputDevice8A *device;
	IDirectInput_CreateDevice(di, deviceInstance->guidInstance, &device, NULL);

	DIPROPGUIDANDPATH iap = {};
	iap.diph.dwSize = sizeof(DIPROPGUIDANDPATH);
	iap.diph.dwHeaderSize = sizeof(DIPROPHEADER);
	iap.diph.dwHow = DIPH_DEVICE;

	std::wstring deviceIdWide;

	if (SUCCEEDED(IDirectInputDevice_GetProperty(device, DIPROP_GUIDANDPATH, &iap.diph))) {
		deviceIdWide = trim(toLower(getDeviceInstanceIdProperty(iap)));
		deviceIdWide.insert(0, L"<");
		deviceIdWide.append(L">");
	} else {
		PrintLog("Error: failed to get device instance ID for: %s", deviceInstance->tszProductName);
		// guaranteed not to match anything
		deviceIdWide = L"!";
	}

	deviceInstanceId = UTF16ToUTF8(deviceIdWide);

	IDirectInputDevice_Release(device);
}

void getDeviceInstanceId(LPCDIDEVICEINSTANCEW deviceInstance, EnumCallbackUserData* userData, wstring& deviceInstanceId)
{
	if (deviceInstanceId.size() > 0) {
		// Already got the device instance ID
		return;
	}

	LPDIRECTINPUT8W di = (LPDIRECTINPUT8W)userData->di;
	IDirectInputDevice8W *device;
	IDirectInput_CreateDevice(di, deviceInstance->guidInstance, &device, NULL);

	DIPROPGUIDANDPATH iap = {};
	iap.diph.dwSize = sizeof(DIPROPGUIDANDPATH);
	iap.diph.dwHeaderSize = sizeof(DIPROPHEADER);
	iap.diph.dwHow = DIPH_DEVICE;

	if (SUCCEEDED(IDirectInputDevice_GetProperty(device, DIPROP_GUIDANDPATH, &iap.diph))) {
		deviceInstanceId = trim(toLower(getDeviceInstanceIdProperty(iap)));
		deviceInstanceId.insert(0, L"<");
		deviceInstanceId.append(L">");
	} else {
		PrintLog(L"Error: failed to get device instance ID for: %s", deviceInstance->tszProductName);
		deviceInstanceId = L"!";
	}

	IDirectInputDevice_Release(device);
}

bool currentProcessIsIgnored()
{
	static int cachedResult = -1;
	if (cachedResult >= 0) return cachedResult != 0;

	WCHAR currentProcessPathCStr[MAX_PATH];
	DWORD size = GetModuleFileName(NULL, currentProcessPathCStr, MAX_PATH);
	wstring processFileName = currentProcessPathCStr;
	const size_t lastSlashIndex = processFileName.find_last_of(L"\\/");

	if (lastSlashIndex != std::string::npos) {
		processFileName.erase(0, lastSlashIndex + 1);
	}

	processFileName = trim(toLower(processFileName));
	PrintLog(L"Current process name: %s", processFileName.c_str());

	std::vector<wstring> &ignored = ignoredProcessesW();

	PrintLog(L"Ignored list:");
	for (auto it = ignored.begin(); it != ignored.end(); ++it) {
		PrintLog(L" - %s", it->c_str());

		if (*it == processFileName) {
			PrintLog(L"   found match!");
			cachedResult = 1;
			return true;
		}
	}

	cachedResult = 0;
	return false;
}

MatchType deviceMatchesEntry(LPCDIDEVICEINSTANCEA deviceInstance, EnumCallbackUserData* userData, string &deviceInstanceId, const string &entry, const GUID &entryGUID)
{
	if (entry.length() == 0) {
		return kNoMatch;
	}

	if (entry[0] == '{') {
		return IsEqualGUID(deviceInstance->guidInstance, entryGUID) ? kGUIDMatch : kNoMatch;
	} else if (entry[0] == '<') {
		getDeviceInstanceId(deviceInstance, userData, deviceInstanceId);
		PrintLog("  a: %s", entry.c_str());
		PrintLog("  b: %s", deviceInstanceId.c_str());
		return (deviceInstanceId == entry) ? kDeviceInstanceIDMatch : kNoMatch;
	} else {
		string deviceIdentifier = trim(deviceInstance->tszProductName);
		return (deviceIdentifier == entry) ? kNameMatch : kNoMatch;
	}
}

MatchType deviceMatchesEntry(LPCDIDEVICEINSTANCEW deviceInstance, EnumCallbackUserData* userData, wstring& deviceInstanceId, const wstring &entry, const GUID &entryGUID)
{
	if (entry.length() == 0) {
		return kNoMatch;
	}

	if (entry[0] == L'{') {
		return IsEqualGUID(deviceInstance->guidInstance, entryGUID) ? kGUIDMatch : kNoMatch;
	} else if (entry[0] == L'<') {
		getDeviceInstanceId(deviceInstance, userData, deviceInstanceId);
		PrintLog(L"  a: %s", entry.c_str());
		PrintLog(L"  b: %s", deviceInstanceId.c_str());
		return (deviceInstanceId == entry) ? kDeviceInstanceIDMatch : kNoMatch;
	} else {
		wstring deviceIdentifier = trim(deviceInstance->tszProductName);
		return (deviceIdentifier == entry) ? kNameMatch : kNoMatch;
	}
}

BOOL CALLBACK enumCallbackA(LPCDIDEVICEINSTANCEA deviceInstance, LPVOID userDataPtr)
{
	EnumCallbackUserData *userData = (EnumCallbackUserData *)userDataPtr;
	DeviceEnumData<DIDEVICEINSTANCEA> *enumData = (DeviceEnumData<DIDEVICEINSTANCEA> *)userData->enumData;
	vector<string> &order = sortedControllersA();
	vector<string> &hidden = hiddenControllersA();
	vector<string> &visible = visibleControllersA();
	vector<GUID> &orderGUIDs = sortedControllersGUIDs();
	vector<GUID> &hiddenGUIDs = hiddenControllersGUIDs();
	vector<GUID> &visibleGUIDs = visibleControllersGUIDs();
	string deviceInstanceId;

	for (unsigned int i = 0; i < hidden.size(); ++i) {
		if (deviceMatchesEntry(deviceInstance, userData, deviceInstanceId, hidden[i], hiddenGUIDs[i])) {
			enumLog("hidden:   \"%s\" %s", deviceInstance->tszProductName, guidStr(deviceInstance->guidInstance).c_str());
			return DIENUM_CONTINUE;
		}
	}

	DWORD deviceType = GET_DIDEVICE_TYPE(deviceInstance->dwDevType);

	if (visible.size() > 0 && deviceType != DI8DEVTYPE_KEYBOARD && deviceType != DI8DEVTYPE_MOUSE
		&& deviceType != DI8DEVTYPE_SCREENPOINTER) {
		bool isVisible = false;

		for (unsigned int i = 0; i < visible.size(); ++i) {
			if (deviceMatchesEntry(deviceInstance, userData, deviceInstanceId, visible[i], visibleGUIDs[i])) {
				isVisible = true;
				break;
			}
		}

		if (!isVisible) {
			enumLog("hidden:   \"%s\" %s (not in visible list)", deviceInstance->tszProductName, guidStr(deviceInstance->guidInstance).c_str());
			return DIENUM_CONTINUE;
		}
	}

	int nameMatchIndex = -1;

	for (unsigned int i = 0; i < order.size(); ++i) {
		MatchType match = deviceMatchesEntry(deviceInstance, userData, deviceInstanceId, order[i], orderGUIDs[i]);

		// Important that we prioritize a match via device instance ID or GUID over a match via name
		if (match == kDeviceInstanceIDMatch) {
			enumLog("sorted:   \"%s\" %s (device instance ID)", deviceInstance->tszProductName, guidStr(deviceInstance->guidInstance).c_str());
			enumData->sorted[i].push_back(*deviceInstance);
			return DIENUM_CONTINUE;

		} else if (match == kGUIDMatch) {
			enumLog("sorted:   \"%s\" %s (GUID)", deviceInstance->tszProductName, guidStr(deviceInstance->guidInstance).c_str());
			enumData->sorted[i].push_back(*deviceInstance);
			return DIENUM_CONTINUE;

		} else if (match == kNameMatch) {
			nameMatchIndex = i;
		}
	}

	if (nameMatchIndex != -1) {
		enumLog("sorted:   \"%s\" %s (name)", deviceInstance->tszProductName, guidStr(deviceInstance->guidInstance).c_str());
		enumData->sorted[nameMatchIndex].push_back(*deviceInstance);
		return DIENUM_CONTINUE;
	}

	enumLog("unsorted: \"%s\" %s", deviceInstance->tszProductName, guidStr(deviceInstance->guidInstance).c_str());
	enumData->nonsorted.push_back(*deviceInstance);
	return DIENUM_CONTINUE;
}

BOOL CALLBACK enumCallbackW(LPCDIDEVICEINSTANCEW deviceInstance, LPVOID userDataPtr)
{
	EnumCallbackUserData *userData = (EnumCallbackUserData *)userDataPtr;
	DeviceEnumData<DIDEVICEINSTANCEW> *enumData = (DeviceEnumData<DIDEVICEINSTANCEW> *)userData->enumData;
	vector<wstring> &order = sortedControllersW();
	vector<wstring> &hidden = hiddenControllersW();
	vector<wstring> &visible = visibleControllersW();
	vector<GUID> &orderGUIDs = sortedControllersGUIDs();
	vector<GUID> &hiddenGUIDs = hiddenControllersGUIDs();
	vector<GUID> &visibleGUIDs = visibleControllersGUIDs();
	wstring deviceInstanceId;

	for (unsigned int i = 0; i < hidden.size(); ++i) {
		if (deviceMatchesEntry(deviceInstance, userData, deviceInstanceId, hidden[i], hiddenGUIDs[i])) {
			enumLogW(L"hidden:   \"%s\" %S", deviceInstance->tszProductName, guidStr(deviceInstance->guidInstance).c_str());
			return DIENUM_CONTINUE;
		}
	}

	DWORD deviceType = GET_DIDEVICE_TYPE(deviceInstance->dwDevType);

	if (visible.size() > 0 && deviceType != DI8DEVTYPE_KEYBOARD && deviceType != DI8DEVTYPE_MOUSE
		&& deviceType != DI8DEVTYPE_SCREENPOINTER) {
		bool isVisible = false;

		for (unsigned int i = 0; i < visible.size(); ++i) {
			if (deviceMatchesEntry(deviceInstance, userData, deviceInstanceId, visible[i], visibleGUIDs[i])) {
				isVisible = true;
				break;
			}
		}

		if (!isVisible) {
			enumLogW(L"hidden:   \"%s\" %S (not in visible list)", deviceInstance->tszProductName, guidStr(deviceInstance->guidInstance).c_str());
			return DIENUM_CONTINUE;
		}
	}
	
	int nameMatchIndex = -1;

	for (unsigned int i = 0; i < order.size(); ++i) {
		MatchType match = deviceMatchesEntry(deviceInstance, userData, deviceInstanceId, order[i], orderGUIDs[i]);

		// Important that we prioritize a match via device instance ID or GUID over a match via name
		if (match == kDeviceInstanceIDMatch) {
			enumLogW(L"sorted:   \"%s\" %S (device instance ID)", deviceInstance->tszProductName, guidStr(deviceInstance->guidInstance).c_str());
			enumData->sorted[i].push_back(*deviceInstance);
			return DIENUM_CONTINUE;

		} else if (match == kGUIDMatch) {
			enumLogW(L"sorted:   \"%s\" %S (GUID)", deviceInstance->tszProductName, guidStr(deviceInstance->guidInstance).c_str());
			enumData->sorted[i].push_back(*deviceInstance);
			return DIENUM_CONTINUE;

		} else if (match == kNameMatch) {
			nameMatchIndex = i;
		}
	}

	if (nameMatchIndex != -1) {
		enumLogW(L"sorted:   \"%s\" %S (name)", deviceInstance->tszProductName, guidStr(deviceInstance->guidInstance).c_str());
		enumData->sorted[nameMatchIndex].push_back(*deviceInstance);
		return DIENUM_CONTINUE;
	}

	enumLogW(L"unsorted: \"%s\" %S", deviceInstance->tszProductName, guidStr(deviceInstance->guidInstance).c_str());
	enumData->nonsorted.push_back(*deviceInstance);
	return DIENUM_CONTINUE;
}

HRESULT STDMETHODCALLTYPE HookEnumDevicesA(LPDIRECTINPUT8A This, DWORD dwDevType, LPDIENUMDEVICESCALLBACKA lpCallback, LPVOID pvRef, DWORD dwFlags)
{
	vector<string> &order = sortedControllersA();
	DeviceEnumData<DIDEVICEINSTANCEA> enumData;
	enumData.sorted.resize(order.size());

	EnumCallbackUserData userData;
	userData.enumData = (void *)&enumData;
	userData.di = (void *)This;

	g_enumLog.clear();
	enumLog("EnumDevices (ANSI)");
	HRESULT result = TrueEnumDevicesA(This, dwDevType, enumCallbackA, (LPVOID)&userData, dwFlags);

	if (result != DI_OK) {
		enumLog("EnumDevices failed: 0x%08X", result);
	}

	writeEnumLog();

	if (result != DI_OK) return result;

	for(unsigned int i = 0; i < enumData.sorted.size(); ++i) {
		for (unsigned int j = 0; j < enumData.sorted[i].size(); ++j) {
			if (lpCallback(&enumData.sorted[i][j], pvRef) != DIENUM_CONTINUE) {
				return result;
			}
		}
	}

	for (unsigned int i = 0; i < enumData.nonsorted.size(); ++i) {
		if (lpCallback(&enumData.nonsorted[i], pvRef) != DIENUM_CONTINUE) {
			return result;
		}
	}

	return result;
}

HRESULT STDMETHODCALLTYPE HookEnumDevicesW(LPDIRECTINPUT8W This, DWORD dwDevType, LPDIENUMDEVICESCALLBACKW lpCallback, LPVOID pvRef, DWORD dwFlags)
{
	vector<wstring> &order = sortedControllersW();
	DeviceEnumData<DIDEVICEINSTANCEW> enumData;
	enumData.sorted.resize(order.size());

	EnumCallbackUserData userData;
	userData.enumData = (void *)&enumData;
	userData.di = (void *)This;

	g_enumLog.clear();
	enumLog("EnumDevices (Unicode)");
	HRESULT result = TrueEnumDevicesW(This, dwDevType, enumCallbackW, (LPVOID)&userData, dwFlags);

	if (result != DI_OK) {
		enumLog("EnumDevices failed: 0x%08X", result);
	}

	writeEnumLog();

	if (result != DI_OK) return result;

	for (unsigned int i = 0; i < enumData.sorted.size(); ++i) {
		for (unsigned int j = 0; j < enumData.sorted[i].size(); ++j) {
			if (lpCallback(&enumData.sorted[i][j], pvRef) != DIENUM_CONTINUE) {
				return result;
			}
		}
	}

	for (unsigned int i = 0; i < enumData.nonsorted.size(); ++i) {
		if (lpCallback(&enumData.nonsorted[i], pvRef) != DIENUM_CONTINUE) {
			return result;
		}
	}

	return result;
}

void CreateHooks(REFIID riidltf, LPVOID *realDI)
{
	PrintLog("devreorder: in CreateHooks");

	if (currentProcessIsIgnored()) {
		PrintLog("... current process is ignored, not hooking into DirectInput");
		return;
	}

	if (IsEqualIID(riidltf, IID_IDirectInput8A))
	{
		LPDIRECTINPUT8A pDIA = static_cast<LPDIRECTINPUT8A>(*realDI);

		if (pDIA)
		{
			PrintLog("devreorder: using ANSI interface");
			if (pDIA->lpVtbl->EnumDevices)
			{
				EnumDevicesA = pDIA->lpVtbl->EnumDevices;
				IH_CreateHook(EnumDevicesA, HookEnumDevicesA, &TrueEnumDevicesA);
				IH_EnableHook(EnumDevicesA);
			}
		}
	}
	else if (IsEqualIID(riidltf, IID_IDirectInput8W))
	{
		LPDIRECTINPUT8W pDIW = static_cast<LPDIRECTINPUT8W>(*realDI);

		if (pDIW)
		{
			PrintLog("devreorder: using UNICODE interface");

			if (pDIW->lpVtbl->EnumDevices)
			{
				EnumDevicesW = pDIW->lpVtbl->EnumDevices;
				IH_CreateHook(EnumDevicesW, HookEnumDevicesW, &TrueEnumDevicesW);
				IH_EnableHook(EnumDevicesW);
			}
		}
	}
}

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID *ppvOut, LPUNKNOWN punkOuter)
{
	PrintLog("devreorder: Calling hooked DirectInput8Create");
	HRESULT hr = DirectInputModuleManager::Get().DirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);

	if (hr != DI_OK) return hr;

	CreateHooks(riidltf, ppvOut);

	return hr;
}

extern "C" HRESULT WINAPI DllCanUnloadNow(void)
{
	return DirectInputModuleManager::Get().DllCanUnloadNow();
}

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID FAR* ppv)
{
	PrintLog("devreorder: Calling hooked DllGetClassObject");

	if (currentProcessIsIgnored()) {
		PrintLog("... current process is ignored, not hooking into DirectInput");
		return DirectInputModuleManager::Get().DllGetClassObject(rclsid, riid, ppv);
	}

	IClassFactory *cf;
	HRESULT hr = DirectInputModuleManager::Get().DllGetClassObject(rclsid, riid, (void**)&cf);

	if (hr != DI_OK) {
		return hr;
	}

	*ppv = cf;

	// Try Unicode first (most modern games), then fall back to ANSI
	IDirectInput8W *realDIW = nullptr;
	hr = cf->lpVtbl->CreateInstance(cf, NULL, IID_IDirectInput8W, (void**)&realDIW);

	if (hr == DI_OK && realDIW) {
		CreateHooks(IID_IDirectInput8W, (LPVOID *)&realDIW);
		realDIW->lpVtbl->Release(realDIW);
	} else {
		IDirectInput8A *realDIA = nullptr;
		hr = cf->lpVtbl->CreateInstance(cf, NULL, IID_IDirectInput8A, (void**)&realDIA);

		if (hr == DI_OK && realDIA) {
			CreateHooks(IID_IDirectInput8A, (LPVOID *)&realDIA);
			realDIA->lpVtbl->Release(realDIA);
		}
	}

	return S_OK;
}

extern "C" HRESULT WINAPI DllRegisterServer(void)
{
	return DirectInputModuleManager::Get().DllRegisterServer();
}

extern "C" HRESULT WINAPI DllUnregisterServer(void)
{
	return DirectInputModuleManager::Get().DllUnregisterServer();
}
