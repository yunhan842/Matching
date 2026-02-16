#pragma once

#include "orderbook.hpp"
#include <deque>
#include <string>
#include <optional>
#include <utility>
#include <cstdlib>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>
#include <boost/unordered/unordered_flat_map.hpp>

#define MATCHING_ENABLE_USER_TRACKING 0

namespace matching{

enum class EventType: std::uint8_t {NewLimit, NewMarket, Cancel, Replace, Stop};

//external event: string symbol (for protocol / parsing layer)
struct Event{
    EventType type;
    std::string symbol;

    Side side{Side::Buy};
    Price price{0};
    Qty qty{0};

    OrderId id{0};
    TimeInForce tif{TimeInForce::GFD};
    UserId user_id{1};
};

//internal event: SymbolId (hot path, zero allocation, trivially copyable)
struct InternalEvent{
    SymbolId symbol;
    OrderId id;
    Price price;
    Qty qty;
    UserId user_id;
    EventType type;
    Side side;
    TimeInForce tif;
};

//maps string symbol names ↔ integer IDs
class SymbolIndex{
public:
    SymbolId getOrCreate(const std::string& name){
        auto it = to_id_.find(name);
        if(it != to_id_.end()){return it->second;}
        SymbolId id = static_cast<SymbolId>(names_.size());
        names_.push_back(name);
        to_id_[name] = id;
        return id;
    }

    std::optional<SymbolId> find(const std::string& name) const{
        auto it = to_id_.find(name);
        if(it != to_id_.end()){return it->second;}
        return std::nullopt;
    }

    const std::string& name(SymbolId id) const{return names_[id];}
    const char* nameCStr(SymbolId id) const{return names_[id].c_str();}
    std::size_t size() const{return names_.size();}

private:
    boost::unordered_flat_map<std::string, SymbolId> to_id_;
    std::deque<std::string> names_; //deque: stable pointers on push_back
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
    using TradeCallback = std::function<void(const Trade&)>;

    //internal callback: concrete type, devirtualized + inlinable
    struct InternalCallback{
        MatchingEngine* engine;
        void operator()(const Trade& t) const{engine->handleTrade(t);}
    };
    using BookType = OrderBook<InternalCallback>;

    explicit MatchingEngine(TradeCallback cb = nullptr):
        callback_(std::move(cb)){}

    //non-copyable, non-movable (InternalCallback stores `this`)
    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;
    MatchingEngine(MatchingEngine&&) = delete;
    MatchingEngine& operator=(MatchingEngine&&) = delete;

    struct UserSymbolPosition{
        Qty position{0};
        Qty traded_volume{0};
    };

    #if MATCHING_ENABLE_USER_TRACKING
    std::optional<UserSymbolPosition> userPositions(UserId user, const std::string& symbol) const{
        auto itUser = user_positions_.find(user);
        if(itUser == user_positions_.end()){return std::nullopt;}
        auto sid = symbols_.find(symbol);
        if(!sid){return std::nullopt;}
        auto itSym = itUser->second.find(*sid);
        if(itSym == itUser->second.end()){return std::nullopt;}
        return itSym->second;
    }
    #else
    std::optional<UserSymbolPosition> userPositions(UserId, const std::string&) const{
        return std::nullopt;
    }
    #endif

    void setMaxPosition(Qty limit){max_abs_position_ = limit;}

    #if MATCHING_ENABLE_USER_TRACKING
    void reserveOwnerMap(std::size_t n){owner_.reserve(n);}
    #else
    void reserveOwnerMap(std::size_t){}
    #endif

    //resolve string symbol → SymbolId
    SymbolId resolveSymbol(const std::string& name){return symbols_.getOrCreate(name);}
    const std::string& symbolName(SymbolId id) const{return symbols_.name(id);}

    //process external Event (string symbol → resolved internally)
    void process(const Event& e){
        InternalEvent ie{};
        ie.symbol = symbols_.getOrCreate(e.symbol);
        ie.type = e.type;
        ie.side = e.side;
        ie.price = e.price;
        ie.qty = e.qty;
        ie.id = e.id;
        ie.tif = e.tif;
        ie.user_id = e.user_id;
        processInternal(ie);
    }

    //process internal event (hot path, no string allocation)
    void processInternal(const InternalEvent& e){
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
            #if MATCHING_ENABLE_USER_TRACKING
            {
                UserId user = e.user_id;
                if(auto it = owner_.find(e.id); it != owner_.end()){user = it->second;}
                OrderId newId = replace(e.symbol, e.id, e.side, e.price, e.qty, e.tif);
                if(newId != 0){owner_[newId] = user;}
                owner_.erase(e.id);
            }
            #else
            replace(e.symbol, e.id, e.side, e.price, e.qty, e.tif);
            #endif
            break;
        case EventType::Stop:
            break;
        }
    }

    //--- convenience overloads (string symbols) ---

    OrderId newLimit(const std::string& symbol, Side side, Price price, Qty qty){
        return newLimit(symbols_.getOrCreate(symbol), UserId{1}, side, price, qty, TimeInForce::GFD);
    }

    OrderId newLimit(const std::string& symbol, Side side, Price price, Qty qty, TimeInForce tif){
        return newLimit(symbols_.getOrCreate(symbol), UserId{1}, side, price, qty, tif);
    }

    OrderId newLimit(const std::string& symbol, UserId user, Side side, Price price, Qty qty, TimeInForce tif = TimeInForce::GFD){
        return newLimit(symbols_.getOrCreate(symbol), user, side, price, qty, tif);
    }

    //--- core SymbolId-based methods ---

    OrderId newLimit(SymbolId symbol, UserId user, Side side, Price price, Qty qty, TimeInForce tif = TimeInForce::GFD){
        #if MATCHING_ENABLE_USER_TRACKING
        if(!checkRisk(user, symbol, side, qty)){return 0;}
        #endif

        auto& book = getOrCreateBook(symbol);

        #if MATCHING_ENABLE_USER_TRACKING
        current_user_ = user;
        current_side_ = side;
        have_current_ = true;
        #endif

        OrderId id = book.addLimit(side, price, qty, tif);

        #if MATCHING_ENABLE_USER_TRACKING
        have_current_ = false;
        if(id != 0){owner_[id] = user;}
        #endif

        return id;
    }

    OrderId newMarket(const std::string& symbol, Side side, Qty qty){
        return newMarket(symbols_.getOrCreate(symbol), UserId{1}, side, qty);
    }

    OrderId newMarket(const std::string& symbol, UserId user, Side side, Qty qty){
        return newMarket(symbols_.getOrCreate(symbol), user, side, qty);
    }

    OrderId newMarket(SymbolId symbol, UserId user, Side side, Qty qty){
        #if MATCHING_ENABLE_USER_TRACKING
        if(!checkRisk(user, symbol, side, qty)){return 0;}
        #endif

        auto& book = getOrCreateBook(symbol);

        #if MATCHING_ENABLE_USER_TRACKING
        current_user_ = user;
        current_side_ = side;
        have_current_ = true;
        #endif

        OrderId id = book.addMarket(side, qty);

        #if MATCHING_ENABLE_USER_TRACKING
        have_current_ = false;
        if(id != 0){owner_[id] = user;}
        #endif

        return id;
    }

    bool cancel(const std::string& symbol, OrderId id){
        auto sid = symbols_.find(symbol);
        if(!sid){return false;}
        return cancel(*sid, id);
    }

    bool cancel(SymbolId symbol, OrderId id){
        if(symbol >= books_.size() || !books_[symbol]){return false;}
        return books_[symbol]->cancel(id);
    }

    OrderId replace(const std::string& symbol, OrderId old_id, Side side, Price price, Qty qty, TimeInForce tif = TimeInForce::GFD){
        return replace(symbols_.getOrCreate(symbol), old_id, side, price, qty, tif);
    }

    OrderId replace(SymbolId symbol, OrderId old_id, Side side, Price price, Qty qty, TimeInForce tif = TimeInForce::GFD){
        cancel(symbol, old_id);
        return newLimit(symbol, UserId{1}, side, price, qty, tif);
    }

    TopOfBook topOfBook(const std::string& symbol) const{
        auto sid = symbols_.find(symbol);
        if(!sid){return TopOfBook{};}
        return topOfBook(*sid);
    }

    TopOfBook topOfBook(SymbolId symbol) const{
        TopOfBook tob{};
        if(symbol >= books_.size() || !books_[symbol]){return tob;}
        const auto& book = *books_[symbol];
        tob.best_bid = book.bestBid();
        tob.bid_size = book.bestBidSize();
        tob.best_ask = book.bestAsk();
        tob.ask_size = book.bestAskSize();
        tob.mid_price = book.midPrice();
        return tob;
    }

    const BookType* findBook(const std::string& symbol) const{
        auto sid = symbols_.find(symbol);
        if(!sid || *sid >= books_.size() || !books_[*sid]){return nullptr;}
        return books_[*sid].get();
    }

    std::optional<BookStats> bookStats(const std::string& symbol) const{
        auto sid = symbols_.find(symbol);
        if(!sid || *sid >= books_.size() || !books_[*sid]){return std::nullopt;}
        return books_[*sid]->stats();
    }

    std::optional<BookStats> bookStats(SymbolId symbol) const{
        if(symbol >= books_.size() || !books_[symbol]){return std::nullopt;}
        return books_[symbol]->stats();
    }

    SymbolIndex& symbolIndex(){return symbols_;}
    const SymbolIndex& symbolIndex() const{return symbols_;}

private:
    TradeCallback callback_;
    SymbolIndex symbols_;
    //O(1) book lookup by SymbolId (index into vector)
    std::vector<std::unique_ptr<BookType>> books_;

    #if MATCHING_ENABLE_USER_TRACKING
    using OwnerMap = boost::unordered_flat_map<OrderId, UserId>;
    OwnerMap owner_;

    using UserPositionsMap = boost::unordered_flat_map<UserId, boost::unordered_flat_map<SymbolId, UserSymbolPosition>>;
    UserPositionsMap user_positions_;

    UserId current_user_{0};
    Side current_side_{Side::Buy};
    bool have_current_{false};
    #endif

    Qty max_abs_position_ = static_cast<Qty>(1'000'000'000);

    BookType& getOrCreateBook(SymbolId symbol){
        if(symbol >= books_.size()){
            books_.resize(symbol + 1);
        }
        if(!books_[symbol]){
            books_[symbol] = std::make_unique<BookType>(
                symbol, symbols_.nameCStr(symbol), InternalCallback{this});
        }
        return *books_[symbol];
    }

    void handleTrade(const Trade& t){
        #if MATCHING_ENABLE_USER_TRACKING
        auto itB = owner_.find(t.buy_id);
        auto itS = owner_.find(t.sell_id);

        if(itB != owner_.end()){
            auto& pos = user_positions_[itB->second][t.symbol_id];
            pos.position += t.qty;
            pos.traded_volume += t.qty;
        }
        else if(have_current_ && current_side_ == Side::Buy && t.buy_id != 0){
            auto& pos = user_positions_[current_user_][t.symbol_id];
            pos.position += t.qty;
            pos.traded_volume += t.qty;
        }

        if(itS != owner_.end()){
            auto& pos = user_positions_[itS->second][t.symbol_id];
            pos.position -= t.qty;
            pos.traded_volume += t.qty;
        }
        else if(have_current_ && current_side_ == Side::Sell && t.sell_id != 0){
            auto& pos = user_positions_[current_user_][t.symbol_id];
            pos.position -= t.qty;
            pos.traded_volume += t.qty;
        }
        #endif

        if(callback_){callback_(t);}
    }

    #if MATCHING_ENABLE_USER_TRACKING
    bool checkRisk(UserId user, SymbolId symbol, Side side, Qty qty) const{
        auto itUser = user_positions_.find(user);
        Qty curr = 0;
        if(itUser != user_positions_.end()){
            auto itSym = itUser->second.find(symbol);
            if(itSym != itUser->second.end()){curr = itSym->second.position;}
        }
        Qty newPos = curr + (side == Side::Buy ? qty: -qty);
        if(std::llabs(newPos) > max_abs_position_){return false;}
        return true;
    }
    #else
    bool checkRisk(UserId, SymbolId, Side, Qty) const{return true;}
    #endif
};
}