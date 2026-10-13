#include "Settings.h"

namespace RC
{
	namespace
	{
		std::filesystem::path GetIniPath()
		{
			HMODULE module = nullptr;
			GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&GetIniPath),
				&module);

			std::wstring buf(MAX_PATH, L'\0');
			const auto   len = GetModuleFileNameW(module, buf.data(), static_cast<DWORD>(buf.size()));
			buf.resize(len);

			std::filesystem::path path{ buf };
			path.replace_extension(L".ini");
			return path;
		}

		std::uint32_t ReadUInt(const std::filesystem::path& a_path, const wchar_t* a_section, const wchar_t* a_key, std::uint32_t a_default)
		{
			return GetPrivateProfileIntW(a_section, a_key, static_cast<INT>(a_default), a_path.c_str());
		}

		bool ReadBool(const std::filesystem::path& a_path, const wchar_t* a_section, const wchar_t* a_key, bool a_default)
		{
			return ReadUInt(a_path, a_section, a_key, a_default ? 1 : 0) != 0;
		}

		float ReadFloat(const std::filesystem::path& a_path, const wchar_t* a_section, const wchar_t* a_key, float a_default)
		{
			wchar_t buf[64]{};
			GetPrivateProfileStringW(a_section, a_key, L"", buf, static_cast<DWORD>(std::size(buf)), a_path.c_str());
			wchar_t*    end = nullptr;
			const float value = std::wcstof(buf, &end);
			return end != buf && std::isfinite(value) ? value : a_default;
		}
	}

	void Settings::Load()
	{
		const auto path = GetIniPath();
		const bool exists = std::filesystem::exists(path);

		enabled = ReadBool(path, L"General", L"bEnabled", enabled);
		disablePrecombines = ReadBool(path, L"General", L"bDisablePrecombines", disablePrecombines);
		apply = ReadBool(path, L"General", L"bApply", apply);
		toggleHotkey = ReadUInt(path, L"General", L"iToggleHotkey", toggleHotkey);
		notify = ReadBool(path, L"General", L"bNotify", notify);

		chunkSize = std::clamp(ReadFloat(path, L"Combine", L"fChunkSize", chunkSize), 128.0F, 8192.0F);
		maxShapeRadius = std::clamp(ReadFloat(path, L"Combine", L"fMaxShapeRadius", maxShapeRadius), 16.0F, 16384.0F);
		minShapesPerChunk = std::clamp(ReadUInt(path, L"Combine", L"iMinShapesPerChunk", minShapesPerChunk), 1u, 1024u);
		settleFrames = std::clamp(ReadUInt(path, L"Combine", L"iSettleFrames", settleFrames), 1u, 600u);
		gatherBudgetMs = std::clamp(ReadFloat(path, L"Combine", L"fGatherBudgetMs", gatherBudgetMs), 0.1F, 50.0F);
		chunkFadeNodes = ReadBool(path, L"Combine", L"bChunkFadeNodes", chunkFadeNodes);
		parkHidden = ReadBool(path, L"Combine", L"bParkHidden", parkHidden);
		chunkGrowth = ReadFloat(path, L"Combine", L"fChunkGrowth", chunkGrowth);
		chunkGrowth = chunkGrowth <= 0.0F ? 0.0F : std::clamp(chunkGrowth, 256.0F, 65536.0F);
		clusterSpread = std::clamp(ReadFloat(path, L"Combine", L"fClusterSpread", clusterSpread), 0.0F, 100.0F);
		groupSize = ReadFloat(path, L"Combine", L"fGroupSize", groupSize);
		groupSize = groupSize < 0.0F ? -1.0F : groupSize == 0.0F ? 0.0F : std::clamp(groupSize, 256.0F, 65536.0F);
		combineDuringLoad = ReadBool(path, L"Combine", L"bCombineDuringLoad", combineDuringLoad);
		loadWaitMs = std::clamp(ReadUInt(path, L"Combine", L"iLoadWaitMs", loadWaitMs), 0u, 60000u);

		logCells = ReadBool(path, L"Debug", L"bLogCells", logCells);
		watchdogRefsPerFrame = std::clamp(ReadUInt(path, L"Debug", L"iWatchdogRefsPerFrame", watchdogRefsPerFrame), 16u, 1u << 20);

		logger::info(
			"settings ({}): enabled {}, disablePrecombines {}, apply {}, toggleHotkey 0x{:X}, notify {}, "
			"chunkSize {}, maxShapeRadius {}, minShapesPerChunk {}, settleFrames {}, gatherBudgetMs {}, chunkFadeNodes {}, parkHidden {}, "
			"chunkGrowth {}, clusterSpread {}, groupSize {}, combineDuringLoad {}, loadWaitMs {}, logCells {}, watchdogRefsPerFrame {}",
			exists ? "RuntimeCombiner.ini" : "defaults, no RuntimeCombiner.ini",
			enabled, disablePrecombines, apply, toggleHotkey, notify,
			chunkSize, maxShapeRadius, minShapesPerChunk, settleFrames, gatherBudgetMs, chunkFadeNodes, parkHidden, chunkGrowth, clusterSpread, groupSize,
			combineDuringLoad, loadWaitMs, logCells, watchdogRefsPerFrame);
	}
}
