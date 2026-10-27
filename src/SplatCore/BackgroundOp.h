// BackgroundOp.h
// SplatPak -- Copyright (c) 2026 Dave Wilkinson / Blueshell LLC
// SPDX-License-Identifier: Apache-2.0
//
// Long operations (bake, open, save) without freezing the window: the heavy work runs on one worker thread
// and reports its stage and progress; a completion callback then runs on the UI thread, where the result is
// swapped into the app and the renderer. One operation at a time. The worker only reads app state (the modal
// progress dialog keeps the UI from changing it meanwhile) and writes its own result object.
//
// Cancel returns control at once: the completion runs with cancelled = true, and the worker's late result is
// dropped when it arrives (a library call that cannot be interrupted simply finishes in the background).

#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace Splat
{
    // Written by the worker, read by the UI thread.
    class Progress
    {
    public:
        void Stage(const std::string& name, float fraction = 0.0f)
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                CloseStage();
                m_stage = name;
            }
            m_fraction = fraction;
        }
        void Fraction(float fraction) { m_fraction = fraction; }
        bool Cancelled() const { return m_cancel.load(); }
        void Cancel() { m_cancel = true; }
        std::string StageName() const { std::lock_guard<std::mutex> lock(m_mutex); return m_stage; }
        float GetFraction() const { return m_fraction.load(); }
        // Two lines of figures under the stage name, kept across stages: line 0 what is being worked on, line 1 on how
        // many threads. DetailText joins those that are set.
        void Detail(int line, const std::string& text) { std::lock_guard<std::mutex> lock(m_mutex); m_detail[line & 1] = text; }
        std::string DetailText() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_detail[0].empty() ? m_detail[1] : m_detail[1].empty() ? m_detail[0] : m_detail[0] + "\n" + m_detail[1];
        }
        // How many threads the current stage runs on (0 = not stated): the dialog's segmented core bar
        void Threads(unsigned n) { m_threads = n; }
        unsigned ThreadsUsed() const { return m_threads.load(); }
        // "stage 1.23 s, ..." for every stage so far: what the stage fractions are weighted from
        std::string Timings() { std::lock_guard<std::mutex> lock(m_mutex); CloseStage(); m_stage.clear(); return m_timings; }

    private:
        void CloseStage()
        {
            const auto now = std::chrono::steady_clock::now();
            if (!m_stage.empty())
            {
                char t[32];
                snprintf(t, sizeof(t), " %.2f s", std::chrono::duration<double>(now - m_stageStart).count());
                m_timings += (m_timings.empty() ? "" : ", ") + m_stage + t;
            }
            m_stageStart = now;
        }
        mutable std::mutex m_mutex;
        std::string        m_stage, m_timings, m_detail[2];
        std::chrono::steady_clock::time_point m_stageStart = std::chrono::steady_clock::now();
        std::atomic<float> m_fraction{ 0.0f };
        std::atomic<bool>  m_cancel{ false };
        std::atomic<unsigned> m_threads{ 0 };
    };

    class BackgroundOp
    {
    public:
        using Work = std::function<bool(Progress&)>;           // Worker thread; false = failed or cancelled
        using Done = std::function<void(bool ok, bool cancelled)>; // UI thread

        static constexpr double kDialogDelaySeconds = 1.0;     // Quicker operations never show the dialog

        ~BackgroundOp() { Abandon(); }

        bool Busy() const { return m_job != nullptr && !m_job->doneCalled; }

        // Starts work on the worker thread (waiting first for an abandoned worker to finish). A quiet operation shows no
        // dialog and leaves the UI free, so its work must not read app state the UI can change (the lazy RDO analysis
        // runs in worker processes from the scan on disk).
        void Start(const std::string& title, Work work, Done done, bool quiet = false)
        {
            Reap(true);
            m_job = std::make_shared<Job>();
            m_job->title = title;
            m_job->quiet = quiet;
            m_job->done = std::move(done);
            m_job->start = std::chrono::steady_clock::now();
            std::shared_ptr<Job> job = m_job;
            job->thread = std::thread([job, work = std::move(work)]()
            {
                bool ok = false;
                try { ok = work(job->progress); }
                catch (...) { ok = false; }
                job->ok = ok;
                job->finished = true;
            });
        }

        // The same without a thread or dialog, for scripted runs and startup loads.
        static bool RunNow(Work work, Done done)
        {
            Progress progress;
            bool ok = false;
            try { ok = work(progress); }
            catch (...) { ok = false; }
            done(ok, false);
            return ok;
        }

        // UI thread, once per frame: runs the completion when the worker has finished.
        void Tick()
        {
            if (m_job && !m_job->doneCalled && m_job->finished)
            {
                m_job->thread.join();
                m_job->doneCalled = true;
                Done done = std::move(m_job->done);
                const bool ok = m_job->ok && !m_job->progress.Cancelled();
                m_job.reset();
                done(ok, false);
            }
            Reap(false);
        }

        // The dialog's Cancel: the completion runs now as cancelled; the worker's result is dropped later.
        void RequestCancel()
        {
            if (!m_job || m_job->doneCalled) return;
            m_job->progress.Cancel();
            m_job->doneCalled = true;
            Done done = std::move(m_job->done);
            m_abandoned = m_job;
            m_job.reset();
            done(false, true);
        }

        // Shutdown: cancel and wait for every worker.
        void Abandon()
        {
            if (m_job) { m_job->progress.Cancel(); m_abandoned = m_job; m_job.reset(); }
            Reap(true);
        }

        bool Quiet() const { return Busy() && m_job->quiet; }

        bool DialogVisible() const
        {
            return Busy() && !m_job->quiet && std::chrono::duration<double>(std::chrono::steady_clock::now() - m_job->start).count() >= kDialogDelaySeconds;
        }
        std::string Title() const { return m_job ? m_job->title : std::string(); }
        std::string StageName() const { return m_job ? m_job->progress.StageName() : std::string(); }
        std::string Detail() const { return m_job ? m_job->progress.DetailText() : std::string(); }
        unsigned Threads() const { return m_job ? m_job->progress.ThreadsUsed() : 0u; }
        float Fraction() const { return m_job ? m_job->progress.GetFraction() : 0.0f; }

    private:
        struct Job
        {
            std::string title;
            Done done;
            Progress progress;
            std::thread thread;
            std::chrono::steady_clock::time_point start;
            std::atomic<bool> finished{ false };
            bool ok = false;
            bool doneCalled = false;
            bool quiet = false;
        };

        // Joins an abandoned (cancelled) worker once it has finished, or waits for it.
        void Reap(bool wait)
        {
            if (!m_abandoned) return;
            if (wait || m_abandoned->finished)
            {
                if (m_abandoned->thread.joinable()) m_abandoned->thread.join();
                m_abandoned.reset();
            }
        }

        std::shared_ptr<Job> m_job;
        std::shared_ptr<Job> m_abandoned;
    };
}
