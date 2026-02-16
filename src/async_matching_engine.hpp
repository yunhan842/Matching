#pragma once

#include "matching_engine.hpp"
#include <boost/lockfree/spsc_queue.hpp>
#include <atomic>
#include <thread>
#include <utility>

namespace matching{

//single producer / single consumer async wrapper
//uses value-based SPSC queue (no heap allocation per event)
class AsyncMatchingEngine{
public:
    using TradeCallback = MatchingEngine::TradeCallback;

    explicit AsyncMatchingEngine(TradeCallback cb, std::size_t queue_capacity = 1 << 20)
    : engine_(std::move(cb)), queue_(queue_capacity), running_(true),
      worker_(&AsyncMatchingEngine::runLoop, this){}

    ~AsyncMatchingEngine(){stop();}

    //non-copyable
    AsyncMatchingEngine(const AsyncMatchingEngine&) = delete;
    AsyncMatchingEngine& operator=(const AsyncMatchingEngine&) = delete;

    //submit an external Event (resolves string symbol → SymbolId, then pushes value)
    void submit(const Event& e){
        InternalEvent ie{};
        ie.symbol = engine_.resolveSymbol(e.symbol);
        ie.type = e.type;
        ie.side = e.side;
        ie.price = e.price;
        ie.qty = e.qty;
        ie.id = e.id;
        ie.tif = e.tif;
        ie.user_id = e.user_id;
        while(!queue_.push(ie)){
            std::this_thread::yield();
        }
    }

    //submit a pre-resolved InternalEvent directly (hot path, zero allocation)
    void submit(const InternalEvent& ie){
        while(!queue_.push(ie)){
            std::this_thread::yield();
        }
    }

    //stop the worker thread
    void stop(){
        bool expected = true;
        if(running_.compare_exchange_strong(expected, false)){
            //push stop sentinel
            InternalEvent sentinel{};
            sentinel.type = EventType::Stop;
            while(!queue_.push(sentinel)){std::this_thread::yield();}
            if(worker_.joinable()){worker_.join();}
        }
    }

    MatchingEngine& engine(){return engine_;}
    const MatchingEngine& engine() const{return engine_;}

private:
    MatchingEngine engine_;
    boost::lockfree::spsc_queue<InternalEvent> queue_;
    std::atomic<bool> running_;
    std::thread worker_;

    void runLoop(){
        InternalEvent ie{};
        while(true){
            while(queue_.pop(ie)){
                if(ie.type == EventType::Stop){return;}
                engine_.processInternal(ie);
            }
            //queue empty: check if we should exit
            if(!running_.load(std::memory_order_relaxed)){break;}
            std::this_thread::yield();
        }
    }
};
}
