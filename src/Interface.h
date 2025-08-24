#pragma once
#include <atomic>

class Interface {
public:
    int Initialize();
    int Render(std::atomic<bool>* runningFlag);
    static void Minimize();
    static void Show();
private:
};