#include <mutex>
#include <thread>
#include <iostream>
#include <chrono>


std::mutex a;
std::mutex b;

void threadA_func()
{
    std::cout<<"线程A上锁a"<<std::endl; 
    
    std::unique_lock<std::mutex> lock1(a);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::unique_lock<std::mutex> lock2(b);

    lock2.unlock();
    lock1.unlock();
}

void threadB_func()
{
    std::cout<<"线程B上锁b"<<std::endl; 
    
    std::unique_lock<std::mutex> lock1(b);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::unique_lock<std::mutex> lock2(a);

    lock2.unlock();
    lock1.unlock();
}


int main()
{
    std::thread A(threadA_func);
    std::thread B(threadB_func);

    A.join();
    B.join();

    return 0;
}