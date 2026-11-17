#ifndef MCUFW_DISCOVERY_HPP
#define MCUFW_DISCOVERY_HPP

// Finds the libraries a sketch uses as arduino-cli 1.6.0 does: the board's preprocessor runs on each queued file, and a
// header it cannot find adds the library that provides it before the same file is probed again

#include "libraries.hpp"
#include "properties.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

enum class probe_status { complete, missing, failed };

struct probe_result {
    probe_status status;
    std::string  header;  // the missing header
    std::string  output;  // what a failed run printed, empty when the run succeeded or printed nothing
};

struct probe {
    std::string              source;
    std::string              depfile;  // `<file>.libsdetect.d` in the build tree, which every probe of the file writes
    std::vector<std::string> words;
    std::string              error;    // why its words could not be built, which the build fails with
};

enum class discovery_stop { unresolved, already_added, refused, unreadable, failed };

// Why discovery stopped: no library provides the header, a library it added should have, the library that provides it
// is refused, that library's files cannot be listed, or the preprocessor failed printing nothing
struct discovery_failure {
    discovery_stop stop;
    std::string    header;
    std::string    library;  // the library already added, refused or unreadable, a refused one with its reason
    std::string    output;   // the preprocessor's messages, or why the library's files cannot be listed
};

// A turn's probe and the result that counted
struct reached_probe {
    probe        turn;
    probe_result result;
};

// Decides which probes run and reads their results, while the build runs them. The build must take the turn's result
// from a probe it ran ahead with the same words, or stop every probe ahead and run the turn's probe alone.
class library_discovery {
public:
    // `sources` are the merged sketch and the sketch's other source files in arduino-cli's order, `include_directories`
    // the core's and the variant's. `libraries` must outlive the discovery.
    library_discovery(const property_table& properties, const std::vector<library>& libraries,
                      std::string architecture, std::string build_directory, std::vector<std::string> sources,
                      std::vector<std::string> include_directories);

    // The probe whose turn it is, or nullptr once discovery finished
    const probe* turn_probe() const;

    // Probes of the turn's file and the files queued after it, at most `count`, with the include path discovery holds
    // when called. Each probe's words are offered once, so a file comes again only with another include path.
    std::vector<probe> probes_ahead(size_t count);

    // Takes the turn's result: a complete file passes the turn on, and a missing header adds the library providing it
    // and probes the same file again. Fails, filling `failure`, when discovery stops.
    bool take_turn_result(const probe_result& result, discovery_failure& failure);

    const std::vector<const library*>& libraries_added() const;  // in the order they joined
    const std::vector<std::string>&    include_directories() const;
    const std::vector<reached_probe>&  reached() const;          // every result taken, in turn order

    // Files whose probes completed, and files queued so far, for the build's count
    size_t files_done() const;
    size_t files_queued() const;

private:
    struct queued_file {
        std::string source;
        std::string depfile;
        std::string extra_include;  // a flat library's utility/ folder, for that library's own files
    };

    probe make_probe(const queued_file& file) const;
    void  start_turn();

    property_table              m_properties;
    const std::vector<library>& m_libraries;
    std::string                 m_architecture;
    std::string                 m_build_directory;
    std::vector<queued_file>    m_queue;  // grows as libraries join
    size_t                      m_turn = 0;
    probe                       m_turn_probe;
    std::vector<std::string>    m_include_directories;
    std::vector<const library*> m_added;
    std::vector<reached_probe>  m_reached;
    std::vector<probe>          m_offered_ahead;
};

// A preprocessor run's result as arduino-cli reads it: the include line GCC quotes names the missing header, else a
// `fatal error` line does. A failure that names no header completes, and one that prints nothing fails.
probe_result read_probe_result(int exit_status, std::string_view output);

// What discovery probes, in arduino-cli's order: the merged sketch, the sketch tree's own files but `<main>.ino.cpp`,
// then its src/ folder's
bool discovery_sources(const std::string& sketch_tree, const std::string& main_file, std::vector<std::string>& sources,
                       std::string& error);

#endif // MCUFW_DISCOVERY_HPP
