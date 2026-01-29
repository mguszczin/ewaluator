#ifndef DEFS_H
#define DEFS_H

#include <string>
#include <vector>

// Buffer sizes
#define STATE_SIZE 100
#define ACTION_SIZE 10
#define NAME_SIZE 10

namespace event_data {
    enum class EventType : int {
        NEW_TEST          = 0, 
        STDIN_CLOSED      = 1, 
        EVT_DATA_READY    = 2, 
        EVT_TRANSFER_DONE = 3, 
        EVT_TEST_FINISHED = 4, 
        EVT_ERROR         = 5 
    };

    struct Event {
        EventType type;
        size_t test_id;
        char data_byte; 
    };

    enum class TestState {
        CREATED,
        WAITING_FOR_ENV_OUTPUT, 
        QUEUED_FOR_GPU,         
        WAITING_FOR_POLICY_OUTPUT, 
        QUEUED_FOR_CPU,         
        SAVING,                 
        FINISHED
    };

    struct Test {
        size_t test_id;
        std::string name;
        pid_t env_pid = -1;
        pid_t policy_pid = -1;
        int env_in = -1;  
        int env_out = -1; 
        int pol_in = -1;  
        int pol_out = -1; 
        TestState state = TestState::CREATED;
        char latch_byte = 0; 
    };
}
#endif