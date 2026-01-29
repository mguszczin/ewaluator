#ifndef DEFS_H
#define DEFS_H

#include <vector>
#include <string>
#include <unistd.h>

// --- CONSTANTS ---

// Size of the state data sent from Environment to Policy
#ifndef STATE_SIZE
#define STATE_SIZE 100
#endif

// Size of the action data sent from Policy to Environment
#ifndef ACTION_SIZE
#define ACTION_SIZE 10
#endif

// Size of the test name string
#ifndef NAME_SIZE
#define NAME_SIZE 10
#endif

// --- UTILITIES ---

namespace utils {
    // Helper class to handle command line arguments
    class ParseReader {
    private:
        using arguments_cont_t = std::vector<std::string>;
        arguments_cont_t arguments;
    public:
        ParseReader(int argc, char** args) {
            // Start loop from 1 to skip the executable name (argv[0])
            for (int i = 1; i < argc; i++) {
                arguments.push_back(std::string{args[i]});
            }
        }

        arguments_cont_t get_args() const {
            return arguments;
        }
    };
}

// --- EVENT & DATA STRUCTURES ---

namespace event_data {
    
    // Types of events sent through the event_pipe
    enum class EventType : int {
        NEW_TEST          = 0, // Reader has sent a new test name
        STDIN_CLOSED      = 1, // Reader has finished (EOF)
        EVT_DATA_READY    = 2, // A Waiter detected the first byte of data
        EVT_TRANSFER_DONE = 3, // A Copier finished moving data between pipes
        EVT_TEST_FINISHED = 4, // A Saver finished writing result to file
        EVT_ERROR         = 5  // Something went wrong
    };

    // The message packet sent through the pipe
    struct Event {
        EventType type;
        size_t test_id;
        char data_byte; // The "latch byte" - the first byte read by a Waiter
    };

    // Lifecycle states for a single Test
    enum class TestState {
        CREATED,
        WAITING_FOR_ENV_OUTPUT,    // Env is calculating, Waiter is listening on Env Output
        QUEUED_FOR_GPU,            // Env finished, waiting for Policy slot (GPU)
        WAITING_FOR_POLICY_OUTPUT, // Policy is calculating, Waiter is listening on Policy Output
        QUEUED_FOR_CPU,            // Policy finished, waiting for Env slot (CPU)
        SAVING,                    // Saver is writing final result to disk
        FINISHED
    };

    // The main database record for a Test
    struct Test {
        size_t test_id;
        std::string name;
        
        // Process IDs (Needed for SIGINT cleanup)
        pid_t env_pid = -1;
        pid_t policy_pid = -1;
        pid_t current_worker_pid = -1;

        // Pipe File Descriptors (Parent's end)
        int env_in = -1;  // We write Actions here
        int env_out = -1; // We read States from here
        int pol_in = -1;  // We write States here
        int pol_out = -1; // We read Actions from here

        // Logic State
        TestState state = TestState::CREATED;
        
        // Temporary storage for the byte caught by the Waiter
        char latch_byte = 0; 
    };
}

#endif // DEFS_H