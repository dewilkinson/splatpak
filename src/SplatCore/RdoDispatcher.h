// RdoDispatcher.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Spreads an RDO analysis over workers (docs/design/distributed-rdo.md): the candidates split into jobs (one per SH degree:
// each degree is encoded once, so a job keeps that work together), each run by SplatPakWorker.exe, the parts merged and the
// curve finished (RdoAnalysis::Merge, Finish). Today the workers are processes on this machine; BLUESHELL and the fleet relay
// are further places a job can go, behind the same RunJobs.
//
// Each worker writes "progress <0..1> <stage>" lines to its stderr, captured to a file the dispatcher reads; the reported
// fraction is the jobs' mean. A cancel terminates the workers.

#pragma once
#define NOMINMAX
#include <windows.h>
#include <chrono>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "RdoJob.h"

namespace Splat
{
    namespace RdoDispatcher
    {
        // The jobs for a scan: every candidate the scan's SH degree allows, one job per degree
        inline std::vector<RdoJob> SplitJobs(const RdoJob& base, uint32_t sourceDegree)
        {
            std::vector<RdoJob> jobs;
            const std::vector<RdoSettings> all = RdoAnalysis::Candidates(std::min(sourceDegree, 3u));
            for (uint32_t d = std::min(sourceDegree, 3u); d >= 1; d--)
            {
                RdoJob j = base;
                j.candidates.clear();
                for (const RdoSettings& s : all) if (s.shDegree == d) j.candidates.push_back(s);
                if (!j.candidates.empty()) jobs.push_back(std::move(j));
            }
            return jobs;
        }

        // The last "progress <f> <stage>" line of a worker's stderr file
        inline bool LastProgress(const std::string& path, float& fraction, std::string& stage)
        {
            std::ifstream in(path);
            std::string line, last;
            while (std::getline(in, line)) if (line.rfind("progress ", 0) == 0) last = line;
            if (last.empty()) return false;
            char buf[128] = {};
            float f = 0.0f;
            if (sscanf(last.c_str(), "progress %f %127[^\n]", &f, buf) < 1) return false;
            fraction = f; stage = buf;
            return true;
        }

        // Runs the jobs on this machine, all at once (each is a whole SH degree; Brotli inside spreads over the cores), in
        // `folder` (job, part and stderr files). progress(fraction, stage) returns false to cancel. Fills `parts` in job
        // order; false with `error` when a worker could not start, failed or was cancelled.
        inline bool RunLocal(const std::vector<RdoJob>& jobs, const std::string& workerExe, const std::string& folder,
                             const std::function<bool(float, const char*)>& progress, std::vector<RdoPart>& parts, std::string& error)
        {
            parts.assign(jobs.size(), RdoPart{});
            CreateDirectoryA(folder.c_str(), nullptr);
            std::vector<PROCESS_INFORMATION> procs(jobs.size());
            std::vector<std::string> errFiles(jobs.size()), partFiles(jobs.size());
            for (size_t i = 0; i < jobs.size(); i++)
            {
                const std::string base = folder + "\\job" + std::to_string(i);
                { std::ofstream o(base + ".json", std::ios::binary); o << jobs[i].Text(); }
                partFiles[i] = base + ".part.json";
                errFiles[i] = base + ".progress.txt";
                DeleteFileA(partFiles[i].c_str());
                SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
                HANDLE err = CreateFileA(errFiles[i].c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                STARTUPINFOA si = {};
                si.cb = sizeof(si);
                si.dwFlags = STARTF_USESTDHANDLES;
                si.hStdError = err;
                si.hStdOutput = err;
                si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
                std::string cmd = "\"" + workerExe + "\" --job \"" + base + ".json\" --out \"" + partFiles[i] + "\"";
                std::vector<char> line(cmd.begin(), cmd.end());
                line.push_back('\0');
                const BOOL ok = CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                                               nullptr, nullptr, &si, &procs[i]);
                CloseHandle(err);
                if (!ok)
                {
                    error = "could not start " + workerExe;
                    for (size_t k = 0; k < i; k++) { TerminateProcess(procs[k].hProcess, 1); CloseHandle(procs[k].hProcess); CloseHandle(procs[k].hThread); }
                    return false;
                }
            }

            // Wait, reporting the jobs' mean progress
            bool cancelled = false;
            for (;;)
            {
                size_t running = 0;
                float sum = 0.0f;
                std::string stage;
                for (size_t i = 0; i < jobs.size(); i++)
                {
                    const bool done = WaitForSingleObject(procs[i].hProcess, 0) == WAIT_OBJECT_0;
                    float f = done ? 1.0f : 0.0f; std::string s;
                    if (!done) { running++; if (LastProgress(errFiles[i], f, s) && stage.empty()) stage = s; }
                    sum += done ? 1.0f : f;
                }
                char label[160];
                snprintf(label, sizeof(label), "%s (%zu of %zu workers running)", stage.empty() ? "Measuring" : stage.c_str(), running, jobs.size());
                if (progress && !progress(sum / (float)jobs.size(), label)) { cancelled = true; break; }
                if (running == 0) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            for (auto& p : procs)
            {
                if (cancelled) TerminateProcess(p.hProcess, 1);
                WaitForSingleObject(p.hProcess, INFINITE);
                CloseHandle(p.hProcess); CloseHandle(p.hThread);
            }
            if (cancelled) { error = "cancelled"; return false; }

            for (size_t i = 0; i < jobs.size(); i++)
            {
                std::ifstream in(partFiles[i]);
                std::string parseError;
                if (!in || !parts[i].Parse(in, parseError)) { error = "worker " + std::to_string(i) + " wrote no part (" + parseError + "): see " + errFiles[i]; return false; }
                if (!parts[i].error.empty()) { error = "worker " + std::to_string(i) + ": " + parts[i].error; return false; }
            }
            return true;
        }

        // The whole analysis through the workers: split, run, merge, finish
        inline bool Analyse(const RdoJob& base, uint32_t sourceDegree, const std::string& workerExe, const std::string& folder,
                            const std::function<bool(float, const char*)>& progress, RdoAnalysisResult& out, std::string& error)
        {
            const auto t0 = std::chrono::high_resolution_clock::now();
            const std::vector<RdoJob> jobs = SplitJobs(base, sourceDegree);
            if (jobs.empty()) { error = "the scan has no spherical harmonics to analyse"; return false; }
            std::vector<RdoPart> parts;
            if (!RunLocal(jobs, workerExe, folder, progress, parts, error)) return false;
            std::vector<RdoAnalysisResult> results;
            for (const RdoPart& p : parts) results.push_back(p.result);
            out = RdoAnalysis::Merge(results);
            RdoAnalysis::Finish(out);
            out.seconds = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t0).count();
            return true;
        }
    }
}
