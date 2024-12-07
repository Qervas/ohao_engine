#include "file_dialog.hpp"
#include <string>
#include <tinyfiledialogs.h>

namespace ohao {
const char* const FileDialog::OBJ_FILTER_PATTERNS[] = {"*.obj"};
const char* const FileDialog::OBJ_FILTER_DESCRIPTION = "Object Files (*.obj)";

std::string FileDialog::openFile(const char* title, const char* defaultPath,
    const std::vector<const char*>& filterPatterns, const char* filterDescription){

    const char* path = tinyfd_openFileDialog(title, defaultPath, static_cast<int>(filterPatterns.size()),
                                            filterPatterns.data(), filterDescription, 0);

    return path ? std::string{path} : std::string{};
}

std::string FileDialog::saveFile(
    const char* title,
    const char* defaultPath,
    const std::vector<const char*>& filterPatterns,
    const char* filterDescription)
{
    const char* path = tinyfd_saveFileDialog(
        title,
        defaultPath,
        static_cast<int>(filterPatterns.size()),
        filterPatterns.data(),
        filterDescription
    );

    return path ? std::string(path) : std::string();
}

}
