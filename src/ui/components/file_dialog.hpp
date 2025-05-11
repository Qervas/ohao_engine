#pragma once
#include <string>
#include <vector>
#include <functional>

namespace ohao{

class FileDialog{
public:
    static std::string openFile(
        const char* title,
        const char* defaultPath,
        const std::vector<const char*>& filterPatterns,
        const char* filterDescription
    );

    static std::string saveFile(
        const char* title,
        const char* defaultPath,
        const std::vector<const char*>& filterPatterns,
        const char* filterDescription
    );

    static std::string selectDirectory(
        const char* title,
        const char* defaultPath
    );

private:
    static const char* const OBJ_FILTER_PATTERNS[];
    static const char* const OBJ_FILTER_DESCRIPTION;
};

}
