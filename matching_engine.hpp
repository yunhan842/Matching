#pragma once

#include "orderbook.hpp"
#include <unordered_map>
#include <string>
#include <optional>
#include <utility>
#include <cstdlib>
#include <cmath>

#define MATCHING_ENABLE_USER_TRACKING 1

namespace matching{

enum class EventType{NewLimit, NewMarket, Cancel, Replace};

struct Event{
    EventType type;
    std::string symbol;

    //for newlimit/newmarket
    Side side{Side::Buy};
    Price price{0};
    Qty qty{0};

    //for cancel
    OrderId id{0};
    TimeInForce tif{TimeInForce::GFD};
    UserId user_id{1};
};

struct TopOfBook{
    std::optional<Price> best_bid;
    std::optional<Qty> bid_size;
    std::optional<Price> best_ask;
    std::optional<Qty> ask_size;
    std::optional<Price> mid_price;
};

class MatchingEngine{
public:
    using TradeCallback = OrderBook::TradeCallBack;

    explicit MatchingEngine(TradeCallback cb = nullptr): 
        callback_(std::move(cb)){}
    
    struct UserSymbolPosition{
        Qty position{0}; //+long,-short
        Qty traded_volume{0}; //sum of qty traded
    };

    #ifdef MATCHING_ENABLE_USER_TRACKING
    //helper to inspect user positions
    std::optional<UserSymbolPosition> userPositions(UserId user, const std::string& symbol) const{
        auto itUser = user_positions_.find(user);
        if(itUser == user_positions_.end()){return std::nullopt;}
        auto itSymbol = itUser->second.find(symbol);
        if(itSymbol == itUser->second.end()){return std::nullopt;}
        return itSymbol->second;
    }
    #else
    //tracking disabled: always "no position"
    std::optional<UserSymbolPosition> userPositions(UserId, const std::string&) const{
        return std::nullopt;
    }
    #endif

    void setMaxPosition(Qty limit){max_abs_position_ = limit;}

    #ifdef MATCHING_ENABLE_USER_TRACKING
    void reserveOwnerMap(std::size_t n){owner_.reserve(n);}
    #else
    void reserveOwnerMap(std::size_t){/*no-op when tracking is disabled*/}
    #endif
    
    //core event processing api
    void process(const Event& e){
        switch(e.type){
        case EventType::NewLimit:
            newLimit(e.symbol, e.user_id, e.side, e.price, e.qty, e.tif);
            break;
        case EventType::NewMarket:
            newMarket(e.symbol, e.user_id, e.side, e.qty);
            break;
        case EventType::Cancel:
            cancel(e.symbol, e.id);
            break;
        case EventType::Replace:
        #ifdef MATCHING_ENABLE_USER_TRACKING
            //keep same user as old order if known
            UserId user = 1;
            if(auto it = owner_.find(e.id); it != owner_.end()){user = it->second;}
            {
                OrderId newId = replace(e.symbol, e.id, e.side, e.price, e.qty, e.tif);
                if(newId != 0){owner_[newId] = user;}
                owner_.erase(e.id);
            }
        #else
            //tracking disabled: simple cancel+new using default user semantics
            replace(e.symbol, e.id, e.side, e.price, e.qty, e.tif);
        #endif
            break;
        }
    }
    
    //old 4 arg signature -> also uses default user + default tif
    OrderId newLimit(const std::string& symbol, Side side, Price price, Qty qty){
        return newLimit(symbol, /*user*/ 1, side, price, qty, TimeInForce::GFD);
    }

    //convenience overload: no explicit user -> use default user 1, but still risk + owner aware
    OrderId newLimit(const std::string& symbol, Side side, Price price, Qty qty, TimeInForce tif){
        return newLimit(symbol, /*user*/ 1, side, price, qty, tif);
    }

    //core implementation: always user-aware, always risk-aware
    OrderId newLimit(const std::string& symbol, UserId user, Side side, Price price, Qty qty, TimeInForce tif = TimeInForce::GFD){
        #ifdef MATCHING_ENABLE_USER_TRACKING
        if(!checkRisk(user, symbol, side, qty)){return 0;} //risk reject: in a real system, might send an explicit reject message
        #endif

        auto& book = getOrCreateBook(symbol);

        #ifdef MATCHING_ENABLE_USER_TRACKING
        current_user_ = user;
        current_side_ = side;
        have_current_ = true;
        #endif

        OrderId id = book.addLimit(side, price, qty, tif);

        #ifdef MATCHING_ENABLE_USER_TRACKING
        have_current_ = false;
        if(id != 0){owner_[id] = user;}
        #endif

        return id;
    }

    //market order (price ignored, just crosses the book)
    OrderId newMarket(const std::string& symbol, Side side, Qty qty){
        return newMarket(symbol, /*user*/ 1, side, qty);
    }

    //core implementatiob
    OrderId newMarket(const std::string& symbol, UserId user, Side side, Qty qty){
        #ifdef MATCHING_ENABLE_USER_TRACKING
        if(!checkRisk(user, symbol, side, qty)){return 0;}
        #endif

        auto& book = getOrCreateBook(symbol);

        #ifdef MATCHING_ENABLE_USER_TRACKING
        current_user_ = user;
        current_side_ = side;
        have_current_ = true;
        #endif

        OrderId id = book.addMarket(side, qty);

        #ifdef MATCHING_ENABLE_USER_TRACKING
        have_current_ = false;
        if(id != 0){owner_[id] = user;}
        #endif

        return id; 
    }

    //cancel by symbol + order id
    bool cancel(const std::string& symbol, OrderId id){
        auto it = books_.find(symbol);
        if(it == books_.end()){return false;}
        return it->second.cancel(id);
    }

    //cancel/replace: cancel old_id in symbol, then submit new limit order
    //this loses original time priority (simple model)
    OrderId replace(const std::string& symbol, OrderId old_id, Side side, Price price, Qty qty, TimeInForce tif = TimeInForce::GFD){
        cancel(symbol, old_id);
        return newLimit(symbol, side, price, qty, tif);
    }

    //get top of book for a symbol
    TopOfBook topOfBook(const std::string& symbol) const{
        TopOfBook tob{};
        auto it = books_.find(symbol);
        if(it == books_.end()){return tob;}
        const OrderBook& book = it->second;
        tob.best_bid = book.bestBid();
        tob.bid_size = book.bestBidSize();
        tob.best_ask = book.bestAsk();
        tob.ask_size = book.bestAskSize();
        tob.mid_price = book.midPrice();
        return tob;
    }

    const OrderBook* findBook(const std::string& symbol) const{
        auto it = books_.find(symbol);
        if(it == books_.end()){return nullptr;}
        return &it->second;
    }

    std::optional<BookStats> bookStats(const std::string& symbol) const{
        auto it = books_.find(symbol);
        if(it == books_.end()){return std::nullopt;}
        return it->second.stats();
    }

private:
    //boost library's flap map over stl unordered map for performance
    TradeCallback callback_;
    using BookMap = boost::unordered_flat_map<std::string, OrderBook>;
    BookMap books_;

    #ifdef MATCHING_ENABLE_USER_TRACKING
    using OwnerMap = boost::unordered_flat_map<OrderId, UserId>;
    OwnerMap owner_;

    using UserPositionsMap = boost::unordered_flat_map<UserId, boost::unordered_flat_map<std::string, UserSymbolPosition>>;
    UserPositionsMap user_positions_;

    UserId current_user_{0};
    Side current_side_{Side::Buy};
    bool have_current_{false};
    #endif

    Qty max_abs_position_ = static_cast<Qty>(1'000'000'000); //big default (can be changed)

    OrderBook& getOrCreateBook(const std::string& symbol){
        auto it = books_.find(symbol);
        if(it == books_.end()){
            auto res = books_.emplace(symbol, OrderBook(symbol, [this](const Trade& t){
                handleTrade(t);
            }));
            return res.first->second;
        }
        return it->second;
    }

    void handleTrade(const Trade& t){
        #ifdef MATCHING_ENABLE_USER_TRACKING
        //update user positions if we know order owners
        auto itB = owner_.find(t.buy_id);
        auto itS = owner_.find(t.sell_id);

        //buy side
        if(itB != owner_.end()){
            UserId u = itB->second;
            auto& pos = user_positions_[u][t.symbol];
            pos.position += t.qty; //bought qty
            pos.traded_volume += t.qty;
        }
        else if(have_current_ && current_side_ == Side::Buy && t.buy_id != 0){
            //incoming buy order whose owner_ hasn't been recorded yet
            auto& pos = user_positions_[current_user_][t.symbol];
            pos.position += t.qty;
            pos.traded_volume += t.qty;
        }

        //sell side
        if(itS != owner_.end()){
            UserId u = itS->second;
            auto& pos = user_positions_[u][t.symbol];
            pos.position -= t.qty; //sold qty
            pos.traded_volume += t.qty;
        }
        else if(have_current_ && current_side_ == Side::Sell && t.sell_id != 0){
            //incoming sell order whose owner_ hasn't been recorded yet
            auto& pos = user_positions_[current_user_][t.symbol];
            pos.position -= t.qty;
            pos.traded_volume += t.qty;
        }
        #endif

        //forward to user callback
        if(callback_){callback_(t);}
    }

    #ifdef MATCHING_ENABLE_USER_TRACKING
    bool checkRisk(UserId user, const std::string& symbol, Side side, Qty qty) const{
        auto itUser = user_positions_.find(user);
        Qty curr = 0;
        if(itUser != user_positions_.end()){
            auto itSymbol = itUser->second.find(symbol);
            if(itSymbol != itUser->second.end()){curr = itSymbol->second.position;}
        }
        Qty newPos = curr + (side == Side::Buy ? qty: -qty);
        if(std::llabs(newPos) > max_abs_position_){return false;}
        return true;
    }
    #else
    //tracking disabled: always pass risk
    bool checkRisk(UserId, const std::string&, Side, Qty) const{return true;}
    #endif
};
}