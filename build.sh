cmake -S . -B obj
cmake --build obj -j$(nproc)
