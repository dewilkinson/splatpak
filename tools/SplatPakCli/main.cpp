// SplatPakCli -- SplatPak's command line (docs/requirements/cli.md)
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// One console program for every SplatPak entry point, scripted and unattended:
//
//   SplatPakCli info <package.gstx> [--json]          header, levels, sections and view settings     (headless)
//   SplatPakCli verify <package.gstx>                 reads and decodes every level and section      (headless)
//   SplatPakCli set-background <package.gstx> solid <r,g,b> | gradient <r,g,b> <r,g,b> | none        (headless)
//   SplatPakCli bake <scan> -o <package.gstx> [--sh-degree N] [--sh-bits N] [--no-precompress] [--remember]
//   SplatPakCli view <package.gstx | scan>            opens SplatPak on it
//   SplatPakCli capture <scan | package> [SplatPak capture options]
//   SplatPakCli test lod-quality|lod-calibration <folder> | stream | ring [<scan | package>] [options]
//   SplatPakCli app [--] <any SplatPak.exe arguments> every app flag, as SplatPak.exe --help lists them
//
// Headless commands create no window and no D3D12 device. The others start SplatPak.exe (beside this program) with the
// matching flags and wait for it; until the bake moves into a library (docs/requirements/cli.md, phase 2) that includes
// bake, which needs SplatPak's renderer. Exit codes: 0 success, 1 failure, 2 usage error. Results on stdout.
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/SplatCore/StreamPackager.h"
#include "../../libs/gstx/PackageView.h"

using namespace Splat;

namespace
{
	const char* kUsage =
		"SplatPakCli <command> [options]\n"
		"\n"
		"Headless (no window, no GPU):\n"
		"  info <package.gstx> [--json]        header, levels, sections and view settings\n"
		"  verify <package.gstx>               read and decode every level and section; exit 1 on a bad package\n"
		"  set-background <package.gstx> solid <r,g,b> | gradient <top r,g,b> <bottom r,g,b> | none   (0..1 each)\n"
		"\n"
		"Through SplatPak.exe (its renderer starts):\n"
		"  bake <scan.ply|.splat|.sog> -o <package.gstx> [--sh-degree 1-3] [--sh-bits 8|6|5|4] [--no-precompress] [--remember]\n"
		"  view <package.gstx | scan>\n"
		"  capture <scan | package> [SplatPak capture options: --camera-capture <folder>, --capture-views <folder>, ...]\n"
		"  test lod-quality <folder> | lod-calibration <folder> | stream | ring   [<scan | package>] [options]\n"
		"  app [--] <arguments>                any SplatPak.exe arguments (SplatPak.exe --help lists them)\n"
		"\n"
		"Exit codes: 0 success, 1 failure, 2 usage error.\n";

	int Usage(const std::string& problem)
	{
		if (!problem.empty()) fprintf(stderr, "SplatPakCli: %s\n\n", problem.c_str());
		fputs(kUsage, problem.empty() ? stdout : stderr);
		return problem.empty() ? 0 : 2;
	}

	std::string JsonEscape(const std::string& s)
	{
		std::string o;
		for (char c : s)
		{
			if (c == '"' || c == '\\') { o += '\\'; o += c; }
			else if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
			else o += c;
		}
		return o;
	}

	uint64_t FileBytes(const std::string& path)
	{
		WIN32_FILE_ATTRIBUTE_DATA a = {};
		if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a)) return 0;
		return ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
	}

	// ---- info / verify ---------------------------------------------------------------------------------------------

	struct LevelStats { uint64_t splats = 0, recordBytes = 0, shBytes = 0; };

	std::vector<LevelStats> Levels(const StreamPackager::SFLWPackageData& pkg)
	{
		std::vector<LevelStats> levels;
		for (size_t c = 0; c < pkg.chunkLODSplats.size(); c++)
		{
			for (size_t l = 0; l < pkg.chunkLODSplats[c].size(); l++)
			{
				if (levels.size() <= l) levels.resize(l + 1);
				levels[l].splats += pkg.chunkLODSplats[c][l].size();
				levels[l].recordBytes += pkg.chunkLODSplats[c][l].size() * sizeof(PackedSplatGPU);
				if (c < pkg.chunkLODSH.size() && l < pkg.chunkLODSH[c].size()) levels[l].shBytes += pkg.chunkLODSH[c][l].size();
			}
		}
		return levels;
	}

	uint64_t Triangles(const ShadowMesh& mesh)
	{
		return mesh.levels.empty() ? 0 : mesh.levels[0].indices.size() / 3;
	}

	int Info(const std::vector<std::string>& args)
	{
		std::string path;
		bool json = false;
		for (const auto& a : args)
		{
			if (a == "--json") json = true;
			else if (!a.empty() && a[0] == '-') return Usage("info: unknown option " + a);
			else if (path.empty()) path = a;
			else return Usage("info takes one package");
		}
		if (path.empty()) return Usage("info needs a package");

		StreamPackager::SFLWPackageData pkg;
		std::string error;
		if (!StreamPackager::LoadPackage(path, pkg, &error))
		{
			fprintf(stderr, "SplatPakCli info: %s\n", error.c_str());
			return 1;
		}
		PackageView view;
		const bool hasView = view.Read(path);
		const auto& h = pkg.header;
		const auto levels = Levels(pkg);
		const uint64_t bytes = FileBytes(path);
		const double ratio = (bytes > 0 && h.sourceFileBytes > 0) ? (double)h.sourceFileBytes / (double)bytes : 0.0;

		if (json)
		{
			std::ostringstream o;
			o << "{\n  \"file\": \"" << JsonEscape(path) << "\",\n  \"bytes\": " << bytes << ",\n  \"version\": " << h.version
			  << ",\n  \"chunks\": " << h.numChunks << ",\n  \"sh_degree\": " << h.shDegree << ",\n  \"source_bytes\": " << h.sourceFileBytes
			  << ",\n  \"ratio\": " << ratio << ",\n  \"bounds_min\": [" << h.globalBoundsMin.x << ", " << h.globalBoundsMin.y << ", " << h.globalBoundsMin.z
			  << "],\n  \"bounds_max\": [" << h.globalBoundsMax.x << ", " << h.globalBoundsMax.y << ", " << h.globalBoundsMax.z << "],\n  \"levels\": [";
			for (size_t l = 0; l < levels.size(); l++)
				o << (l ? ", " : "") << "{\"splats\": " << levels[l].splats << ", \"record_bytes\": " << levels[l].recordBytes << ", \"sh_bytes\": " << levels[l].shBytes << "}";
			o << "],\n  \"shadow_mesh_triangles\": " << Triangles(pkg.shadowMesh) << ",\n  \"occluder_mesh_triangles\": " << Triangles(pkg.occluderMesh)
			  << ",\n  \"occlusion_voxels\": " << h.occlusionVoxelCount << ",\n  \"detail_grid\": " << (pkg.detailGrid.nx > 0 ? "true" : "false");
			if (hasView)
			{
				o << ",\n  \"view\": {\"scene\": \"" << JsonEscape(view.scene) << "\", \"navigation\": \"" << JsonEscape(view.navigation)
				  << "\", \"background\": \"" << JsonEscape(view.background.mode) << "\", \"training_cameras\": " << view.cameras.size() << "}";
			}
			o << "\n}\n";
			fputs(o.str().c_str(), stdout);
			return 0;
		}

		printf("%s\n", path.c_str());
		printf("  format version %u, %u chunks, SH degree %u, %.1f MB", h.version, h.numChunks, h.shDegree, bytes / (1024.0 * 1024.0));
		if (ratio > 0.0) printf(" (%.2fx smaller than the %.1f MB scan)", ratio, h.sourceFileBytes / (1024.0 * 1024.0));
		printf("\n  bounds (%.3f, %.3f, %.3f) .. (%.3f, %.3f, %.3f)\n", h.globalBoundsMin.x, h.globalBoundsMin.y, h.globalBoundsMin.z,
			h.globalBoundsMax.x, h.globalBoundsMax.y, h.globalBoundsMax.z);
		for (size_t l = 0; l < levels.size(); l++)
			printf("  level %zu: %llu splats (records %.1f MB, SH %.1f MB decoded)\n", l, (unsigned long long)levels[l].splats,
				levels[l].recordBytes / (1024.0 * 1024.0), levels[l].shBytes / (1024.0 * 1024.0));
		printf("  shadow mesh: %s\n", pkg.shadowMesh.levels.empty() ? "none" : (std::to_string(pkg.shadowMesh.levels.size()) + " LODs, " +
			std::to_string(Triangles(pkg.shadowMesh)) + " triangles at LOD 0").c_str());
		printf("  occlusion: %s\n", !pkg.occluderMesh.levels.empty() ? ("occluder mesh, " + std::to_string(Triangles(pkg.occluderMesh)) + " triangles").c_str()
			: h.occlusionVoxelCount ? (std::to_string(h.occlusionVoxelCount) + " voxels").c_str() : "none");
		printf("  detail grid: %s\n", pkg.detailGrid.nx > 0 ? "stored" : "none");
		if (hasView)
			printf("  view: scene %s, navigation %s, background %s, %zu training cameras\n", view.scene.empty() ? "-" : view.scene.c_str(),
				view.navigation.empty() ? "-" : view.navigation.c_str(), view.background.mode.empty() ? "-" : view.background.mode.c_str(), view.cameras.size());
		else
			printf("  view: none stored\n");
		return 0;
	}

	int Verify(const std::vector<std::string>& args)
	{
		if (args.size() != 1 || args[0].empty() || args[0][0] == '-') return Usage("verify needs one package");
		StreamPackager::SFLWPackageData pkg;
		std::string error;
		if (!StreamPackager::LoadPackage(args[0], pkg, &error))
		{
			printf("FAIL %s: %s\n", args[0].c_str(), error.c_str());
			return 1;
		}
		// Every payload decoded on load; the counts must agree with the header
		const auto levels = Levels(pkg);
		const uint64_t level0 = levels.empty() ? 0 : levels[0].splats;
		if (pkg.chunkManifests.size() != pkg.header.numChunks || level0 != pkg.header.totalSplatsLOD0)
		{
			printf("FAIL %s: %zu chunks read for %u in the header, %llu level 0 splats for %llu\n", args[0].c_str(), pkg.chunkManifests.size(),
				pkg.header.numChunks, (unsigned long long)level0, (unsigned long long)pkg.header.totalSplatsLOD0);
			return 1;
		}
		uint64_t all = 0;
		for (const auto& l : levels) all += l.splats;
		printf("OK %s: %u chunks, %zu levels, %llu splats at level 0, %llu in all levels\n", args[0].c_str(), pkg.header.numChunks, levels.size(),
			(unsigned long long)level0, (unsigned long long)all);
		return 0;
	}

	// ---- set-background (as SplatPak.exe --set-background) --------------------------------------------------------

	int SetBackground(const std::vector<std::string>& args)
	{
		auto colour = [](const std::string& s, DirectX::XMFLOAT3& c)
		{
			return sscanf_s(s.c_str(), "%f,%f,%f", &c.x, &c.y, &c.z) == 3 && c.x >= 0.0f && c.x <= 1.0f && c.y >= 0.0f && c.y <= 1.0f && c.z >= 0.0f && c.z <= 1.0f;
		};
		const std::string pkg = args.size() > 0 ? args[0] : std::string(), how = args.size() > 1 ? args[1] : std::string();
		PackageBackground bg;
		bg.basis = "chosen";
		bool ok = !pkg.empty();
		if (how == "solid") { ok = ok && args.size() == 3 && colour(args[2], bg.top); bg.bottom = bg.top; bg.mode = how; }
		else if (how == "gradient") { ok = ok && args.size() == 4 && colour(args[2], bg.top) && colour(args[3], bg.bottom); bg.mode = how; }
		else ok = ok && how == "none" && args.size() == 2;
		if (!ok) return Usage("set-background needs <package.gstx> solid <r,g,b> | gradient <r,g,b> <r,g,b> | none (0..1 each)");
		PackageView view;
		view.Read(pkg); // Kept: its other settings, if it has a view section
		view.background = bg;
		if (!view.Write(pkg))
		{
			fprintf(stderr, "SplatPakCli set-background: could not write %s\n", pkg.c_str());
			return 1;
		}
		printf("%s: background %s\n", pkg.c_str(), how.c_str());
		return 0;
	}

	// ---- Through SplatPak.exe ---------------------------------------------------------------------------------------

	std::string Quote(const std::string& a)
	{
		if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
		std::string o = "\"";
		for (char c : a) { if (c == '"') o += '\\'; o += c; }
		return o + "\"";
	}

	// SplatPak.exe runs in its own folder (it finds its shaders there), so a path given relative to the caller's folder is
	// made absolute first. Options and numbers pass through as they are.
	std::string AbsolutePath(const std::string& a)
	{
		if (a.empty() || a[0] == '-') return a;
		char* end = nullptr;
		strtod(a.c_str(), &end);
		if (end && *end == '\0') return a; // A number
		if (a.find_first_of("\\/.") == std::string::npos) return a; // A word such as a test or policy name
		char full[MAX_PATH] = {};
		return GetFullPathNameA(a.c_str(), MAX_PATH, full, nullptr) ? std::string(full) : a;
	}

	// Runs SplatPak.exe (beside this program) with the arguments and returns its exit code
	int RunApp(const std::vector<std::string>& appArgs)
	{
		char self[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, self, MAX_PATH);
		std::string dir = self;
		dir.resize(dir.find_last_of("\\/") + 1);
		const std::string exe = dir + "SplatPak.exe";
		if (GetFileAttributesA(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			fprintf(stderr, "SplatPakCli: %s not found (this command needs SplatPak.exe beside SplatPakCli.exe)\n", exe.c_str());
			return 1;
		}
		std::string cmd = Quote(exe);
		for (const auto& a : appArgs) cmd += " " + Quote(AbsolutePath(a));
		fprintf(stderr, "SplatPakCli: running %s\n", cmd.c_str());
		STARTUPINFOA si = { sizeof(si) };
		PROCESS_INFORMATION pi = {};
		std::vector<char> buf(cmd.begin(), cmd.end());
		buf.push_back('\0');
		if (!CreateProcessA(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi))
		{
			fprintf(stderr, "SplatPakCli: could not start SplatPak.exe (error %lu)\n", GetLastError());
			return 1;
		}
		WaitForSingleObject(pi.hProcess, INFINITE);
		DWORD code = 1;
		GetExitCodeProcess(pi.hProcess, &code);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		return (int)code;
	}

	int Bake(const std::vector<std::string>& args)
	{
		std::string scan, out;
		std::vector<std::string> app;
		for (size_t i = 0; i < args.size(); i++)
		{
			const std::string& a = args[i];
			if ((a == "-o" || a == "--out") && i + 1 < args.size()) out = args[++i];
			else if ((a == "--sh-degree" || a == "--sh-bits") && i + 1 < args.size()) { app.push_back(a); app.push_back(args[++i]); }
			else if (a == "--no-precompress") app.push_back(a);
			else if (a == "--remember") app.push_back("--remember-settings");
			else if (!a.empty() && a[0] == '-') return Usage("bake: unknown option " + a);
			else if (scan.empty()) scan = a;
			else return Usage("bake takes one scan");
		}
		if (scan.empty() || out.empty()) return Usage("bake needs <scan> -o <package.gstx>");
		std::vector<std::string> full = { scan, "--bake-to", out };
		full.insert(full.end(), app.begin(), app.end());
		const int code = RunApp(full);
		// SplatPak quits 0 even when there was nothing to bake: the package on disk is the result
		if (code == 0 && FileBytes(out) == 0 && FileBytes(out + ".gstx") == 0)
		{
			fprintf(stderr, "SplatPakCli bake: no package written (see bin\\lod_transition_trace.log)\n");
			return 1;
		}
		return code;
	}

	int Test(const std::vector<std::string>& args)
	{
		if (args.empty()) return Usage("test needs lod-quality, lod-calibration, stream or ring");
		const std::string& which = args[0];
		std::vector<std::string> rest(args.begin() + 1, args.end());
		std::vector<std::string> app;
		if (which == "lod-quality" || which == "lod-calibration")
		{
			if (rest.empty()) return Usage("test " + which + " needs a report folder");
			app = { which == "lod-quality" ? "--lod-quality-test" : "--lod-calibration", rest[0] };
			rest.erase(rest.begin());
		}
		else if (which == "stream") app = { "--stream-test" };
		else if (which == "ring") app = { "--ring-test" };
		else return Usage("unknown test " + which);
		app.insert(app.end(), rest.begin(), rest.end());
		return RunApp(app);
	}
}

int main(int argc, char** argv)
{
	std::vector<std::string> args(argv + 1, argv + argc);
	if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h" || args[0] == "/?") return Usage("");
	const std::string command = args[0];
	std::vector<std::string> rest(args.begin() + 1, args.end());

	if (command == "info") return Info(rest);
	if (command == "verify") return Verify(rest);
	if (command == "set-background") return SetBackground(rest);
	if (command == "bake") return Bake(rest);
	if (command == "test") return Test(rest);
	if (command == "view" || command == "capture") return RunApp(rest); // The scan or package first, then SplatPak's own options
	if (command == "app")
	{
		if (!rest.empty() && rest[0] == "--") rest.erase(rest.begin());
		return RunApp(rest);
	}
	return Usage("unknown command " + command);
}
