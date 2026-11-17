#include "discovery.hpp"

#include "files.hpp"
#include "recipes.hpp"

#include <algorithm>
#include <iterator>
#include <regex>

constexpr const char* DISCARDED_OUTPUT = "/dev/null";
constexpr const char* DEPFILE_SUFFIX = ".libsdetect.d";
constexpr const char* SKETCH_SOURCE_FOLDER = "src";
constexpr std::string_view FATAL_ERROR = "fatal error";

static std::string trimmed(std::string_view text) {
    size_t first = text.find_first_not_of(" \t\r\n");
    size_t last = text.find_last_not_of(" \t\r\n");
    return first == std::string_view::npos ? std::string() : std::string(text.substr(first, last - first + 1));
}

// The header a `fatal error` line names, as arduino-cli reads compilers that quote no include line
static std::string header_after_fatal_error(std::string_view output) {
    while (!output.empty()) {
        size_t end = output.find('\n');
        std::string_view line = output.substr(0, end);
        size_t at = line.find(FATAL_ERROR);
        size_t colon = at == std::string_view::npos ? std::string_view::npos : line.find(':', at);
        if (colon != std::string_view::npos) {
            return trimmed(line.substr(colon + 1, line.find(':', colon + 1) - colon - 1));
        }

        output.remove_prefix(end == std::string_view::npos ? output.size() : end + 1);
    }

    return {};
}

static std::string missing_header(std::string_view output) {
    static const std::regex control_sequence("\x1b\\[[0-?]*[ -/]*[@-~]");
    static const std::regex include_line(R"(^\s*[0-9 |]*\s*#[ \t]*include\s*[<"](\S+)[">])", std::regex::multiline);

    std::string text = std::regex_replace(std::string(output), control_sequence, "");
    std::smatch match;
    if (std::regex_search(text, match, include_line)) {
        return trimmed(match[1].str());
    }

    return header_after_fatal_error(text);
}

probe_result read_probe_result(int exit_status, std::string_view output) {
    if (exit_status == 0) {
        return { probe_status::complete, "", "" };
    }

    if (output.empty()) {
        return { probe_status::failed, "", "" };
    }

    std::string header = missing_header(output);
    probe_status status = header.empty() ? probe_status::complete : probe_status::missing;
    return { status, header, std::string(output) };
}

library_discovery::library_discovery(const property_table& properties, const std::vector<library>& libraries,
                                     std::string architecture, std::string build_directory,
                                     std::vector<std::string> sources, std::vector<std::string> include_directories)
    : m_properties(properties),
      m_libraries(libraries),
      m_architecture(std::move(architecture)),
      m_build_directory(std::move(build_directory)),
      m_include_directories(std::move(include_directories)) {
    for (std::string& source : sources) {
        m_queue.push_back({ source, source + DEPFILE_SUFFIX, "" });
    }

    start_turn();
}

probe library_discovery::make_probe(const queued_file& file) const {
    std::vector<std::string> includes = m_include_directories;
    if (!file.extra_include.empty()) {
        includes.push_back(file.extra_include);
    }

    probe made = { file.source, file.depfile, {}, "" };
    preprocess_words(m_properties, file.source, DISCARDED_OUTPUT, includes, file.depfile, made.words, made.error);

    return made;
}

void library_discovery::start_turn() {
    m_turn_probe = m_turn < m_queue.size() ? make_probe(m_queue[m_turn]) : probe{};
}

const probe* library_discovery::turn_probe() const {
    return m_turn < m_queue.size() ? &m_turn_probe : nullptr;
}

std::vector<probe> library_discovery::probes_ahead(size_t count) {
    std::vector<probe> ahead;
    for (size_t index = m_turn; index < m_queue.size() && ahead.size() < count; index++) {
        probe candidate = make_probe(m_queue[index]);
        bool offered = std::any_of(m_offered_ahead.begin(), m_offered_ahead.end(), [&](const probe& earlier) {
            return earlier.words == candidate.words;
        });

        if (!offered) {
            m_offered_ahead.push_back(candidate);
            ahead.push_back(std::move(candidate));
        }
    }

    return ahead;
}

bool library_discovery::take_turn_result(const probe_result& result, discovery_failure& failure) {
    m_reached.push_back({ m_turn_probe, result });
    if (result.status == probe_status::complete) {
        m_turn++;
        start_turn();
        return true;
    }

    if (result.status == probe_status::failed) {
        failure = { discovery_stop::failed, "", "", result.output };
        return false;
    }

    std::vector<const library*> candidates = libraries_providing(m_libraries, result.header);
    if (candidates.empty()) {
        failure = { discovery_stop::unresolved, result.header, "", result.output };
        return false;
    }

    for (const library* candidate : candidates) {
        if (std::find(m_added.begin(), m_added.end(), candidate) != m_added.end()) {
            failure = { discovery_stop::already_added, result.header, candidate->name, result.output };
            return false;
        }
    }

    const library* picked = pick_library(candidates, result.header, m_architecture);
    if (!picked->refusal.empty()) {
        failure = { discovery_stop::refused, result.header, picked->name + " " + picked->refusal, result.output };
        return false;
    }

    std::vector<std::string> sources;
    std::string error;
    if (!library_sources(*picked, sources, error)) {
        failure = { discovery_stop::unreadable, result.header, picked->name, error };
        return false;
    }

    std::string source_directory = library_source_directory(*picked);
    std::string build_folder = library_build_directory(m_build_directory, *picked);
    for (const std::string& source : sources) {
        std::string relative = source.substr(source_directory.size() + 1);
        queued_file file = { source, join_path(build_folder, relative + DEPFILE_SUFFIX), picked->utility_directory };
        auto waiting = m_queue.begin() + static_cast<ptrdiff_t>(m_turn + 1);
        bool queued = std::any_of(waiting, m_queue.end(), [&](const queued_file& item) {
            return item.source == file.source && item.depfile == file.depfile &&
                   item.extra_include == file.extra_include;
        });

        if (!queued) {
            m_queue.push_back(std::move(file));
        }
    }

    m_added.push_back(picked);
    m_include_directories.push_back(source_directory);
    start_turn();
    return true;
}

const std::vector<const library*>& library_discovery::libraries_added() const {
    return m_added;
}

const std::vector<std::string>& library_discovery::include_directories() const {
    return m_include_directories;
}

const std::vector<reached_probe>& library_discovery::reached() const {
    return m_reached;
}

size_t library_discovery::files_done() const {
    return m_turn;
}

size_t library_discovery::files_queued() const {
    return m_queue.size();
}

bool discovery_sources(const std::string& sketch_tree, const std::string& main_file, std::vector<std::string>& sources,
                       std::string& error) {
    std::string main_source = join_path(sketch_tree, main_file + ".cpp");
    std::string source_tree = join_path(sketch_tree, SKETCH_SOURCE_FOLDER);
    std::vector<std::string> root;
    std::vector<std::string> nested;
    if (!find_source_files(sketch_tree, false, root, error) ||
        (is_directory(source_tree) && !find_source_files(source_tree, true, nested, error))) {
        return false;
    }

    sources = { main_source + ".merged" };
    std::copy_if(root.begin(), root.end(), std::back_inserter(sources), [&](const std::string& source) {
        return source != main_source;
    });
    sources.insert(sources.end(), nested.begin(), nested.end());

    return true;
}
