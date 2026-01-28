#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {
    
    using std::string;
    using std::move;
    using std::vector;

    struct TestData {
        size_t test_number;
        string test_name;
        string test_status;

        TestData(size_t test_number, string test_name) 
            : test_number(test_number),
              test_name(move(test_name)) {};
    };

    struct EvaluatorData {
        string policy_path;
        string env_path;
        int max_concurrent_policy_calls;
        int max_concurrent_calls;
        int max_active_environments;
        vector<string> extra_arguments;

        EvaluatorData(string p_path, 
                    string e_path, 
                    int max_p_calls, 
                    int max_total_calls,
                    int max_envs, 
                    vector<string> extra_args = {})
            : policy_path(move(p_path)), 
            env_path(move(e_path)), 
            max_concurrent_policy_calls(max_p_calls),
            max_concurrent_calls(max_total_calls),
            max_active_environments(max_envs),
            extra_arguments(move(extra_args)) 
        {}
};

    class ParseReader {
        private:
            vector<string> arguments;
        public:
            ParseReader(int argc, char** args)
            {
                for (int i = 1; i < argc; i++) {
                    arguments.push_back(string{args[i]});
                }
            }

            EvaluatorData get_eval_info()
            {
                if (arguments.size() < 5) {
                    throw std::invalid_argument(
                        "Ewaluator Requires at least 5 arguments");
                }
                using std::stoi;
                const string p_path = arguments[1];
                const string e_path = arguments[2];
                int max_con_policy_calls = stoi(arguments[3]);
                int max_con_calls = stoi(arguments[4]);
                int max_act_env = stoi(arguments[5]);
                vector<string> additional;
                std::copy(arguments.begin() + 6, arguments.end(), 
                                        std::back_inserter(additional));
                return EvaluatorData(p_path, 
                                     e_path,
                                     max_con_policy_calls,
                                     max_con_calls,
                                     max_act_env,
                                     additional);
            }
        
    };

    class Evaluator {
        private:
            EvaluatorData eval_info;
        public: 
            Evaluator(EvaluatorData args): eval_info(move(args)) {};

            void start()
            {

            }
    };

}

int main(int argc, char** argv)
{
    ParseReader reader(argc, argv);
    Evaluator eval(reader.get_eval_info());
    try {
        eval.start();
    } catch(...) { // correct this
        std::cerr << "OH NO EXCEPTION" << std::endl;
        return 1;
    }

}