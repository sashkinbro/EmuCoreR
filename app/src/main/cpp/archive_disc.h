#pragma once
#include <string>
#include "libretro.h"
namespace emucorer::archive_disc {
std::string Mount(int fd);
void Unmount(const std::string& path);
const retro_vfs_interface* Interface();
}
