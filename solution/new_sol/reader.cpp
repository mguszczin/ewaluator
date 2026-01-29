#include "defs.h"
#include "err.h"

#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

using namespace std;
using namespace event_data;

int main(int argc, char* argv[]) {
    if (argc < 3) return 1;

    int names_pipe_fd = atoi(argv[1]);
    int event_pipe_fd = atoi(argv[2]);

    string command;

    while (cin >> command) {
        
        vector<char> buffer(NAME_SIZE, 0);
        
        size_t len = command.length();
        if (len > NAME_SIZE) len = NAME_SIZE;
        command.copy(buffer.data(), len);

        if (write(names_pipe_fd, buffer.data(), NAME_SIZE) < 0) {
            break;
        }

        Event e;
        e.type = EventType::NEW_TEST;
        e.test_id = -1;
        e.data_byte = 0;
        
        if (write(event_pipe_fd, &e, sizeof(e)) < 0) {
            break; 
        }
    }

    Event e = {EventType::STDIN_CLOSED, -1, 0};
    write(event_pipe_fd, &e, sizeof(e));

    close(names_pipe_fd);
    close(event_pipe_fd);
    
    return 0;
}