#include "health.hpp"
#include "text.hpp"

#include <cinttypes>

constexpr uint16_t KNOWN_EVENT_IDS[] = {
    ktrace::EVENT_SCHED_SWITCH,
    ktrace::EVENT_SCHED_WAKEUP,
    ktrace::EVENT_SYSCALL,
    ktrace::EVENT_PAGE_FAULT,
};

static bool is_known_event_id(uint16_t event_id) {
    for (uint16_t known_event_id : KNOWN_EVENT_IDS) {
        if (event_id == known_event_id) {
            return true;
        }
    }

    return false;
}

static void append_list_item(std::string& list, const std::string& item) {
    if (!list.empty()) {
        list += ", ";
    }

    list += item;
}

static health_check check_file_complete(const trace_file& file) {
    if (!file.has_end_chunk) {
        std::string detail = "the file has no end chunk, so it was cut short";
        if (file.trailing_bytes > 0) {
            detail += format_string(", and its last %zu bytes are not a whole record", file.trailing_bytes);
        }

        return {"file_complete", false, detail};
    }

    if (file.trailing_bytes > 0) {
        return {"file_complete", false, format_string("%zu bytes follow the end chunk", file.trailing_bytes)};
    }

    return {"file_complete", true, "the file ends with its end chunk"};
}

static health_check check_no_lost_records(const trace_file& file) {
    std::string losses;
    for (size_t cpu = 0; cpu < file.lost_records_by_cpu.size(); cpu++) {
        if (file.lost_records_by_cpu[cpu] > 0) {
            append_list_item(losses, format_string("CPU %zu lost %" PRIu64 " records", cpu,
                                                   file.lost_records_by_cpu[cpu]));
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
        if (counts.by_event_id[event_id] > 0 && !is_known_event_id(event_id)) {
            append_list_item(unknown, format_string("%" PRIu64 " records have event id %u",
                                                    counts.by_event_id[event_id], event_id));
        }
    }

    if (counts.with_event_id_out_of_range > 0) {
        append_list_item(unknown, format_string("%" PRIu64 " records have an event id of %u or more",
                                                counts.with_event_id_out_of_range, ktrace::EVENT_ID_COUNT));
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
