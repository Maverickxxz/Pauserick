#pragma once

/* The one place for the version: the code, the exe's version resource and the
 * updater (which compares it with the latest GitHub release) all read it here.
 * (The #ifndef lets a test build pretend to be another version.) */
#ifndef PAUSERICK_VER_MAJOR
#define PAUSERICK_VER_MAJOR 1
#define PAUSERICK_VER_MINOR 0
#define PAUSERICK_VER_PATCH 2
#define PAUSERICK_VER_STR   "1.0.2"
#endif

/* GitHub repository the updater checks: https://github.com/<this>/releases */
#define PAUSERICK_REPO      "Maverickxxz/Pauserick"
