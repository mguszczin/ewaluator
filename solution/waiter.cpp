#include <unistd.h>
#include <iostream>
#include <string>
#include <vector>
#include <cerrno> 
#include <cstring>
#include <stdexcept>
#include "defs.h" 

using namespace std;
using namespace event_data;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " <test_id>" << endl;
        return 1;
    }

    size_t test_id = 0;
    try {
        test_id = stoull(argv[1]);
    } catch (const exception& ex) {
        cerr << "Error: Invalid Test ID provided. " << ex.what() << endl;
        return 1;
    }

    char c = 0;
    ssize_t r;

    do {
        r = read(STDIN_FILENO, &c, 1);
    } while (r == -1 && errno == EINTR);

    Event e = {}; 
    e.test_id = test_id;

    if (r > 0) {
        e.type = EventType::EVT_DATA_READY;
        e.data_byte = c;
    } else {
        cerr << "error in waiter" << endl;
        e.type = EventType::EVT_ERROR;
        e.data_byte = 0;
        
        if (r == -1) {
            cerr << "Warning: Read failed: " << strerror(errno) << endl;
        }
    }
    if (!utils::write_all(STDOUT_FILENO, &e, sizeof(e))) {
        cerr << "Error: Failed to write event to stdout: " << strerror(errno) << endl;
        return 1;
    }

    return 0;
}