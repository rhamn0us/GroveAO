#pragma once

#include <QString>

// Android storage handling. Everything here is a harmless no-op on other platforms.
//
// Layout on Android:
//   access granted -> <shared storage>/GroveAO/base   (e.g. /storage/emulated/0/GroveAO/base)
//   access denied  -> <app-specific external dir>/base (Android/data/<pkg>/files/base)

// True when the app may read/write shared storage (always true off Android).
bool android_storage_access_granted();

// Opens the system screen/dialog asking for file access (no-op off Android).
void android_request_storage_access();

// Root folder that contains "base" and "logs". Empty off Android.
QString android_storage_root();

// Call once from main() right after QApplication is constructed, before anything
// touches Options or get_base_path(). Asks for access if needed, picks the root,
// and creates the base folder with its usual sub-folders.
void android_setup_storage();
