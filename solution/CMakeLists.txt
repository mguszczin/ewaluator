project(evaluator LANGUAGES C CXX)
add_executable(evaluator evaluator.cpp) 

add_executable(reader reader.cpp)
add_executable(waiter waiter.cpp)

target_link_libraries(evaluator err pthread rt)
target_link_libraries(reader err pthread rt)
target_link_libraries(waiter err pthread rt)

target_include_directories(evaluator PRIVATE ../common)

add_compile_definitions(ORDERED_OUTPUT=1)
