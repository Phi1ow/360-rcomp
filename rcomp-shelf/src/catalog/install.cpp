// R-comp shelf: the installer (see install.h).
//
// Copyright (C) 2026 R-comp contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "install.h"

#include "fsutil.h"
#include "titles.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace rshelf
{
namespace
{
constexpr size_t kChunk = 4u << 20;

std::string Errno(const char* what, const std::string& path)
{
	return std::string(what) + " " + path + ": " + std::strerror(errno);
}

bool SameDevice(const std::string& a, const std::string& b)
{
	struct stat sa = {}, sb = {};
	return stat(a.c_str(), &sa) == 0 && stat(b.c_str(), &sb) == 0 && sa.st_dev == sb.st_dev;
}
} // namespace

bool RemoveTree(const std::string& root)
{
	struct stat st = {};
	if (lstat(root.c_str(), &st) != 0)
		return errno == ENOENT;
	if (!S_ISDIR(st.st_mode))
		return unlink(root.c_str()) == 0;
	bool ok = true;
	for (const std::string& n : ListDir(root))
		ok = RemoveTree(root + "/" + n) && ok;
	return rmdir(root.c_str()) == 0 && ok;
}

Installer::~Installer()
{
	Cancel();
	if (m_thread.joinable())
		m_thread.join();
}

std::string Installer::error() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_error;
}

std::string Installer::installed_path() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_installed;
}

std::string Installer::trashed_path() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_trashed;
}

bool Installer::Fail(const std::string& why)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_error.empty())
		m_error = why;
	return false;
}

bool Installer::Start(const InstallJob& job)
{
	if (m_thread.joinable() || m_state.load() == State::Running)
		return Fail("an install is already running");
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_error.clear();
		m_installed.clear();
		m_trashed.clear();
	}
	m_cancel = false;
	m_done = 0;
	m_total = 0;
	m_moved = false;
	m_state = State::Running;
	m_thread = std::thread([this, job]() { m_state = Run(job); });
	return true;
}

void Installer::Finish()
{
	if (m_thread.joinable())
		m_thread.join();
	m_state = State::Idle;
}

bool Installer::CopyFile(const std::string& from, const std::string& to, uint64_t size)
{
	const int in = open(from.c_str(), O_RDONLY);
	if (in < 0)
		return Fail(Errno("open", from));
	const int out = open(to.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
	if (out < 0)
	{
		close(in);
		return Fail(Errno("create", to));
	}
	std::vector<uint8_t> buf(kChunk);
	uint64_t copied = 0;
	bool ok = true;
	while (ok && !m_cancel)
	{
		const ssize_t n = read(in, buf.data(), buf.size());
		if (n < 0)
			ok = Fail(Errno("read", from));
		if (n <= 0)
			break;
		for (ssize_t w = 0; ok && w < n;)
		{
			const ssize_t k = write(out, buf.data() + w, static_cast<size_t>(n - w));
			if (k <= 0)
				ok = Fail(Errno("write", to));
			else
				w += k;
		}
		copied += static_cast<uint64_t>(n);
		m_done += static_cast<uint64_t>(n);
	}
	ok = ok && !m_cancel;
	if (ok && copied != size)
		ok = Fail(from + ": copied " + std::to_string(copied) + " of " + std::to_string(size) + " bytes");
	if (ok && fsync(out) != 0)
		ok = Fail(Errno("fsync", to));
	if (close(out) != 0 && ok)
		ok = Fail(Errno("close", to));
	close(in);
	return ok;
}

bool Installer::CopyTree(const std::string& from, const std::string& to)
{
	if (mkdir(to.c_str(), 0777) != 0 && errno != EEXIST)
		return Fail(Errno("mkdir", to));
	for (const std::string& n : ListDir(from))
	{
		if (m_cancel)
			return false;
		const std::string a = from + "/" + n, b = to + "/" + n;
		uint64_t size = 0;
		if (IsRegular(a, &size))
		{
			if (!CopyFile(a, b, size))
				return false;
		}
		else if (IsDirectory(a))
		{
			if (!CopyTree(a, b))
				return false;
		}
		// Anything else (a symlink, a device) is not part of a title: R-comp's packaging refuses them.
	}
	return true;
}

Installer::State Installer::Run(const InstallJob& job)
{
	TitleFolder pkg;
	std::string why;
	if (!ReadTitleFolder(job.package_path, pkg, &why))
		return Fail(job.package_path + ": " + why), State::Failed;
	if (pkg.ps5_title_id != job.ps5_title_id || !IsPs5TitleId(job.ps5_title_id))
		return Fail(job.package_path + ": its title id is " + pkg.ps5_title_id), State::Failed;
	const std::string target = job.homebrew_dir + "/" + job.ps5_title_id;
	const std::string staging = job.homebrew_dir + "/.rcomp-install-" + job.ps5_title_id;
	const bool replacing = IsDirectory(target);
	if (replacing)
	{
		TitleFolder old;
		if (!ReadTitleFolder(target, old, &why) || old.ps5_title_id != job.ps5_title_id)
			return Fail(target + " is not an R-comp title of " + job.ps5_title_id + " (" + why + "); not touched"), State::Failed;
	}
	else if (access(target.c_str(), F_OK) == 0)
		return Fail(target + " exists and is not a folder; not touched"), State::Failed;
	// A staging folder left by an earlier run that stopped half way holds only that run's copy.
	if (access(staging.c_str(), F_OK) == 0 && !RemoveTree(staging))
		return Fail(Errno("clear", staging)), State::Failed;
	m_total = pkg.bytes;

	if (!job.keep_package && SameDevice(job.package_path, job.homebrew_dir) &&
		std::rename(job.package_path.c_str(), staging.c_str()) == 0)
	{
		m_moved = true;
		m_done = pkg.bytes;
	}
	else if (!CopyTree(job.package_path, staging))
	{
		const bool cancelled = m_cancel.load();
		RemoveTree(staging);
		return cancelled ? State::Cancelled : State::Failed;
	}
	if (m_cancel && !m_moved)
	{
		RemoveTree(staging);
		return State::Cancelled;
	}

	std::string trashed;
	if (replacing)
	{
		MakeDirs(job.trash_dir);
		trashed = job.trash_dir + "/" + job.ps5_title_id + "-" + std::to_string(static_cast<long long>(std::time(nullptr)));
		if (std::rename(target.c_str(), trashed.c_str()) != 0)
		{
			Fail(Errno("move aside", target) + " (to " + trashed + ")");
			if (m_moved)
				std::rename(staging.c_str(), job.package_path.c_str()); // the package back where it was
			else
				RemoveTree(staging);
			return State::Failed;
		}
	}
	if (std::rename(staging.c_str(), target.c_str()) != 0)
	{
		Fail(Errno("rename", staging) + " (to " + target + ")");
		if (!trashed.empty())
			std::rename(trashed.c_str(), target.c_str());
		if (m_moved)
			std::rename(staging.c_str(), job.package_path.c_str());
		else
			RemoveTree(staging);
		return State::Failed;
	}
	std::lock_guard<std::mutex> lock(m_mutex);
	m_installed = target;
	m_trashed = trashed;
	return State::Done;
}
} // namespace rshelf
