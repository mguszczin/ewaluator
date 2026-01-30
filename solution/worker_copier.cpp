#include <unistd.h>
#include <cstdlib>
#include <vector>
#include <string>
#include "defs.h"

using namespace std;
using namespace event_data;

int main(int argc, char* argv[]) {
    if (argc < 5) return 1;

    // argv[1] = Event Pipe Write FD
    // argv[2] = Test ID
    // argv[3] = Latch Byte (int)
    // argv[4] = Size to read
    
    int event_fd = atoi(argv[1]);
    size_t test_id = stoull(argv[2]);
    char latch_byte = (char)atoi(argv[3]);
    size_t size = stoull(argv[4]);

    if (write(STDOUT_FILENO, &latch_byte, 1) < 0) return 1;

    size_t bytes_to_copy = size - 1;
    vector<char> buffer(bytes_to_copy);
    
    size_t total_read = 0;
    while(total_read < bytes_to_copy) {
        ssize_t r = read(STDIN_FILENO, buffer.data() + total_read, bytes_to_copy - total_read);
        if (r <= 0) break; 
        total_read += r;
    }

    if (total_read > 0) {
        write(STDOUT_FILENO, buffer.data(), total_read);
    }

    Event e;
    e.type = EventType::EVT_TRANSFER_DONE;
    e.test_id = test_id;
    e.data_byte = 0;

    if(write(event_fd, &e, sizeof(e)) < 0) return 1;

    return 0;
}