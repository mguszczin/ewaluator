#ifndef DEFS_H
#define DEFS_H

#include <vector>
#include <string>
#include <unistd.h>
#include <iostream>
#include <stdio.h>
#include <cstring>

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
            for (int i = 0; i < argc; i++) {
                arguments.push_back(std::string{args[i]});
            }
        }

        arguments_cont_t get_args() const {
            return arguments;
        }
    };

    bool write_all(int fd, const void* buffer, size_t count) {
    const char* ptr = static_cast<const char*>(buffer);
    size_t remaining = count;

    while (remaining > 0) {
        ssize_t written = write(fd, ptr, remaining);
        
        if (written == -1) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "Write error: " << strerror(errno) << std::endl;
            return false;
        }
        
        ptr += written;
        remaining -= written;
    }
    return true;
}
}

// --- EVENT & DATA STRUCTURES ---

namespace event_data {
    
    // Types of events sent through the event_pipe
    enum class EventType : int {
        NEW_TEST          = 0, // Reader has sent a new test name
        STDIN_CLOSED      = 1, // Reader has finished (EOF)
        EVT_DATA_READY    = 2, // A Waiter detected the first byte of data
        EVT_TEST_FINISHED = 4, // A Saver finished writing result to file
        EVT_ERROR         = 5  // Something went wrong
    };

    struct PolicyWorker {
        ssize_t id;             // Indeks w wektorze
        pid_t pid;          // PID procesu
        int pipe_in;        // Do pisania (Input Polityki)
        int pipe_out;       // Do czytania (Output Polityki)
        bool is_busy;       // Czy aktualnie mieli jakiś test?
        ssize_t current_test_id = -1;
        pid_t waiter_pid;
    };

    // The message packet sent through the pipe
    struct Event {
        EventType type;
        ssize_t test_id = -1;
        ssize_t policy_id = -1;
        char data_byte; // The "latch byte" - the first byte read by a Waiter
    };

    // The main database record for a Test
}

#endif