// PS2-103 (OPT8-F): storage devices for add-ons (memory card, USB) and the auto-load folders. PS2 only.
#ifndef PS2_ADDONS_H
#define PS2_ADDONS_H

#include "../doomtype.h"

// Loads the IOP drivers a path needs, the first time a path of that device is used: "mc0:"/"mc1:" (mcman + mcserv), "mass:" (usbd, bdm,
// usbmass_bd, bdmfs_fatfs). The drivers are IRX files in <data>/modules/ (not embedded: they would stay in the ELF, i.e. in RAM, for
// every game that never touches a card or a stick). Returns false when the device cannot be used (no such device, a module is missing or
// refused to start); "host:", "cdfs:" and relative paths are always usable. Safe to call with any path, often.
boolean PS2Addons_Prepare(const char *path);

// Names of the add-on files (pk3, wad, soc, lua) in a folder, sorted by name; the callback gets "<dir>/<name>". Returns the number of files.
size_t PS2Addons_Scan(const char *dir, void (*add)(const char *path, void *ctx), void *ctx);

// Auto-load: the folder "autoload" of the data directory and of the home directory, plus every "-autoload <dir>" of the command line;
// "-noautoload" switches it off. Called while the start-up add-on list is built.
void PS2Addons_Autoload(void (*add)(const char *path, void *ctx), void *ctx);

#endif
