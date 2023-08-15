#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objidl.h>
#include "tp_stub.h"
#include "tvpsnd.h"
#if 0
#if defined(_M_IX86) || defined(_M_X64) || defined(_M_AMD64)
#include "MinHook.h"
#endif
#endif
#include "plthook.h"
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
static bool is_tpm = false;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD Reason, LPVOID lpReserved)
{
	if (Reason == DLL_PROCESS_ATTACH)
	{
		this_hmodule = hModule;
		if (hModule != NULL)
		{
			DisableThreadLibraryCalls(hModule);
		}
		WCHAR* modnamebuf = new WCHAR[32768];
		if (modnamebuf)
		{
			DWORD retLen = GetModuleFileNameW(hModule, modnamebuf, 32768);
			if (retLen)
			{
				const WCHAR* modname_tpm_ext = L".tpm";
				WCHAR* modnameloc = wcsstr(modnamebuf + (retLen - wcslen(modname_tpm_ext)), modname_tpm_ext);
				if (modnameloc)
				{
					is_tpm = true;
				}
			}
			delete[] modnamebuf;
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

#if 0
#if defined(_M_IX86) || defined(_M_X64) || defined(_M_AMD64)
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
#endif
#endif

	plthook_t *plthook;

	if (plthook_open(&plthook, NULL) != 0)
	{
		return E_FAIL;
	}
	// Make our IAT changes.
	plthook_replace(plthook, "LoadLibraryA", reinterpret_cast<LPVOID>(DetourLoadLibraryA), reinterpret_cast<LPVOID*>(&fpLoadLibraryA));
	plthook_replace(plthook, "GetFileAttributesA", reinterpret_cast<LPVOID>(DetourGetFileAttributesA), reinterpret_cast<LPVOID*>(&fpGetFileAttributesA));
	plthook_replace(plthook, "LoadLibraryW", reinterpret_cast<LPVOID>(DetourLoadLibraryW), reinterpret_cast<LPVOID*>(&fpLoadLibraryW));
	plthook_replace(plthook, "GetFileAttributesW", reinterpret_cast<LPVOID>(DetourGetFileAttributesW), reinterpret_cast<LPVOID*>(&fpGetFileAttributesW));
	plthook_replace(plthook, "GetProcAddress", reinterpret_cast<LPVOID>(DetourGetProcAddress), reinterpret_cast<LPVOID*>(&fpGetProcAddress));

	// Add the stub entry into the plugin list.
	TVPExecuteScript("global.Plugins.link('__krmemplugin_internal__.dll');");

	// Undo our IAT changes.
	plthook_replace(plthook, "LoadLibraryA", reinterpret_cast<LPVOID>(fpLoadLibraryA), NULL);
	plthook_replace(plthook, "GetFileAttributesA", reinterpret_cast<LPVOID>(fpGetFileAttributesA), NULL);
	plthook_replace(plthook, "LoadLibraryW", reinterpret_cast<LPVOID>(fpLoadLibraryW), NULL);
	plthook_replace(plthook, "GetFileAttributesW", reinterpret_cast<LPVOID>(fpGetFileAttributesW), NULL);
	plthook_replace(plthook, "GetProcAddress", reinterpret_cast<LPVOID>(fpGetProcAddress), NULL);

	plthook_close(plthook);

	if (this_ntdll_stub)
	{
		this_ntdll_stub = NULL;
	}
	else
	{
		return E_FAIL;
	}

	// At this point, we can prepare the TJS interface side by replacing the functions in the Plugins class with our own.
	iTJSDispatch2 *global_dispatch = TVPGetScriptDispatch();
	if (global_dispatch)
	{
		tTJSVariant varGlobal(global_dispatch);
		global_dispatch->Release();
		tTJSVariantClosure cloGlobal = varGlobal.AsObjectClosureNoAddRef();
		tTJSVariant varPlugins;
		cloGlobal.PropGet(0, TJS_W("Plugins"), NULL, &varPlugins, NULL);
		tTJSVariantClosure cloPlugins = varPlugins.AsObjectClosureNoAddRef();
		if (cloPlugins.Object)
		{
			tTJSVariant tmp;
			tTJSDispatch *method;

			method = new tPluginsLinkMem();
			tmp = tTJSVariant(method);
			method->Release();
			cloPlugins.PropSet(TJS_MEMBERENSURE, TJS_W("link"), NULL, &tmp, cloPlugins.Object);
			method = new tPluginsUnlinkMem();
			tmp = tTJSVariant(method);
			method->Release();
			cloPlugins.PropSet(TJS_MEMBERENSURE, TJS_W("unlink"), NULL, &tmp, cloPlugins.Object);
			method = new tPluginsGetListMem();
			tmp = tTJSVariant(method);
			method->Release();
			cloPlugins.PropSet(TJS_MEMBERENSURE, TJS_W("getList"), NULL, &tmp, cloPlugins.Object);

			cloPlugins.DeleteMember(0, TJS_W("prepare_krmemplugin_internal"), NULL, cloPlugins.Object);
			cloPlugins.DeleteMember(0, TJS_W("prepare_krmemplugin"), NULL, cloPlugins.Object);
			tmp = (tTVInteger)1;
			cloPlugins.PropSet(TJS_MEMBERENSURE|TJS_IGNOREPROP, TJS_W("krmemplugin_is_ready"), NULL, &tmp, cloPlugins.Object);
		}
	}

	return S_OK;
}

// Modified TVPGetXP3ArchiveOffset from XP3Archive.cpp
static bool IsXP3File(IStream *st)
{
	st->Seek({0}, STREAM_SEEK_SET, NULL);
	tjs_uint8 mark[11+1];
	static tjs_uint8 XP3Mark1[] =
		{ 0x58/*'X'*/, 0x50/*'P'*/, 0x33/*'3'*/, 0x0d/*'\r'*/,
		  0x0a/*'\n'*/, 0x20/*' '*/, 0x0a/*'\n'*/, 0x1a/*EOF*/,
		  0xff /* sentinel */, 
		// Extra junk data to break it up a bit (in case of compiler optimization)
		0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF
		};
	static tjs_uint8 XP3Mark2[] =
		{ 0x8b, 0x67, 0x01, 0xff/* sentinel */ };

	// XP3 header mark contains:
	// 1. line feed and carriage return to detect corruption by unnecessary
	//    line-feeds convertion
	// 2. 1A EOF mark which indicates file's text readable header ending.
	// 3. 8B67 KANJI-CODE to detect curruption by unnecessary code convertion
	// 4. 01 file structure version and character coding
	//    higher 4 bits are file structure version, currently 0.
	//    lower 4 bits are character coding, currently 1, is BMP 16bit Unicode.

	static tjs_uint8 XP3Mark[11+1];
		// +1: I was warned by CodeGuard that the code will do
		// access overrun... because a number of 11 is not aligned by DWORD, 
		// and the processor may read the value of DWORD at last of this array
		// from offset 8. Then the last 1 byte would cause a fail.
#if 0
	static bool DoInit = true;
	if(DoInit)
	{
		// the XP3 header above is splitted into two part; to avoid
		// mis-finding of the header in the program's initialized data area.
		DoInit = false;
		memcpy(XP3Mark, XP3Mark1, 8);
		memcpy(XP3Mark + 8, XP3Mark2, 3);
		// here joins it.
	}
#else
	if (memcmp(XP3Mark, XP3Mark1, 8))
	{
		memcpy(XP3Mark, XP3Mark1, 8);
		memcpy(XP3Mark + 8, XP3Mark2, 3);
	}
#endif

	mark[0] = 0; // sentinel
	st->Read(mark, 11, NULL);
	if(mark[0] == 0x4d/*'M'*/ && mark[1] == 0x5a/*'Z'*/)
	{
		// "MZ" is a mark of Win32/DOS executables,
		// TVP searches the first mark of XP3 archive
		// in the executeble file.
		bool found = false;

		st->Seek({16}, STREAM_SEEK_SET, NULL);

		// XP3 mark must be aligned by a paragraph ( 16 bytes )
		const tjs_uint one_read_size = 256*1024;
		ULONG read;
		tjs_uint8 buffer[one_read_size]; // read 256kbytes at once

		while(st->Read(buffer, one_read_size, &read) == S_OK && read != 0)
		{
			tjs_uint p = 0;
			while(p<read)
			{
				if(!memcmp(XP3Mark, buffer + p, 11))
				{
					// found the mark
					found = true;
					break;
				}
				p+=16;
			}
			if(found) break;
		}

		if(!found)
		{
			return false;
		}
	}
	else if(!memcmp(XP3Mark, mark, 11))
	{
	}
	else
	{
		return false;
	}

	return true;
}

EXPORT(HRESULT)
V2Link(iTVPFunctionExporter *exporter)
{
	TVPInitImportStub(exporter);
	this_exporter = exporter;

	ttstr pluginPath = TJS_W("krmemplugin.dll");
	{
		WCHAR* modnamebuf = new WCHAR[32768];
		if (modnamebuf)
		{
			if (this_hmodule)
			{
				DWORD ret_len = GetModuleFileNameW(this_hmodule, modnamebuf, 32768);
				if (ret_len)
				{
					pluginPath = modnamebuf;
				}
			}
			delete[] modnamebuf;
		}
	}

	{
		iTJSDispatch2 *global_dispatch = TVPGetScriptDispatch();
		if (global_dispatch)
		{
			tTJSVariant varGlobal(global_dispatch);
			global_dispatch->Release();
			tTJSVariantClosure cloGlobal = varGlobal.AsObjectClosureNoAddRef();
			tTJSVariant varPlugins;
			cloGlobal.PropGet(TJS_MEMBERMUSTEXIST, TJS_W("Plugins"), NULL, &varPlugins, NULL);
			tTJSVariantClosure cloPlugins = varPlugins.AsObjectClosureNoAddRef();
			if (cloPlugins.Object)
			{
				tTJSVariant tmp;
				tTJSDispatch *method;
				tmp = (tTVInteger)0;
				cloPlugins.PropSet(TJS_MEMBERENSURE, TJS_W("krmemplugin_is_ready"), NULL, &tmp, cloPlugins.Object);
				tmp = pluginPath;
				cloPlugins.PropSet(TJS_MEMBERENSURE, TJS_W("krmemplugin_path"), NULL, &tmp, cloPlugins.Object);
				method = new tPluginsPrepareKrMemPlugin();
				tmp = tTJSVariant(method);
				method->Release();
				cloPlugins.PropSet(TJS_MEMBERENSURE, TJS_W("prepare_krmemplugin_internal"), NULL, &tmp, cloPlugins.Object);
				TVPExecuteScript("global.Plugins.prepare_krmemplugin=function{var old_unlink=global.Plugins.unlink;global.Plugins.prepare_krmemplugin_internal();old_unlink(global.Storages.extractStorageName(global.Plugins.krmemplugin_path));};");
			}
		}

	}

	if (is_tpm)
	{
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
					arcname += TJS_W(">");
					ttstr normarcname = TVPNormalizeStorageName(arcname);
					IStream *in = TVPCreateIStream(normmodname, TJS_BS_READ);
					if (in)
					{
						if (IsXP3File(in))
						{
							TVPSetCurrentDirectory(normarcname);
						}
						in->Release();
					}
				}
			}
			delete[] modnamebuf;
		}
	}

	return S_OK;
}

EXPORT(HRESULT)
V2Unlink(void)
{
	iTJSDispatch2 *global_dispatch = TVPGetScriptDispatch();
	if (global_dispatch)
	{
		tTJSVariant varGlobal(global_dispatch);
		global_dispatch->Release();
		tTJSVariantClosure cloGlobal = varGlobal.AsObjectClosureNoAddRef();
		tTJSVariant varPlugins;
		cloGlobal.PropGet(TJS_MEMBERMUSTEXIST, TJS_W("Plugins"), NULL, &varPlugins, NULL);
		tTJSVariantClosure cloPlugins = varPlugins.AsObjectClosureNoAddRef();
		if (cloPlugins.Object)
		{
			cloPlugins.DeleteMember(0, TJS_W("prepare_krmemplugin_internal"), NULL, cloPlugins.Object);
			cloPlugins.DeleteMember(0, TJS_W("prepare_krmemplugin"), NULL, cloPlugins.Object);
		}
	}
	this_exporter = NULL;
	TVPUninitImportStub();
	return S_OK;
}

