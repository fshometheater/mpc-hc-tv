#ifndef MPC_TV_VERSION_H
#define MPC_TV_VERSION_H

// MPC-HC TV's own version, shown as "MPC-HC 2.8.5 TV 0.1.0" (installer, Windows' app list, Help > About)
// and used by the update check (version-tv.txt). MPC-HC's own version (version.h) stays upstream's:
// the language DLLs must match it, and upstream merges change it.
// Raise PATCH for each release, MINOR for bigger steps; keep MPC_TV_VERSION_STR the same.
// Also read by the installer script (Inno Setup's preprocessor), so plain #defines only.
#define MPC_TV_VERSION_MAJOR 0
#define MPC_TV_VERSION_MINOR 1
#define MPC_TV_VERSION_PATCH 0
#define MPC_TV_VERSION_STR   _T("0.1.0")

#endif
