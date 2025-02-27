# C++ example
add_executable(cpp_example cpp_example.cpp)
target_link_libraries(cpp_example PRIVATE j1939sim)

# C example
add_executable(c_example c_example.c)
target_link_libraries(c_example PRIVATE j1939sim)
