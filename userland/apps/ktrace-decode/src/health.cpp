#include "health.hpp"
#include "text.hpp"

#include <cinttypes>

static void append_list_item(std::string& list, const std::string& item) {
    if (!list.empty()) {
        list += ", ";
    }

    list += item;
}

static health_check check_file_complete(const trace_file& file) {
    std::string trailing_bytes = count_with_noun(file.trailing_bytes, "byte", "bytes");
    if (!file.has_end_chunk) {
        std::string detail = "the file has no end chunk, so it was cut short";
        if (file.trailing_bytes > 0) {
            detail += ", and it ends with " + trailing_bytes + " of a partial record";
        }

        return {"file_complete", false, detail};
    }

    if (file.trailing_bytes > 0) {
        return {"file_complete", false, "the end chunk is followed by " + trailing_bytes};
    }

    return {"file_complete", true, "the file ends with its end chunk"};
}

static health_check check_no_lost_records(const trace_file& file) {
    std::string losses;
    for (size_t cpu = 0; cpu < file.lost_records_by_cpu.size(); cpu++) {
        if (file.lost_records_by_cpu[cpu] > 0) {
            append_list_item(losses, format_string("CPU %zu lost %s", cpu,
                                                   count_with_noun(file.lost_records_by_cpu[cpu], "record",
                                                                   "records").c_str()));
        }
    }

    if (losses.empty()) {
        return {"no_lost_records", true, "no CPU lost records"};
    }

    return {"no_lost_records", false, losses};
}

static health_check check_known_event_ids(const record_counts& counts) {
    std::string unknown;
    for (uint16_t event_id = 0; event_id < ktrace::EVENT_ID_COUNT; event_id++) {
        if (counts.by_event_id[event_id] > 0 && !find_event_description(event_id)) {
            std::string records = count_with_noun(counts.by_event_id[event_id], "record", "records");
            append_list_item(unknown, format_string("%s with event id %u", records.c_str(), event_id));
        }
    }

    if (counts.with_event_id_out_of_range > 0) {
        std::string records = count_with_noun(counts.with_event_id_out_of_range, "record", "records");
        append_list_item(unknown, format_string("%s with an event id of %u or more", records.c_str(),
                                                ktrace::EVENT_ID_COUNT));
    }

    if (unknown.empty()) {
        return {"known_event_ids", true, "every record has a known event id"};
    }

    return {"known_event_ids", false, unknown};
}

std::vector<health_check> check_health(const trace_file& file, const record_counts& counts) {
    return {
        check_file_complete(file),
        check_no_lost_records(file),
        check_known_event_ids(counts),
    };
}
