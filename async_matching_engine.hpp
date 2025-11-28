#pragma once

#include "matching_engine.hpp"
#include <boost/lockfree/spsc_queue.hpp>
#include <atomic>
#include <thread>
#include <memory>
#include <utility>

namespace matching{

//simple single producer / single consumer async wrapper
class AsyncMatchingEngine{
public:
    using TradeCallback = MatchingEngine::TradeCallback;

    explicit AsyncMatchingEngine(TradeCallback cb, std::size_t queue_capacity = 1 << 20)
    : engine_(std::move(cb)), queue_(queue_capacity), running_(true), worker_(&AsyncMatchingEngine::runLoop, this){}

    ~AsyncMatchingEngine(){stop();}

    //non-copyable
    AsyncMatchingEngine(const AsyncMatchingEngine&) = delete;
    AsyncMatchingEngine& operator=(const AsyncMatchingEngine&) = delete;

    //submit an event from the producer thread
    //will spin until the queue has space (simple backoff)
    void submit(const Event& e){
        Event* copy = new Event(e);
        while(!queue_.push(copy)){
            //in real sys, add better backoff
            std::this_thread::yield();
        }
    }

    //stop the worker thread
    void stop(){
        bool expected = true;
        if(running_.compare_exchange_strong(expected, false)){
            //wake up worker if its sleeping on an empty queue
            //by pushing a null ptr sentinel
            while(!queue_.push(nullptr)){std::this_thread::yield();}
            if(worker_.joinable()){worker_.join();}
        }
    }

    MatchingEngine& engine(){return engine_;}
    const MatchingEngine& engine() const{return engine_;}

private:
    MatchingEngine engine_;
    boost::lockfree::spsc_queue<Event*> queue_;
    std::atomic<bool> running_;
    std::thread worker_;

    void runLoop(){
        Event* ep = nullptr;
        while(true){
            while(queue_.pop(ep)){
                if(ep == nullptr){
                    //sentinel -> terminate
                    return;
                }
                engine_.process(*ep);
                delete ep;
            }
            //if we're asked to stop and queue is empty, exit
            if(!running_.load(std::memory_order_relaxed) && queue_.empty()){break;}
        }
        std::this_thread::yield();
    }
};
}