#include "Logger.h"
#include "SC4VersionDetection.h"
#include "version.h"
#include "cIGZCOM.h"
#include "cRZCOMDllDirector.h"
#include <Windows.h>
#include "wil/resource.h"
#include "wil/win32_helpers.h"
#include "cISC4City.h"


#ifdef __clang__
#define NAKED_FUN __attribute__((naked))
#else
#define NAKED_FUN __declspec(naked)
#endif


#define ROWS_VERTEX_COUNT_MAX 257

static constexpr uint32_t kThumbnailFixDllDirectorID = 0xD25A91D5;

static constexpr std::string_view PluginLogFileName = "memo.thumbnail-fix.log";


// static constexpr uint32_t SC4WriteCityRegionViewThumbnail_InjectPoint = 0x5de2db;
// static constexpr uint32_t SC4WriteCityRegionViewThumbnail_ContinueJump = 0x5de2e0;
static constexpr uint32_t ComputeDrawRectsForDrawFrustum_InjectPoint = 0x752f43;
static constexpr uint32_t ComputeDrawRectsForDrawFrustum_ContinueJump = 0x752f49;
static constexpr uint32_t WriteRegionViewThumbnail_InjectPoint = 0x459d4e;


namespace
{
	std::filesystem::path GetDllFolderPath()
	{
		wil::unique_cotaskmem_string modulePath = wil::GetModuleFileNameW(wil::GetModuleInstanceHandle());

		std::filesystem::path temp(modulePath.get());

		return temp.parent_path();
	}

	void InstallHook(uint32_t address, void (*pfnFunc)(void))
	{
		DWORD oldProtect;
		THROW_IF_WIN32_BOOL_FALSE(VirtualProtect((void *)address, 5, PAGE_EXECUTE_READWRITE, &oldProtect));
		*((uint8_t*)address) = 0xE9;
		*((uint32_t*)(address+1)) = ((uint32_t)pfnFunc) - address - 5;
	}

	void UninstallHook(uint32_t address, uint8_t uFirstByte, uint32_t uOtherBytes)
	{
		DWORD oldProtect;
		THROW_IF_WIN32_BOOL_FALSE(VirtualProtect((void *)address, 5, PAGE_EXECUTE_READWRITE, &oldProtect));
		*((uint8_t*)address) = uFirstByte;
		*((uint32_t*)(address+1)) = uOtherBytes;
	}

	void InstallCallHook(uint32_t address, void (*pfnFunc)(void))
	{
		DWORD oldProtect;
		THROW_IF_WIN32_BOOL_FALSE(VirtualProtect((void *)address, 5, PAGE_EXECUTE_READWRITE, &oldProtect));
		*((uint8_t*)address) = 0xE8;
		*((uint32_t*)(address+1)) = ((uint32_t)pfnFunc) - address - 5;
	}

	void NAKED_FUN Hook_ComputeDrawRectsForDrawFrustum(void)
	{
		// Overwrite number of rows to draw by maximum.
		// (this is subsequently lowered for small/medium city tiles, so there's no problem in setting this to 257, the value for large city tiles)
		__asm {
			mov dword ptr [edi + 0x90], ROWS_VERTEX_COUNT_MAX;
			mov eax, ROWS_VERTEX_COUNT_MAX;
			push ComputeDrawRectsForDrawFrustum_ContinueJump;
			ret;
		}
	}

	typedef uint32_t(* pfn_SC4WriteCityRegionViewThumbnail)(cIGZPersistDBSegment* persistDbSegment);

	pfn_SC4WriteCityRegionViewThumbnail SC4WriteCityRegionViewThumbnail_orig = reinterpret_cast<pfn_SC4WriteCityRegionViewThumbnail>(0x5ddec0);

	uint32_t SC4WriteCityRegionViewThumbnail_wrapper(cIGZPersistDBSegment* persistDbSegment)
	{
		// The actual patch is only applied for the duration of creating the thumbnail to avoid a performance impact during normal gameplay.
		Logger& logger = Logger::GetInstance();
		try {
			InstallHook(ComputeDrawRectsForDrawFrustum_InjectPoint, Hook_ComputeDrawRectsForDrawFrustum);
			logger.WriteLine(LogLevel::Info, "Installed temporary patch for rendering thumbnail.");
		} catch (const wil::ResultException& e) {
			logger.WriteLineFormatted(LogLevel::Error, "Failed to install temporary patch for rendering thumbnail.\n%s", e.what());
		}

		auto result = SC4WriteCityRegionViewThumbnail_orig(persistDbSegment);

		try {
			UninstallHook(ComputeDrawRectsForDrawFrustum_InjectPoint, 0x89, 0x00009087);
			logger.WriteLine(LogLevel::Info, "Uninstalled temporary patch for rendering thumbnail.");
		} catch (const wil::ResultException& e) {
			logger.WriteLineFormatted(LogLevel::Error, "Failed to uninstall temporary patch for rendering thumbnail.\n%s", e.what());
		}
		return result;
	}

	// void NAKED_FUN Hook_SC4WriteCityRegionViewThumbnail(void)
	// {
	// 	// overwrite magnification for testing on smaller screens (<=1024 vertical resolution)
	// 	__asm {
	// 		push 0x3e000000;  // 0.125
	// 		push SC4WriteCityRegionViewThumbnail_ContinueJump;
	// 		ret;
	// 	}
	// }

	void InstallPatches()
	{
		Logger& logger = Logger::GetInstance();
		try
		{
			// InstallHook(SC4WriteCityRegionViewThumbnail_InjectPoint, Hook_SC4WriteCityRegionViewThumbnail);  // overwrite magnification for testing on smaller screens
			InstallCallHook(WriteRegionViewThumbnail_InjectPoint, reinterpret_cast<void(*)(void)>(SC4WriteCityRegionViewThumbnail_wrapper));
			logger.WriteLine(LogLevel::Info, "Installed Region View Thumbnail Fix.");
		}
		catch (const wil::ResultException& e)
		{
			logger.WriteLineFormatted(LogLevel::Error, "Failed to install Region View Thumbnail Fix.\n%s", e.what());
		}
	}
}


class ThumbnailFixDllDirector final : public cRZCOMDllDirector
{
public:

	ThumbnailFixDllDirector()
	{
		std::filesystem::path dllFolderPath = GetDllFolderPath();

		std::filesystem::path logFilePath = dllFolderPath;
		logFilePath /= PluginLogFileName;

		Logger& logger = Logger::GetInstance();
		logger.Init(logFilePath, LogLevel::Error);
		logger.WriteLogFileHeader("Region Thumbnail Fix DLL " PLUGIN_VERSION_STR);
	}

	uint32_t GetDirectorID() const
	{
		return kThumbnailFixDllDirectorID;
	}

	bool OnStart(cIGZCOM* pCOM)
	{
		const uint16_t gameVersion = versionDetection.GetGameVersion();
		if (gameVersion == 641)
		{
			InstallPatches();
		}
		else
		{
			Logger& logger = Logger::GetInstance();
			logger.WriteLineFormatted(
				LogLevel::Error,
				"Requires game version 641, found game version %d.",
				gameVersion);
		}
		return true;
	}

private:

	const SC4VersionDetection versionDetection;
};

cRZCOMDllDirector* RZGetCOMDllDirector() {
	static ThumbnailFixDllDirector sDirector;
	return &sDirector;
}