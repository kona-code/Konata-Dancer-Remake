#pragma once
#include <atomic>

class Interface {
public:
    int Initialize();
    int Render(std::atomic<bool>* runningFlag);
    static void Show();
    bool stats = false;
private:
    static void Minimize();
};