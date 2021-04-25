#include <windows.h>
#include "tp_stub.h"
#include "tvpsnd.h"
#include "MinHook.h"
#include "MemoryModule.h"
#include <vector>
#include <algorithm>

#define EXPORT(hr) extern "C" __declspec(dllexport) hr __stdcall

extern "C"
{
	typedef HRESULT (__stdcall * tTVPV2LinkProc)(iTVPFunctionExporter *);
	typedef HRESULT (__stdcall * tTVPGetModuleInstanceProc)(ITSSModule **out,
		ITSSStorageProvider *provider, IStream * config, HWND mainwin);
}

static HMODULE this_hmodule = NULL;
static iTVPFunctionExporter *this_exporter = NULL;
static ITSSStorageProvider *this_storageprovider = NULL;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD Reason, LPVOID lpReserved)
{
	if (Reason == DLL_PROCESS_ATTACH)
	{
		this_hmodule = hModule;
		if (hModule != NULL)
		{
			DisableThreadLibraryCalls(hModule);
		}
	}
	return TRUE;
}

struct tTVPMemPlugin
{
	ttstr Name;
	HMEMORYMODULE Instance;

	ITSSModule *TSSModule;

	tTVPV2LinkProc V2Link;

	tTVPGetModuleInstanceProc GetModuleInstance;

	std::vector<ttstr> SupportedExts;

	tTVPMemPlugin(const ttstr & name);
	~tTVPMemPlugin();

	bool Uninit();
};

tTVPMemPlugin::tTVPMemPlugin(const ttstr & name)
{
	Name = name;

	Instance = NULL;
	TSSModule = NULL;

	V2Link = NULL;

	GetModuleInstance = NULL;

	// load DLL
	BYTE* data = NULL;
	ULONG size = 0;
	{
		ttstr normmodname = TVPNormalizeStorageName(name);
		IStream *in = TVPCreateIStream(normmodname, TJS_BS_READ);
		if (in)
		{
			STATSTG stat;
			in->Stat(&stat, STATFLAG_NONAME);
			size = (ULONG)(stat.cbSize.QuadPart);
			data = new BYTE[size];
			HRESULT read_result = in->Read(data, size, &size);
			in->Release();
			if (read_result != S_OK)
			{
				delete[] data;
				TVPThrowExceptionMessage(TJS_W("Cannot load Plugin %1"), name);
			}
		}
	}

	if (data)
	{
		Instance = MemoryLoadLibrary(data, size);
		delete[] data;
		if (!Instance)
		{
			TVPThrowExceptionMessage(TJS_W("Cannot load Plugin %1"), name);
		}
	}
	else
	{
		TVPThrowExceptionMessage(TJS_W("Cannot load Plugin %1"), name);
	}

	try
	{
		// retrieve each functions
		V2Link = (tTVPV2LinkProc)
			MemoryGetProcAddress(Instance, "V2Link");

		GetModuleInstance = (tTVPGetModuleInstanceProc)
			MemoryGetProcAddress(Instance, "GetModuleInstance");

		// link
		if(V2Link)
		{
			V2Link(this_exporter);
		}

		if(GetModuleInstance)
		{
			HRESULT hr = GetModuleInstance(&TSSModule, this_storageprovider,
				 NULL, TVPGetApplicationWindowHandle());
			if(FAILED(hr) || TSSModule == NULL)
				TVPThrowExceptionMessage(TJS_W("Cannot load Plugin %1"), name);

			// get supported extensions
			unsigned long index = 0;
			while(true)
			{
				tjs_char mediashortname[33];
				tjs_char buf[256];
				HRESULT hr = TSSModule->GetSupportExts(index,
					mediashortname, buf, 255);
				if(hr == S_OK)
				{
					ttstr buf_lowercase = buf;
					buf_lowercase.ToLowerCase();
					SupportedExts.push_back(buf_lowercase);
				}
				else
					break;
				index ++;
			}
		}
	}
	catch(...)
	{
		MemoryFreeLibrary(Instance);
		throw;
	}
}

tTVPMemPlugin::~tTVPMemPlugin()
{
}

typedef std::vector<tTVPMemPlugin*> tTVPMemPluginVectorType;
struct tTVPMemPluginVectorStruc
{
	tTVPMemPluginVectorType Vector;
} static TVPMemPluginVector;

static bool TVPMemPluginLoading = false;
static void TVPLoadMemPlugin(const ttstr & name)
{
	// load plugin
	if(TVPMemPluginLoading)
		TVPThrowExceptionMessage(TJS_W("Cannot link plugin while plugin linking"));
			// linking plugin while other plugin is linking, is prohibited
			// by data security reason.

	// check whether the same plugin was already loaded
	tTVPMemPluginVectorType::iterator i;
	for(i = TVPMemPluginVector.Vector.begin();
		i != TVPMemPluginVector.Vector.end(); i++)
	{
		if((*i)->Name == name) return;
	}

	tTVPMemPlugin * p;

	try
	{
		TVPMemPluginLoading = true;
		p = new tTVPMemPlugin(name);
		TVPMemPluginLoading = false;
	}
	catch(...)
	{
		TVPMemPluginLoading = false;
		throw;
	}

	TVPMemPluginVector.Vector.push_back(p);
}

static ITSSWaveDecoder * TVPSearchAvailTSSWaveDecoderMem(const ttstr & storage, const ttstr & extension)
{
	tTVPMemPluginVectorType::iterator i;
	for(i = TVPMemPluginVector.Vector.begin();
		i != TVPMemPluginVector.Vector.end(); i++)
	{
		if((*i)->TSSModule)
		{
			// check whether the plugin supports extension
			bool supported = false;
			std::vector<ttstr>::iterator ei;
			for(ei = (*i)->SupportedExts.begin(); ei != (*i)->SupportedExts.end(); ei++)
			{
				if(ei->GetLen() == 0) { supported = true; break; }
				if(extension == *ei) { supported = true; break; }
			}

			if(!supported) continue;

			// retrieve instance from (*i)->TSSModule
			IUnknown *intf = NULL;
			HRESULT hr = (*i)->TSSModule->GetMediaInstance(
				(tjs_char*)storage.c_str(), &intf);
			if(SUCCEEDED(hr))
			{
				try
				{
					// check  whether the instance has IID_ITSSWaveDecoder
					// interface.
					ITSSWaveDecoder * decoder;
					if(SUCCEEDED(intf->QueryInterface(IID_ITSSWaveDecoder,
						(void**) &decoder)))
					{
						intf->Release();
						return decoder; // OK
					}
				}
				catch(...)
				{
				}
				intf->Release();
			}

		}
	}
	return NULL; // not found
}


class KrMemPluginStubModule : public ITSSModule
{
	ULONG RefCount;

public:
	KrMemPluginStubModule();
	virtual ~KrMemPluginStubModule();

public:
	// IUnknown
	HRESULT __stdcall QueryInterface(REFIID iid, void **ppvObject);
	ULONG __stdcall AddRef(void);
	ULONG __stdcall Release(void);

	// ITSSModule
	HRESULT __stdcall GetModuleCopyright(LPWSTR buffer, unsigned long buflen);
	HRESULT __stdcall GetModuleDescription(LPWSTR buffer, unsigned long buflen);
	HRESULT __stdcall GetSupportExts(unsigned long index, LPWSTR mediashortname, LPWSTR buf, unsigned long buflen);
	HRESULT __stdcall GetMediaInfo(LPWSTR url, ITSSMediaBaseInfo **info);
	HRESULT __stdcall GetMediaSupport(LPWSTR url);
	HRESULT __stdcall GetMediaInstance(LPWSTR url, IUnknown **instance);
};

KrMemPluginStubModule::KrMemPluginStubModule()
{
	RefCount = 1;
}

KrMemPluginStubModule::~KrMemPluginStubModule()
{
}

HRESULT __stdcall KrMemPluginStubModule::QueryInterface(REFIID iid, void **ppvObject)
{
	return E_NOTIMPL;
}

ULONG __stdcall KrMemPluginStubModule::AddRef()
{
	return ++RefCount;
}

ULONG __stdcall KrMemPluginStubModule::Release()
{
	if (RefCount == 1)
	{
		delete this;
		return 0;
	}
	else
	{
		return --RefCount;
	}
}

HRESULT __stdcall KrMemPluginStubModule::GetModuleCopyright(LPWSTR buffer, unsigned long buflen)
{
	return E_NOTIMPL;
}

HRESULT __stdcall KrMemPluginStubModule::GetModuleDescription(LPWSTR buffer, unsigned long buflen)
{
	return E_NOTIMPL;
}

HRESULT __stdcall KrMemPluginStubModule::GetSupportExts(unsigned long index, LPWSTR mediashortname, LPWSTR buf, unsigned long buflen)
{
	if (index >= 1)
	{
		return S_FALSE;
	}
	wcsncpy(buf, TEXT(""), buflen); // Allow any extension.
	return S_OK;
}

HRESULT __stdcall KrMemPluginStubModule::GetMediaInfo(LPWSTR url, ITSSMediaBaseInfo **info)
{
	return E_NOTIMPL;
}

HRESULT __stdcall KrMemPluginStubModule::GetMediaSupport(LPWSTR url)
{
	return E_NOTIMPL;
}

HRESULT __stdcall KrMemPluginStubModule::GetMediaInstance(LPWSTR url, IUnknown **instance)
{
	ttstr storage(url);
	ttstr ext(TVPExtractStorageExt(url));
	ext.ToLowerCase();
	ITSSWaveDecoder * res = TVPSearchAvailTSSWaveDecoderMem(storage, ext);
	if (res)
	{
		*instance = (IUnknown*)res;
		return S_OK;
	}
	return E_NOTIMPL;
}

EXPORT(HRESULT)
GetModuleInstanceInternal(ITSSModule **out, ITSSStorageProvider *provider, IStream *config, HWND mainwin)
{
	this_storageprovider = provider;
	*out = new KrMemPluginStubModule();
	return S_OK;
}


static HMODULE this_ntdll_stub = NULL;


template <typename T>
inline MH_STATUS MH_CreateHookEx(LPVOID pTarget, LPVOID pDetour, T** ppOriginal)
{
    return MH_CreateHook(pTarget, pDetour, reinterpret_cast<LPVOID*>(ppOriginal));
}

template <typename T>
inline MH_STATUS MH_CreateHookApiEx(
    LPCWSTR pszModule, LPCSTR pszProcName, LPVOID pDetour, T** ppOriginal)
{
    return MH_CreateHookApi(
        pszModule, pszProcName, pDetour, reinterpret_cast<LPVOID*>(ppOriginal));
}

typedef HMODULE (WINAPI *LOADLIBRARYA)(LPCSTR);
static LOADLIBRARYA fpLoadLibraryA = NULL;

static HMODULE WINAPI DetourLoadLibraryA(LPCSTR lpLibFileName)
{
	if (strcmp(lpLibFileName, "__krmemplugin_internal__.dll"))
	{
		// It might be good to return a loadlibrary to ntdll.dll so that the incremented/deincremented will match.
		this_ntdll_stub = fpLoadLibraryA("ntdll.dll");
		return this_ntdll_stub;
	}
	return fpLoadLibraryA(lpLibFileName);
}

typedef DWORD (WINAPI *GETFILEATTRIBUTESA)(LPCSTR);
static GETFILEATTRIBUTESA fpGetFileAttributesA = NULL;

static DWORD WINAPI DetourGetFileAttributesA(LPCSTR lpFileName)
{
	if (strcmp(lpFileName, "__krmemplugin_internal__.dll"))
	{
		// Always return this is a file
		return 0;
	}
	return fpGetFileAttributesA(lpFileName);
}

typedef HMODULE (WINAPI *LOADLIBRARYW)(LPCWSTR);
static LOADLIBRARYW fpLoadLibraryW = NULL;

static HMODULE WINAPI DetourLoadLibraryW(LPCWSTR lpLibFileName)
{
	if (wcsstr(lpLibFileName, L"__krmemplugin_internal__.dll"))
	{
		// It might be good to return a loadlibrary to ntdll.dll so that the incremented/deincremented will match.
		this_ntdll_stub = fpLoadLibraryW(L"ntdll.dll");
		return this_ntdll_stub;
	}
	return fpLoadLibraryW(lpLibFileName);
}

typedef DWORD (WINAPI *GETFILEATTRIBUTESW)(LPCWSTR);
static GETFILEATTRIBUTESW fpGetFileAttributesW = NULL;

static DWORD WINAPI DetourGetFileAttributesW(LPCWSTR lpFileName)
{
	if (wcsstr(lpFileName, L"__krmemplugin_internal__.dll"))
	{
		// Always return this is a file
		return 0;
	}
	return fpGetFileAttributesW(lpFileName);
}

typedef FARPROC (WINAPI *GETPROCADDRESS)(HMODULE, LPCSTR);
static GETPROCADDRESS fpGetProcAddress = NULL;

static FARPROC WINAPI DetourGetProcAddress(HMODULE hModule, LPCSTR lpProcName)
{
	if (hModule == this_ntdll_stub && !strcmp(lpProcName, "GetModuleInstance"))
	{
		// We can add our implementation of GetModuleInstance here to allow wave unpacker plugins to work in memory.
		return (FARPROC)&GetModuleInstanceInternal;
	}
	return fpGetProcAddress(hModule, lpProcName);
}


static void
addMethod(iTJSDispatch2 *dispatch, const tjs_char *methodName, tTJSDispatch *method)
{
	tTJSVariant var = tTJSVariant(method);
	method->Release();
	dispatch->PropSet(
		TJS_MEMBERENSURE, // メンバがなかった場合には作成するようにするフラグ
		methodName, // メンバ名 ( かならず TJS_W( ) で囲む )
		NULL, // ヒント ( 本来はメンバ名のハッシュ値だが、NULL でもよい )
		&var, // 登録する値
		dispatch // コンテキスト
		);
}

static void
delMethod(iTJSDispatch2 *dispatch, const tjs_char *methodName)
{
	dispatch->DeleteMember(
		0, // フラグ ( 0 でよい )
		methodName, // メンバ名
		NULL, // ヒント
		dispatch // コンテキスト
		);
}


class tPluginsLinkMem : public tTJSDispatch
{
protected:
public:
	tjs_error TJS_INTF_METHOD FuncCall(
		tjs_uint32 flag, const tjs_char * membername, tjs_uint32 *hint,
		tTJSVariant *result,
		tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *objthis) {

		if (membername) return TJS_E_MEMBERNOTFOUND;
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;

		ttstr name = *param[0];

		TVPLoadMemPlugin(name);
		return TJS_S_OK;
	}
};

class tPluginsUnlinkMem : public tTJSDispatch
{
protected:
public:
	tjs_error TJS_INTF_METHOD FuncCall(
		tjs_uint32 flag, const tjs_char * membername, tjs_uint32 *hint,
		tTJSVariant *result,
		tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *objthis) {
		// stub
		return TJS_S_OK;
	}
};


class tPluginsGetListMem : public tTJSDispatch
{
protected:
public:
	tjs_error TJS_INTF_METHOD FuncCall(
		tjs_uint32 flag, const tjs_char * membername, tjs_uint32 *hint,
		tTJSVariant *result,
		tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *objthis) {
		iTJSDispatch2 * array = TJSCreateArrayObject();
		try
		{
			tTVPMemPluginVectorType::iterator i;
			tjs_int idx = 0;
			for(i = TVPMemPluginVector.Vector.begin(); i != TVPMemPluginVector.Vector.end(); i++)
			{
				tTJSVariant val = (*i)->Name.c_str();
				array->PropSetByNum(TJS_MEMBERENSURE, idx++, &val, array);
			}
		
			if (result) *result = tTJSVariant(array, array);
		}
		catch(...)
		{
			array->Release();
			throw;
		}
		array->Release();
		return TJS_S_OK;
	}
};


class tPluginsPrepareKrMemPlugin : public tTJSDispatch
{
protected:
public:
	tjs_error TJS_INTF_METHOD FuncCall(
		tjs_uint32 flag, const tjs_char * membername, tjs_uint32 *hint,
		tTJSVariant *result,
		tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *objthis) {
		// Since TVPPluginLoading is not true here, we can perform our injecton stuff here.
		// Read our own library
		BYTE* data = NULL;
		ULONG size = 0;
		WCHAR* modnamebuf = new WCHAR[32768];
		if (modnamebuf)
		{
			if (this_hmodule)
			{
				DWORD ret_len = GetModuleFileNameW(this_hmodule, modnamebuf, 32768);
				if (ret_len)
				{
					ttstr arcname = modnamebuf;
					ttstr normmodname = TVPNormalizeStorageName(modnamebuf);
					IStream *in = TVPCreateIStream(normmodname, TJS_BS_READ);
					if (in)
					{
						STATSTG stat;
						in->Stat(&stat, STATFLAG_NONAME);
						size = (ULONG)(stat.cbSize.QuadPart);
						data = new BYTE[size];
						HRESULT read_result = in->Read(data, size, &size);
						in->Release();
						if (read_result != S_OK)
						{
							delete[] data;
							return TJS_S_OK;
						}
					}
				}
			}
			delete[] modnamebuf;
		}
		if (data)
		{
			// Yes, We will leak library memory. This library is intended to be loaded only once.
			HMEMORYMODULE library = MemoryLoadLibrary(data, size);
			if (library)
			{
				tTVPV2LinkProc V2LinkInternal = (tTVPV2LinkProc)MemoryGetProcAddress(library, "V2LinkInternal");
				if (V2LinkInternal)
				{
					V2LinkInternal(this_exporter);
				}
			}
			delete[] data;
		}
		return TJS_S_OK;
	}
};


EXPORT(HRESULT)
V2LinkInternal(iTVPFunctionExporter *exporter)
{
	// Internal V2Link for when this is loaded in memory
	TVPInitImportStub(exporter);
	this_exporter = exporter;

	// We need GetModuleInstance:
	// We need to override GetFileAttributes, GetProcAddress and LoadLibrary.
	MH_Initialize();

	MH_CreateHookApi(L"kernel32.dll", "LoadLibraryA", reinterpret_cast<LPVOID>(&DetourLoadLibraryA), reinterpret_cast<LPVOID*>(&fpLoadLibraryA));
	MH_CreateHookApi(L"kernel32.dll", "GetFileAttributesA", reinterpret_cast<LPVOID>(&DetourGetFileAttributesA), reinterpret_cast<LPVOID*>(&fpGetFileAttributesA));
	MH_CreateHookApi(L"kernel32.dll", "LoadLibraryW", reinterpret_cast<LPVOID>(&DetourLoadLibraryW), reinterpret_cast<LPVOID*>(&fpLoadLibraryW));
	MH_CreateHookApi(L"kernel32.dll", "GetFileAttributesW", reinterpret_cast<LPVOID>(&DetourGetFileAttributesW), reinterpret_cast<LPVOID*>(&fpGetFileAttributesW));
	MH_CreateHookApi(L"kernel32.dll", "GetProcAddress", reinterpret_cast<LPVOID>(&DetourGetProcAddress), reinterpret_cast<LPVOID*>(&fpGetProcAddress));

	MH_EnableHook(MH_ALL_HOOKS);

	// Add the stub entry into the plugin list.
	TVPExecuteScript("global.Plugins.link('__krmemplugin_internal__.dll');");

	MH_DisableHook(MH_ALL_HOOKS);

	MH_Uninitialize();

	if (this_ntdll_stub)
	{
		this_ntdll_stub = NULL;
	}
	else
	{
		return E_FAIL;
	}

	// At this point, we can prepare the TJS interface side by replacing the functions in the Plugins class with our own.
	{
		tTJSVariant varScripts;
		TVPExecuteExpression(TJS_W("Plugins"), &varScripts);
		iTJSDispatch2 *dispatch = varScripts.AsObjectNoAddRef();
		if (dispatch) {
			addMethod(dispatch, TJS_W("link"), new tPluginsLinkMem());
			addMethod(dispatch, TJS_W("unlink"), new tPluginsUnlinkMem());
			addMethod(dispatch, TJS_W("getList"), new tPluginsGetListMem());
			delMethod(dispatch, TJS_W("prepare_krmemplugin_internal"));
			delMethod(dispatch, TJS_W("prepare_krmemplugin"));
			tTJSVariant one = (tTVInteger)1;
			dispatch->PropSet(TJS_MEMBERENSURE|TJS_IGNOREPROP, TJS_W("krmemplugin_is_ready"), NULL, &one, dispatch);
		}
	}

	return S_OK;
}

EXPORT(HRESULT)
V2Link(iTVPFunctionExporter *exporter)
{
	TVPInitImportStub(exporter);
	this_exporter = exporter;

	{
		tTJSVariant varScripts;
		TVPExecuteExpression(TJS_W("Plugins"), &varScripts);
		iTJSDispatch2 *dispatch = varScripts.AsObjectNoAddRef();
		if (dispatch)
		{
			tTJSVariant zero = (tTVInteger)0;
			dispatch->PropSet(TJS_MEMBERENSURE|TJS_IGNOREPROP, TJS_W("krmemplugin_is_ready"), NULL, &zero, dispatch);
			addMethod(dispatch, TJS_W("prepare_krmemplugin_internal"), new tPluginsPrepareKrMemPlugin());
			TVPExecuteScript("global.Plugins.prepare_krmemplugin=function{var old_unlink=global.Plugins.unlink;global.Plugins.prepare_krmemplugin_internal();old_unlink('krmemplugin.dll');};");
		}
	}

	return S_OK;
}

EXPORT(HRESULT)
V2Unlink(void)
{
	{
		tTJSVariant varScripts;
		TVPExecuteExpression(TJS_W("Plugins"), &varScripts);
		iTJSDispatch2 *dispatch = varScripts.AsObjectNoAddRef();
		if (dispatch)
		{
			delMethod(dispatch, TJS_W("prepare_krmemplugin_internal"));
			delMethod(dispatch, TJS_W("prepare_krmemplugin"));
		}
	}
	this_exporter = NULL;
	TVPUninitImportStub();
	return S_OK;
}

