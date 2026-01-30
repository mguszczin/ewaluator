project(evaluator LANGUAGES C CXX)
add_executable(evaluator 
    main.cpp
    reader.cpp
    worker_copier.cpp 
    worker_waiter.cpp
)
target_link_libraries(evaluator err)
target_link_libraries(evaluator pthread)
target_link_libraries(evaluator rt)

target_include_directories(evaluator PRIVATE ../common)

add_compile_definitions(ORDERED_OUTPUT=1)
