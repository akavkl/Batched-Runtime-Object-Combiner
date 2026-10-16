#pragma once

namespace RC
{
	// RuntimeCombiner.ini next to the DLL. Missing keys keep these defaults.
	struct Settings
	{
		// [General]
		bool          enabled{ true };
		bool          disablePrecombines{ true };  // switch the engine's precombines off at startup (bUseCombinedObjects)
		bool          apply{ true };               // false: gather and bake, log, change nothing in the scene
		std::uint32_t toggleHotkey{ 114 };         // virtual-key code of the on/off switch (114 = F3, 0 = unbound)
		bool          notify{ true };              // HUD notice when the switch is used

		// [Combine]
		float         chunkSize{ 1024.0F };      // combined mesh diameter (game units): members within half of it of a seed; stragglers: within twice it
		float         maxShapeRadius{ 2048.0F };  // bigger meshes stay as they are
		std::uint32_t minShapesPerChunk{ 2 };     // a combined mesh replaces at least this many meshes; fewer: solo clones
		std::uint32_t settleFrames{ 45 };         // frames without 3D changes in a cell before it is (re)combined
		float         gatherBudgetMs{ 2.0F };     // main-thread time per frame for collecting cells
		bool          chunkFadeNodes{ true };     // each chunk under its own BSFadeNode, fading like its originals; off: in the container
		bool          chunkMeshLOD{ true };       // chunks keep their members' mesh LOD levels (with chunkFadeNodes); off: full detail
		bool          parkHidden{ true };         // roots hidden whole wait under a hidden node the scene walk never visits
		float         chunkGrowth{ 2048.0F };     // beyond this distance from the player chunks grow in proportion (x8 at most); 0 = off
		float         clusterSpread{ 0.0F };      // a chunk reaches at most this x its members' volume radius (beyond fChunkSize / 4); 0 = off
		                                          // (3 in run 35 more than doubled the solos and the far cascade's draws)
		float         groupSize{ -1.0F };         // chunk group nodes: one per square of this size per parent; 0 = one per parent;
		                                          // below 0 = auto (0 with CBRO, 1024 without)
		bool          combineDuringLoad{ true };  // combine every attached cell before the loading menu closes
		std::uint32_t loadWaitMs{ 5000 };         // the longest the loading menu is held for it; the rest finishes after

		// [Debug]
		bool          logCells{ true };              // one line per combined cell
		std::uint32_t watchdogRefsPerFrame{ 512 };  // combined references re-checked per frame
		bool          probes{ false };              // measurement probes: GPU probe, census, chunk audit, previs line, self-check

		void Load();

		[[nodiscard]] static Settings& Get() noexcept
		{
			static Settings settings;
			return settings;
		}
	};
}
