#include <unistd.h>
#include <cstdlib>
#include <iostream>
#include <string>
#include "defs.h"

using namespace std;
using namespace event_data;

int main(int argc, char* argv[]) {
    if (argc < 3) return 1;

    // argv[1] = Event Pipe Write FD
    // argv[2] = Test ID
    int event_fd = atoi(argv[1]);
    size_t test_id = stoull(argv[2]);

    char c;
    // Blocking read from STDIN (which is dup2'd to the pipe we are watching)
    ssize_t r = read(STDIN_FILENO, &c, 1);

    Event e;
    e.test_id = test_id;

    if (r > 0) {
        e.type = EventType::EVT_DATA_READY;
        e.data_byte = c;
    } else {
        // Pipe closed or error
        e.type = EventType::EVT_ERROR;
        e.data_byte = 0;
    }

    // Report back to Evaluator
    if (write(event_fd, &e, sizeof(e)) < 0) {
        return 1;
    }

    return 0;
}