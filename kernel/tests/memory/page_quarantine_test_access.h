#ifndef STELLUX_TESTS_MEMORY_PAGE_QUARANTINE_TEST_ACCESS_H
#define STELLUX_TESTS_MEMORY_PAGE_QUARANTINE_TEST_ACCESS_H

namespace page_quarantine {

// Holds the drain claim as a drain in progress does, through functions page_quarantine.cpp defines in unit test builds
struct drain_claim_test_access {
    // Claims the drain, waiting as `drain` does while another drain is in progress
    static void hold();

    // Releases the claim and wakes the drains waiting for it
    static void release();
};

} // namespace page_quarantine

#endif // STELLUX_TESTS_MEMORY_PAGE_QUARANTINE_TEST_ACCESS_H
