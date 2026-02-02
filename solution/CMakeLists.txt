project(evaluator LANGUAGES C CXX)
add_executable(evaluator evaluator.cpp) 
target_link_libraries(evaluator err pthread rt)

add_compile_definitions(ORDERED_OUTPUT=1)
