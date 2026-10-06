// R-comp shelf: one shelf session, the same on the console and in the PC preview: the catalog, the covers, the
// shelf (fe::App), and what its buttons ask for: installs (with progress and cancel), a fresh look for titles and
// packages, and the title to play. The platform owns the display, the controller and the frame loop.
//
// Copyright (C) 2026 R-comp contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "fe_app.h"
#include "fe_covers.h"
#include "install.h"
#include "titles.h"

#include <functional>
#include <memory>
#include <string>

namespace rshelf
{
struct SessionConfig
{
	CatalogPaths catalog;
	std::string covers_dir;      // the user's covers
	std::string cover_cache_dir; // downloaded covers
	std::string trash_dir;       // where an update moves the copy it replaces
	bool allow_download = false;
	fe::DownloadFn download;     // may be empty (no downloads)
	// Fails the download in flight (the shelf is closing or opening again), and lets downloads run again after.
	std::function<void()> abort_download, resume_download;
	std::string build_tag;
	fe::SoundSink* sound = nullptr;
	Log log;
};

class Session
{
public:
	~Session();

	// Scans the catalog and opens the shelf on `preselect` (a PS5 title id; "" for the first game), showing
	// `message` for a few seconds when it isn't empty. False when the shelf could not start.
	bool Start(fe::Renderer* renderer, const fe::Fonts* fonts, const SessionConfig& cfg, const std::string& preselect,
		const std::string& message);

	void Update(double dt, const fe::Input& in);
	void Build(fe::FrameDesc& f, const std::string& clock) { m_app->Build(f, clock); }

	// A title was picked to play (its launch animation has played): the platform closes the shelf and starts it.
	bool PlayChosen(fe::GameInfo& game) const;

	// Ends the covers' worker (waits up to `timeout_ms`; false: a download still runs and the session must stay
	// alive), cancels an install and waits for it, and frees the shelf's textures.
	bool Stop(int timeout_ms);

	const std::vector<fe::GameInfo>& games() const { return m_app->games(); }
	bool Installing() const { return m_installing >= 0; }

private:
	bool Open(const std::string& preselect, const std::string& message);
	void Close();
	void PollInstall();

	fe::Renderer* m_renderer = nullptr;
	const fe::Fonts* m_fonts = nullptr;
	SessionConfig m_cfg;
	std::unique_ptr<fe::App> m_app;
	std::unique_ptr<fe::CoverService> m_covers;
	Installer m_installer;
	int m_installing = -1;
	std::string m_installing_id;
	bool m_play = false;
	fe::GameInfo m_play_game;
};

// printf-style text with up to two strings (the shelf's translated formats).
std::string Format(const char* fmt, const std::string& a, const std::string& b = std::string());
} // namespace rshelf
