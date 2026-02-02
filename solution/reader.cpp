#include "defs.h"
// #include "err.h" // Assuming this is your custom error handler, kept optional

#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>
#include <cstdlib> // for EXIT_FAILURE
#include <cstring> // for strerror
#include <cerrno>  // for errno, EINTR
#include <stdexcept>

using namespace std;
using namespace event_data;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " <event_pipe_fd>" << endl;
        return EXIT_FAILURE;
    }

    int event_pipe_fd = -1;
    try {
        event_pipe_fd = stoi(argv[1]);
    } catch (const exception& e) {
        cerr << "Error: Invalid file descriptor argument." << endl;
        return EXIT_FAILURE;
    }

    if (event_pipe_fd < 0) {
        cerr << "Error: File descriptor cannot be negative." << endl;
        return EXIT_FAILURE;
    }

    string command;

    while (cin >> command) {
        
        vector<char> buffer(NAME_SIZE, 0);
        
        size_t len = command.length();
        if (len > NAME_SIZE) len = NAME_SIZE; 

        command.copy(buffer.data(), len);

        if (!utils::write_all(STDOUT_FILENO, buffer.data(), NAME_SIZE)) {
            break;
        }

        Event e = {}; 
        e.type = EventType::NEW_TEST;
        e.test_id = -1;
        e.data_byte = 0;
        
        if (!utils::write_all(event_pipe_fd, &e, sizeof(e))) {
            break;
        }
    }

    Event e = {};
    e.type = EventType::STDIN_CLOSED;
    e.test_id = -1;
    e.data_byte = 0;

    utils::write_all(event_pipe_fd, &e, sizeof(e));

    close(event_pipe_fd);
    
    return 0;
}