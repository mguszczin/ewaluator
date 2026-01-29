#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <queue>
#include <csignal>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <cstring>
#include <atomic>
#include <algorithm>
#include <functional>

#include "defs.h"
#include "err.h"

using namespace std;
using namespace event_data;


volatile atomic_bool stop_requested = false;

void handle_sigint(int) {
    stop_requested = true;
}


pair<int, int> create_pipe() {
    int pd[2];
    if (pipe(pd) != 0) std::ios_base::failure("Pipe failed");
    return {pd[0], pd[1]};
}

string read_fixed_string(int fd, size_t size) {
    string s(size, '\0');
    ssize_t r = read(fd, &s[0], size);
    if (r < 0) return "";
    s.resize(strlen(s.c_str()));
    return s;
}

struct EvaluatorData {
    string policy_path;
    string env_path;
    size_t max_active_env;
    size_t max_calls;
    size_t max_policy;

    EvaluatorData(const vector<string>& args) {
        if (args.size() < 5) 
            throw std::invalid_argument("To little arguments");
        policy_path = args[0];
        env_path = args[1];
        max_policy = stoul(args[2]);
        max_calls = stoul(args[3]);
        max_active_env = stoul(args[4]);
    }
};

class Evaluator {
private:
    EvaluatorData config;
    
    int names_read_fd;
    int event_write_fd;
    pid_t reader_pid;

    size_t active_calls = 0;
    size_t concurrent_policy = 0;
    size_t active_env = 0;

    map<int, Test> tests;
    
    queue<int> new_tests;
    queue<int> policy_wait;
    queue<int> env_wait;

    size_t next_id = 0;

    map<EventType, function<void(Event)>> commands;

    /**
     * Creates Reader process.
     * 
     * After this operation main Process has two new file descriptors.
     * -`names_read_fd` - for reading names of tests.
     * -`event_write_fd` - we will pass this file descriptor to our subprocesses.
     *   It will let them notify us about events.
     * - replaced STDIN_FILENO with `event_read_fd`
     *   because evaluator will now just read events.
     * 
     *  Now new processes neeed to replace STDIN_FILENO,
     *  all processes will want `event_write_fd`
     */
    pid_t spawn_reader(int& read_names_fd, int& event_writer_fd) {
        auto [name_reader, name_writer] = create_pipe();
        auto [event_reader, event_writer] = create_pipe();

        pid_t pid = fork();
        if (pid == -1) {
            throw std::ios_base::failure("Fork failed");
        }

        if (pid == 0) {
            close(name_reader);
            close(event_reader);
            execl("./reader",
                "./reader",
                to_string(name_writer).c_str(),
                to_string(event_writer).c_str(),
                NULL);

            syserr("exec reader failed");
        }

        close(name_writer); 
        
        if (dup2(event_reader, STDIN_FILENO) == -1) syserr("dup2 failed");
        close(event_reader);

        read_names_fd = name_reader;
        fcntl(read_names_fd, F_SETFD, FD_CLOEXEC);

        /* We will pass `event_writer` to workers so don't close */
        event_writer_fd = event_writer;
        return pid;
    }

    /**
     * Spawns waiter sub-process.
     * 
     * Gives reading file descriptor and id
     * of test - to then pass it in event.
     * Passes only fd_to_watch as stdin, event_write_fd.
     */
    pid_t spawn_waiter(int fd_to_watch, int id) {
        pid_t worker = fork();

        if (worker == -1)
            throw std::ios_base::failure("fork failed inside `spawn waiter`");

        if (worker == 0) { 
            if (dup2(fd_to_watch, STDIN_FILENO) == -1) {
                perror("dup2 failed in waiter");
                exit(1);
            }
            close(fd_to_watch);
            
            execl("./worker_waiter", "worker_waiter", 
                to_string(event_write_fd).c_str(), 
                to_string(id).c_str(), NULL);
            
            perror("execl failed");
            exit(1);
        }

        return worker;
    }

    /**
     * Copies data from src do dst, so that evaluator doesn't have to take it's time to
     * do that.
     * 
     * Copies from src -> dst, and takes event_writer to notify about succes.
     */
    pid_t spawn_copier(int src, int dst, int id, char latch, size_t size) {
        pid_t worker = fork();
        if (!worker) {
            dup2(src, STDIN_FILENO);
            dup2(dst, STDOUT_FILENO);
            close(src); close(dst);

            execl("./worker_copier", "worker_copier",
                  to_string(event_write_fd).c_str(),
                  to_string(id).c_str(),
                  to_string((int)latch).c_str(),
                  to_string(size).c_str(), NULL);
            exit(1);
        }
    }

    /**
     * Spawns new enviroment process from `test_id`
     * 
     * Additionally uses 
     */
    void spawn_environment(int id) {
        auto& test = tests[id];
        
        // p_to_child:   Rodzic pisze [1] -> Dziecko czyta [0]
        // p_from_child: Dziecko pisze [1] -> Rodzic czyta [0]
        int p_to_child[2];
        int p_from_child[2];

        if (pipe(p_to_child) == -1) syserr("pipe input");
        if (pipe(p_from_child) == -1) syserr("pipe output");

        test.env_pid = fork();
        if (test.env_pid == -1) syserr("fork env");

        if (test.env_pid == 0) {
            dup2(p_to_child[0], STDIN_FILENO);

            dup2(p_from_child[1], STDOUT_FILENO);

            close(p_to_child[0]); close(p_to_child[1]);
            close(p_from_child[0]); close(p_from_child[1]);
            
            close(event_write_fd); 

            execl(config.env_path.c_str(), 
                  config.env_path.c_str(),
                  test.name.c_str(),
                  NULL);
            
            syserr("exec env failed");
        }

        close(p_to_child[0]);
        close(p_from_child[1]);

        test.env_in = p_to_child[1];
        test.env_out = p_from_child[0];

        fcntl(test.env_in, F_SETFD, FD_CLOEXEC);
        fcntl(test.env_out, F_SETFD, FD_CLOEXEC);
        test.current_worker_pid = spawn_waiter(test.env_out, id);
        
        test.state = TestState::WAITING_FOR_ENV_OUTPUT;
    }

    /**
     * to delete probably useless
     */
    void spawn_saver(int src, int id, char latch, string fname) {
        if (fork() == 0) {
            int fd = open(fname.c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0666);
            if (fd < 0) _exit(1);

            // Zapisz latch byte
            write(fd, &latch, 1);
            
            // Przepisz resztę z rury środowiska
            char buf[1024];
            ssize_t n;
            while((n = read(src, buf, sizeof(buf))) > 0) {
                write(fd, buf, n);
            }
            close(fd);
            
            // Raport końca
            Event e;
            e.type = EventType::EVT_TEST_FINISHED;
            e.test_id = id;
            write(event_write_fd, &e, sizeof(e));
            exit(0);
        }
    }

    /** Cleans everything when getting SIGINT */
    void perform_shutdown() {

        for (auto& [id, t] : tests) {
            if (t.env_pid > 0) kill(t.env_pid, SIGINT);
            if (t.policy_pid > 0) kill(t.policy_pid, SIGINT);
            if (t.current_worker_pid > 0) kill(t.current_worker_pid, SIGINT);

            if (t.env_in != -1) close(t.env_in);
            if (t.env_out != -1) close(t.env_out);
            if (t.pol_in != -1) close(t.pol_in);
            if (t.pol_out != -1) close(t.pol_out);
        }

        close(names_read_fd);
        close(event_write_fd);
        kill(reader_pid, SIGINT);
        waitpid(reader_pid, NULL, 0);

        for (auto& [id, t] : tests) {
            if (t.env_pid > 0) waitpid(t.env_pid, NULL, 0);
            if (t.policy_pid > 0) waitpid(t.policy_pid, NULL, 0);
            if (t.current_worker_pid) waitpid(t.current_worker_pid, NULL, 0);
        }
    }

    /**
     * Cleans everyting when getting test finished.
     */
    void cleanup_test(int id) {
        auto& t = tests[id];
        
        if (t.env_pid > 0) waitpid(t.env_pid, NULL, 0);
        
        if (t.policy_pid > 0) { 
            kill(t.policy_pid, SIGINT); 
            waitpid(t.policy_pid, NULL, 0); 
        }

        if (t.env_in != -1) close(t.env_in);
        if (t.env_out != -1) close(t.env_out);
        if (t.pol_in != -1) close(t.pol_in);
        if (t.pol_out != -1) close(t.pol_out);

        tests.erase(id);
        active_env--;
    }

    void schedule() {
        // Jeśli trwa zamykanie (SIGINT), nie uruchamiamy nic nowego
        if (stop_requested) return;

        // --- 1. Priorytet: Wznawianie Środowiska (Policy -> Env) ---
        // Wymaga: Wolnego CPU (active_calls)
        while (!q_cpu.empty() && active_calls < config.max_calls) {
            int id = q_cpu.front();
            q_cpu.pop();
            auto& t = tests[id];

            // Rezerwujemy CPU (Env zacznie liczyć jak tylko dostanie dane)
            active_calls++; 
            
            // Uruchamiamy transfer: Policy Output -> Environment Input
            spawn_copier(t.pol_out, t.env_in, id, t.latch_byte, ACTION_SIZE);
        }

        // --- 2. Priorytet: Uruchamianie Polityki (Env -> Policy) ---
        // Wymaga: Wolnego CPU (active_calls) ORAZ Wolnego GPU (concurrent_policy)
        while (!q_gpu.empty() && active_calls < config.max_calls && concurrent_policy < config.max_policy) {
            int id = q_gpu.front();
            q_gpu.pop();
            auto& t = tests[id];

            // Leniwe tworzenie procesu polityki (jeśli jeszcze nie istnieje)
            if (t.policy_pid == -1) {
                int pin[2], pout[2];
                // WAŻNE: Używamy pipe2 z O_CLOEXEC, żeby inne procesy tego nie ukradły!
                if (pipe2(pin, O_CLOEXEC) == -1 || pipe2(pout, O_CLOEXEC) == -1) {
                    syserr("pipe2 policy failed");
                }

                t.policy_pid = fork();
                if (t.policy_pid == -1) syserr("fork policy failed");

                if (t.policy_pid == 0) {
                    // Dziecko (Policy)
                    dup2(pin[0], STDIN_FILENO);
                    dup2(pout[1], STDOUT_FILENO);
                    
                    // Ponieważ użyliśmy O_CLOEXEC, wystarczy zamknąć te końcówki,
                    // które zduplikowaliśmy na stdin/stdout. Reszta zamknie się przy execl.
                    close(pin[0]); close(pin[1]); 
                    close(pout[0]); close(pout[1]);
                    
                    // Ręczne sprzątanie dziedziczonych zmiennych globalnych (dla pewności)
                    // Chociaż O_CLOEXEC na rurach głównych powinno to załatwić
                    close(names_read_fd); 
                    close(event_write_fd);
                    
                    execl(config.policy_path.c_str(), config.policy_path.c_str(), NULL);
                    _exit(1);
                }

                // Rodzic (Evaluator)
                close(pin[0]); close(pout[1]);
                t.pol_in = pin[1];
                t.pol_out = pout[0];
            }

            active_calls++;
            concurrent_policy++;

            spawn_copier(t.env_out, t.pol_in, id, t.latch_byte, STATE_SIZE);
        }

        while (!new_tests.empty() && active_calls < config.max_calls && active_env < config.max_active_env) {
            int id = new_tests.front();
            new_tests.pop();

            active_calls++;
            active_env++;

            spawn_environment(id);
        }
    }

    /** 
     * Creates a new test waiting for execution. 
     * 
     * It has at the beggining all values set to -1.
     */
    void handle_new_test(Event e)
    {
        string name = read_fixed_string(names_read_fd, NAME_SIZE);
        size_t id = next_id++;
        tests[id] = Test{ (size_t)id, name };
        new_tests.push(id);
    }

    /**
     * Handles the data which is ready to transfer.
     * 
     * Test gets the temporary `latch_byte` and waits in queue in order to
     * be handled by from queue in the future.
     */
    void handle_data_ready(Event e)
    {
        auto& t = tests[e.test_id];
        t.latch_byte = e.data_byte;
        active_calls--;

        if (t.state == TestState::WAITING_FOR_ENV_OUTPUT) {
            if (e.data_byte == 'T') {
                t.state = TestState::SAVING;
                spawn_saver(t.env_out, e.test_id, e.data_byte, t.name + ".res");
            } else {
                t.state = TestState::QUEUED_FOR_GPU;
                policy_wait.push(e.test_id);
            }
        } 
        else if (t.state == TestState::WAITING_FOR_POLICY_OUTPUT) {
            concurrent_policy--;
            t.state = TestState::QUEUED_FOR_CPU;
            env_wait.push(e.test_id);
        }
    }

    void handle_transfer_done(Event e)
    {
        auto& t = tests[e.test_id];
        // Transfer skończony -> Uruchom Waitera u celu
        
        if (t.state == TestState::QUEUED_FOR_GPU) {
            // Copier skończył słać do Polityki -> Nasłuchuj Polityki
            t.state = TestState::WAITING_FOR_POLICY_OUTPUT;
            spawn_waiter(t.pol_out, e.test_id);
        } 
        else if (t.state == TestState::QUEUED_FOR_CPU) {
            // Copier skończył słać do Env -> Nasłuchuj Env
            t.state = TestState::WAITING_FOR_ENV_OUTPUT;
            spawn_waiter(t.env_out, e.test_id);
        }
    }

public:
    Evaluator(EvaluatorData data) : config(std::move(data)) {
        commands = {
        { EventType::NEW_TEST, [this](Event e) { 
            this->handle_new_test(e); 
        }},
        { EventType::EVT_DATA_READY, [this](Event e) { 
            this->handle_data_ready(e); 
        }},
        { EventType::EVT_TRANSFER_DONE, [this](Event e) { 
            this->handle_transfer_done(e); 
        }},
        { EventType::EVT_TEST_FINISHED, [this](Event e) { 
            this->cleanup_test(e.test_id);
        }},
        { EventType::STDIN_CLOSED, [](Event e) { 
            stop_requested = true;
        }},
        { EventType::EVT_ERROR, [](Event e) {
            std::cerr << 
                "Error reported from worker for test " << e.test_id 
                    << std::endl;
        }}
    };
    }

    void run() {
        reader_pid = spawn_reader(names_read_fd, event_write_fd);
        
        Event e;
        while (true) {

            if (stop_requested) {
                perform_shutdown();
                break;
            }

            ssize_t n = read(STDIN_FILENO, &e, sizeof(e));

            if (stop_requested || (n < 0 && errno == EINTR)) {
                perform_shutdown();
                break;
            }

            /* We got EOF. */
            if (n <= 0) break;

            commands.at(e.type)(e);
            schedule();
        }
        
        close(event_write_fd);
    }
};

int main(int argc, char* argv[]) {
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    try {
        utils::ParseReader parser(argc, argv);
        EvaluatorData config(parser.get_args());
        Evaluator app(config);
        app.run();
    } catch (const exception& e) {
        cerr << "Error: " << e.what() << endl;
        return 1;
    }

    return 0;
}