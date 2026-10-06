// R-comp shelf: installing a package as its own PS5 application, /data/homebrew/<PS5 title id>/.
//
//   1. The package goes to a staging folder beside the target, /data/homebrew/.rcomp-install-<id>: renamed
//      there when it is on the same file system (a package in /data/rcomp/packages: instant, the package
//      folder moves), else copied file by file (a USB drive, or keep_package), with progress and cancel.
//   2. An installed copy is renamed aside into the trash folder (/data/rcomp/trash/<id>-<time>), never
//      deleted by the shelf; then the staging folder is renamed to the target. If that rename fails the
//      old copy is put back.
// Only the staging folder of a failed or cancelled copy is removed (it holds nothing but that copy).
// The installer never writes over a folder of /data/homebrew that isn't an R-comp title of the same id.
//
// Copyright (C) 2026 R-comp contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace rshelf
{
struct InstallJob
{
	std::string package_path;  // the package folder
	std::string ps5_title_id;  // from its sce_sys/param.json
	std::string homebrew_dir;  // /data/homebrew
	std::string trash_dir;     // /data/rcomp/trash
	bool keep_package = false; // copy even on the same file system, so the package stays where it is
};

class Installer
{
public:
	enum class State
	{
		Idle,
		Running,
		Done,
		Failed,
		Cancelled
	};

	~Installer();

	// Starts `job` on a thread of its own; false (and error()) when one is running or the job is refused.
	bool Start(const InstallJob& job);
	void Cancel() { m_cancel = true; }
	// Waits for the thread after Done/Failed/Cancelled (or a Cancel), and returns to Idle.
	void Finish();

	State state() const { return m_state.load(); }
	uint64_t done_bytes() const { return m_done.load(); }
	uint64_t total_bytes() const { return m_total.load(); }
	bool moved() const { return m_moved.load(); } // renamed, not copied
	std::string error() const;
	std::string installed_path() const;
	std::string trashed_path() const; // the previous copy's new place ("" when there was none)

	// The work itself, on the calling thread (the thread runs this; tests call it directly).
	State Run(const InstallJob& job);

private:
	bool Fail(const std::string& why);
	bool CopyTree(const std::string& from, const std::string& to);
	bool CopyFile(const std::string& from, const std::string& to, uint64_t size);

	std::thread m_thread;
	std::atomic<State> m_state{State::Idle};
	std::atomic<bool> m_cancel{false};
	std::atomic<uint64_t> m_done{0}, m_total{0};
	std::atomic<bool> m_moved{false};
	mutable std::mutex m_mutex;
	std::string m_error, m_installed, m_trashed;
};

// Removes `root` and everything under it (symlinks are removed, not followed). Used on the installer's own
// staging folders only.
bool RemoveTree(const std::string& root);
} // namespace rshelf
