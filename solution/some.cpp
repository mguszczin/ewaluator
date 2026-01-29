#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <atomic>
#include <csignal>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdexcept>

#ifndef NAME_SIZE
#define NAME_SIZE 10
#endif
#ifndef STATE_SIZE
#define STATE_SIZE 10
#endif
#ifndef ACTION_SIZE
#define ACTION_SIZE 10
#endif

using std::string;
using std::vector;
using std::atomic;

volatile sig_atomic_t shutdown_requested = 0;


struct EvaluatorData {
    string policy_path;
    string env_path;
    int max_concurrent_policy_calls;
    int max_concurrent_calls;
    int max_active_env;
    vector<string> extra_arguments;

    EvaluatorData(string p, string e, int mp, int mt, int me, vector<string> ex)
        : policy_path(std::move(p)), env_path(std::move(e)),
          max_concurrent_policy_calls(mp), max_concurrent_calls(mt),
          max_active_env(me), extra_arguments(std::move(ex)) {}
};

bool read_exact(int fd, char* buf, size_t count) {
    size_t total = 0;
    while (total < count) {
        ssize_t ret = read(fd, buf + total, count - total);
        if (ret == 0) return false; // EOF
        if (ret < 0) {
            if (errno == EINTR) continue;
            return false; // Error
        }
        total += ret;
    }
    return true;
}

bool write_all(int fd, const char* buf, size_t count) {
    size_t total = 0;
    while (total < count) {
        ssize_t ret = write(fd, buf + total, count - total);
        if (ret < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        total += ret;
    }
    return true;
}

void run_test_supervisor(string test_name, const EvaluatorData& config) {
    int env_to_sup[2]; // Stan -> Nadzorca
    int sup_to_pol[2]; // Stan -> Polityka
    int pol_to_sup[2]; // Akcja -> Nadzorca
    int sup_to_env[2]; // Akcja -> Środowisko

    if (pipe(env_to_sup) == -1 || pipe(sup_to_pol) == -1 || 
        pipe(pol_to_sup) == -1 || pipe(sup_to_env) == -1) {
        exit(1);
    }

    // --- URUCHAMIANIE POLITYKI ---
    pid_t policy_pid = fork();
    if (policy_pid == 0) {
        // DZIECKO: POLITYKA
        // Reset sygnałów
        signal(SIGINT, SIG_DFL); signal(SIGCHLD, SIG_DFL);

        // Przekierowanie I/O
        dup2(sup_to_pol[0], STDIN_FILENO);
        dup2(pol_to_sup[1], STDOUT_FILENO);

        // Zamknięcie wszystkich rur
        close(env_to_sup[0]); close(env_to_sup[1]);
        close(sup_to_pol[0]); close(sup_to_pol[1]);
        close(pol_to_sup[0]); close(pol_to_sup[1]);
        close(sup_to_env[0]); close(sup_to_env[1]);

        string idx_str = "0"; 
        vector<char*> args;
        args.push_back(const_cast<char*>(config.policy_path.c_str()));
        args.push_back(const_cast<char*>(idx_str.c_str()));
        for(const auto& s : config.extra_arguments) args.push_back(const_cast<char*>(s.c_str()));
        args.push_back(nullptr);

        execv(config.policy_path.c_str(), args.data());
        exit(1);
    }

    pid_t env_pid = fork();
    if (env_pid == 0) {
        // DZIECKO: ŚRODOWISKO
        signal(SIGINT, SIG_DFL); signal(SIGCHLD, SIG_DFL);

        dup2(sup_to_env[0], STDIN_FILENO);
        dup2(env_to_sup[1], STDOUT_FILENO);

        close(env_to_sup[0]); close(env_to_sup[1]);
        close(sup_to_pol[0]); close(sup_to_pol[1]);
        close(pol_to_sup[0]); close(pol_to_sup[1]);
        close(sup_to_env[0]); close(sup_to_env[1]);

        vector<char*> args;
        args.push_back(const_cast<char*>(config.env_path.c_str()));
        args.push_back(const_cast<char*>(test_name.c_str()));
        for(const auto& s : config.extra_arguments) args.push_back(const_cast<char*>(s.c_str()));
        args.push_back(nullptr);

        execv(config.env_path.c_str(), args.data());
        exit(1);
    }

    close(env_to_sup[1]); // Środowisko tu pisze
    close(sup_to_pol[0]); // Polityka tu czyta
    close(pol_to_sup[1]); // Polityka tu pisze
    close(sup_to_env[0]); // Środowisko tu czyta

    // Bufory (+1 na znak nowej linii)
    vector<char> state_buf(STATE_SIZE + 1);
    vector<char> action_buf(ACTION_SIZE + 1);

    while (true) {
        // 1. Czytaj STAN ze środowiska
        if (!read_exact(env_to_sup[0], state_buf.data(), STATE_SIZE + 1)) break;

        // 2. Sprawdź warunek końca ('T')
        if (state_buf[0] == 'T') {
            // Sukces! Wypisz wynik (atomic write na stdout, żeby się nie pomieszało)
            // Format: NAZWA spacja STAN\n  (stan ma już \n na końcu)
            std::cout << test_name << " " << string(state_buf.data(), STATE_SIZE + 1);
            std::cout.flush();
            break; 
        }

        // 3. Wyślij STAN do Polityki
        if (!write_all(sup_to_pol[1], state_buf.data(), STATE_SIZE + 1)) break;

        // 4. Czytaj AKCJĘ z Polityki
        if (!read_exact(pol_to_sup[0], action_buf.data(), ACTION_SIZE + 1)) break;

        // 5. Wyślij AKCJĘ do Środowiska
        if (!write_all(sup_to_env[1], action_buf.data(), ACTION_SIZE + 1)) break;
    }

    // Sprzątanie
    close(env_to_sup[0]); close(sup_to_pol[1]);
    close(pol_to_sup[0]); close(sup_to_env[1]);

    // Czekamy na dzieci (Environment i Policy)
    kill(policy_pid, SIGTERM); // Dla pewności, jeśli env skończyło wcześniej
    kill(env_pid, SIGTERM);
    waitpid(policy_pid, nullptr, 0);
    waitpid(env_pid, nullptr, 0);

    exit(0); // Nadzorca kończy pracę
}

// --- GŁÓWNA KLASA ---

class Evaluator {
    EvaluatorData config;
    
    // Licznik aktywnych testów (każdy test = 1 Nadzorca = 1 Env + 1 Pol)
    atomic<int> active_tests{0}; 
    
    static Evaluator* self; // Wskaźnik dla handlera sygnałów

    // Handler SIGCHLD (zbiera martwych Nadzorców)
    static void sigchld_handler(int) {
        if (!self) return;
        int saved_errno = errno;
        while (waitpid(-1, nullptr, WNOHANG) > 0) {
            self->active_tests--;
        }
        errno = saved_errno;
    }

    // Handler SIGINT
    static void sigint_handler(int) {
        shutdown_requested = 1;
        // sigaction z flagą 0 przerwie syscalla (np. read/pause)
    }

public:
    Evaluator(EvaluatorData c) : config(std::move(c)) { self = this; }

    void start() {
        // Konfiguracja sygnałów
        struct sigaction sa_int = {}, sa_chld = {};
        
        sa_int.sa_handler = sigint_handler;
        sigemptyset(&sa_int.sa_mask);
        sa_int.sa_flags = 0; // Brak SA_RESTART -> przerwij I/O przy SIGINT

        sa_chld.sa_handler = sigchld_handler;
        sigemptyset(&sa_chld.sa_mask);
        sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;

        sigaction(SIGINT, &sa_int, nullptr);
        sigaction(SIGCHLD, &sa_chld, nullptr);

        string line;
        while (!shutdown_requested) {
            // Czytanie nazwy testu
            if (!std::getline(std::cin, line)) {
                if (errno == EINTR && !shutdown_requested) {
                    std::cin.clear(); continue;
                }
                break; // EOF
            }
            if (line.empty()) continue;
            // Oczekiwanie na zasoby (limity)
            wait_for_slot();

            if (shutdown_requested) break;

            // Start testu
            start_supervisor(line);
        }

        cleanup();
    }

private:
    void wait_for_slot() {
        
        while (!shutdown_requested) {
            int current_p = active_tests;
            
            bool cond_policy = (current_p + 1 <= config.max_concurrent_policy_calls);
            bool cond_env    = (current_p + 1 <= config.max_active_env);
            bool cond_total  = ((current_p * 2) + 2 <= config.max_concurrent_calls);

            if (cond_policy && cond_env && cond_total) {
                break;
            }
            
            pause();
        }
    }

    void start_supervisor(const string& test_name) {
        pid_t pid = fork();
        if (pid == -1) {
            std::cerr << "Fork failed\n";
            return;
        }

        if (pid == 0) {
            // Proces Nadzorcy
            // Reset handlerów, żeby Nadzorca nie łapał sygnałów Evaluatora
            signal(SIGCHLD, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            
            run_test_supervisor(test_name, config);
            // run_test_supervisor kończy się exit(0)
            exit(0);
        } else {
            // Proces Główny
            active_tests++;
        }
    }

    void cleanup() {
        // Czekamy na zakończenie wszystkich dzieci
        while (active_tests > 0) {
            pause(); // Czekamy na SIGCHLD
        }
    }
};

Evaluator* Evaluator::self = nullptr;

int main(int argc, char** argv) {
    if (argc < 6) return 1;

    try {
        vector<string> extra;
        for(int i=6; i<argc; ++i) extra.push_back(argv[i]);

        EvaluatorData data(argv[1], argv[2], 
                           std::stoi(argv[3]), std::stoi(argv[4]), std::stoi(argv[5]), 
                           extra);
        
        Evaluator app(data);
        app.start();

        return shutdown_requested ? 2 : 0;

    } catch (...) {
        return 1;
    }
}