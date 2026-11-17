#include "library_hint.hpp"

#include "files.hpp"
#include "indexes.hpp"
#include "libraries.hpp"
#include "versions.hpp"

#include <algorithm>
#include <map>

bool suggest_library(const std::string& library_index, const std::string& header, const std::string& architecture,
                     library_suggestion& suggestion, std::string& error) {
    if (!is_file(library_index)) {
        error = "no library index at " + library_index;
        return false;
    }

    std::map<std::string, library_release> newest;
    auto keep_newest = [&](library_release& release, std::string&) {
        auto kept = newest.find(release.name);
        if (kept == newest.end() || compare_versions(release.version, kept->second.version) > 0) {
            newest[release.name] = std::move(release);
        }

        return true;
    };

    if (!read_library_releases(library_index, keep_newest, error)) {
        return false;
    }

    const library_release* picked = nullptr;
    int picked_priority = 0;
    for (const auto& [name, release] : newest) {
        const std::vector<std::string>& headers = release.provided_headers;
        if (std::find(headers.begin(), headers.end(), header) == headers.end()) {
            continue;
        }

        int priority = library_priority(name, library_folder_name(name), release.architectures,
                                        library_location::installed, header, architecture);
        if (!picked || priority > picked_priority) {
            picked = &release;
            picked_priority = priority;
        }
    }

    if (!picked) {
        error = "no library in Arduino's index provides " + header;
        return false;
    }

    suggestion = { picked->name, picked->version };
    return true;
}
