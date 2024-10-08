#include "Logger.h"
#include "SC4VersionDetection.h"
#include "version.h"
#include "cIGZCOM.h"
#include "cRZCOMDllDirector.h"
#include <Windows.h>
#include "wil/resource.h"
#include "wil/win32_helpers.h"


#ifdef __clang__
#define NAKED_FUN __attribute__((naked))
#else
#define NAKED_FUN __declspec(naked)
#endif

// uncomment for testing on smaller screen sizes
// #define DEBUG_SMALL_SCREEN

#define ROWS_VERTEX_COUNT_MAX 257

static constexpr uint32_t kThumbnailFixDllDirectorID = 0xD25A91D5;

static constexpr std::string_view PluginLogFileName = "memo.thumbnail-fix.log";

#ifdef DEBUG_SMALL_SCREEN
static constexpr uint32_t SC4WriteCityRegionViewThumbnail_InjectPoint = 0x5de2db;
static constexpr uint32_t SC4WriteCityRegionViewThumbnail_ContinueJump = 0x5de2e0;
#endif
static constexpr uint32_t ComputeDrawRectsForDrawFrustum_InjectPoint = 0x752f43;
static constexpr uint32_t ComputeDrawRectsForDrawFrustum_ContinueJump = 0x752f49;
static constexpr uint32_t WriteRegionViewThumbnail_InjectPoint = 0x459d4e;
static constexpr uint32_t WriteRegionViewThumbnail_ContinueJump = 0x459d53;
static constexpr uint32_t SC4WriteCityRegionViewThumbnail_address = 0x5ddec0;

static bool activelyWritingThumbnail = false;

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

	void NAKED_FUN Hook_ComputeDrawRectsForDrawFrustum(void)
	{
		__asm {
			mov dl, byte ptr [activelyWritingThumbnail];
			test dl, dl;
			jz skipOverwrite;
			// Overwrite number of rows to draw by maximum when writing thumbnail.
			// (this is subsequently lowered for small/medium city tiles, so there's no problem in setting this to 257, the value for large city tiles)
			mov eax, ROWS_VERTEX_COUNT_MAX;
skipOverwrite:
			mov dword ptr [edi + 0x90], eax;
			push ComputeDrawRectsForDrawFrustum_ContinueJump;  // edx/dl is reassigned directly after
			ret;
		}
	}

	void NAKED_FUN Hook_WriteRegionViewThumbnail(void)
	{
		__asm {
			mov byte ptr [activelyWritingThumbnail], 1;
			call dword ptr [SC4WriteCityRegionViewThumbnail_address];
			mov byte ptr [activelyWritingThumbnail], 0;
			push WriteRegionViewThumbnail_ContinueJump;
			ret;
		}
	}

#ifdef DEBUG_SMALL_SCREEN
	void NAKED_FUN Hook_SC4WriteCityRegionViewThumbnail(void)
	{
		// overwrite magnification for testing on smaller screens (<=1024 vertical resolution)
		__asm {
			push 0x3e000000;  // 0.125
			push SC4WriteCityRegionViewThumbnail_ContinueJump;
			ret;
		}
	}
#endif

	void InstallPatches()
	{
		Logger& logger = Logger::GetInstance();
		try
		{
#ifdef DEBUG_SMALL_SCREEN
			InstallHook(SC4WriteCityRegionViewThumbnail_InjectPoint, Hook_SC4WriteCityRegionViewThumbnail);  // overwrite magnification for testing on smaller screens
#endif
			InstallHook(ComputeDrawRectsForDrawFrustum_InjectPoint, Hook_ComputeDrawRectsForDrawFrustum);
			InstallHook(WriteRegionViewThumbnail_InjectPoint, Hook_WriteRegionViewThumbnail);
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