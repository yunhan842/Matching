#pragma once

#include <cstdint>
#include <list>
#include <functional>
#include <optional>
#include <string>
#include <limits>
#include <algorithm>
#include <iostream>
#include <boost/container/flat_map.hpp>
#include <boost/pool/pool_alloc.hpp>
#include <boost/unordered/unordered_flat_map.hpp>

namespace matching{

using Price = std::int64_t;
using Qty = std::int64_t;
using OrderId = std::int64_t;
using UserId = std::int64_t;
using SymbolId = std::uint32_t;

enum class Side: std::uint8_t {Buy, Sell};
enum class OrderType: std::uint8_t {Limit, Market};
enum class TimeInForce: std::uint8_t {GFD, IOC, FOK};

struct Order{
    OrderId id;
    Price price;
    Qty qty;
    Side side;
    OrderType type;
    TimeInForce tif;
};

struct Trade{
    SymbolId symbol_id;
    const char* symbol_name; //borrowed pointer, valid for engine lifetime
    Price price;
    Qty qty;
    OrderId buy_id;
    OrderId sell_id;
};

struct BookStats{
    std::uint64_t trade_count{0};
    Qty traded_qty{0};
    Price last_trade_price{0};
    bool has_last_trade{false};
};

template<typename TradeCallback>
class OrderBook{
public:
    explicit OrderBook(SymbolId symbol_id, const char* symbol_name, TradeCallback cb):
        symbol_id_(symbol_id), symbol_name_(symbol_name),
        callback_(std::move(cb)), next_id_(1) {}

    OrderId addLimit(Side side, Price price, Qty qty, TimeInForce tif = TimeInForce::GFD){
        Order o{next_id_++, price, qty, side, OrderType::Limit, tif};
        if(o.tif == TimeInForce::FOK){
            if(!canFullyMatch(o.side, o.price, o.qty)){return o.id;}
        }
        match(o);
        if(o.qty > 0 && o.tif == TimeInForce::GFD){addRestingOrder(o);}
        return o.id;
    }

    OrderId addMarket(Side side, Qty qty){
        Price px = (side == Side::Buy) ? std::numeric_limits<Price>::max(): std::numeric_limits<Price>::min();
        Order o{next_id_++, px, qty, side, OrderType::Market, TimeInForce::IOC};
        match(o);
        return o.id;
    }

    bool cancel(OrderId id){
        auto it = index_.find(id);
        if(it == index_.end()){return false;}

        const OrderLocator loc = it->second;
        if(loc.side == Side::Buy){
            auto lvlIt = bids_.find(loc.price);
            if(lvlIt == bids_.end()){index_.erase(it); return false;}
            PriceLevel& lvl = lvlIt->second;
            if(loc.it->qty > 0){lvl.total_qty -= loc.it->qty;}
            lvl.orders.erase(loc.it);
            index_.erase(it);
            if(lvl.orders.empty()){bids_.erase(lvlIt);}
        } else {
            auto lvlIt = asks_.find(loc.price);
            if(lvlIt == asks_.end()){index_.erase(it); return false;}
            PriceLevel& lvl = lvlIt->second;
            if(loc.it->qty > 0){lvl.total_qty -= loc.it->qty;}
            lvl.orders.erase(loc.it);
            index_.erase(it);
            if(lvl.orders.empty()){asks_.erase(lvlIt);}
        }
        return true;
    }

    std::optional<Price> bestBid() const{
        if(bids_.empty()){return std::nullopt;}
        return bids_.rbegin()->first;
    }

    //asks stored descending (greater<>), so rbegin = lowest = best ask
    std::optional<Price> bestAsk() const{
        if(asks_.empty()){return std::nullopt;}
        return asks_.rbegin()->first;
    }

    std::optional<Qty> bestBidSize() const{
        if(bids_.empty()){return std::nullopt;}
        return bids_.rbegin()->second.total_qty;
    }

    std::optional<Qty> bestAskSize() const{
        if(asks_.empty()){return std::nullopt;}
        return asks_.rbegin()->second.total_qty;
    }

    std::optional<Price> midPrice() const{
        auto bb = bestBid();
        auto ba = bestAsk();
        if(!bb || !ba){return std::nullopt;}
        return (*bb + *ba) / 2;
    }

    SymbolId symbolId() const {return symbol_id_;}
    const char* symbolName() const {return symbol_name_;}

    void printBook(std::ostream& os, int depth) const {
        os << "OrderBook(" << symbol_name_ << ")\n";

        //asks: best first = lowest price. rbegin = lowest with greater<>
        os << "\tAsks:\n";
        int shown = 0;
        for(auto it = asks_.rbegin(); it != asks_.rend() && shown < depth; ++it, ++shown){
            os << "\t\tpx=" << it->first << " total_qty=" << it->second.total_qty
               << " (orders=" << it->second.orders.size() << ")\n";
        }
        if(shown == 0){os << "\t\t<empty>\n";}

        //bids: best first = highest price. rbegin = highest with less<>
        os << "\tBids:\n";
        shown = 0;
        for(auto it = bids_.rbegin(); it != bids_.rend() && shown < depth; ++it, ++shown){
            os << "\t\tpx=" << it->first << " total_qty=" << it->second.total_qty
               << " (orders=" << it->second.orders.size() << ")\n";
        }
        if(shown == 0){os << "\t\t<empty>\n";}
    }

    const BookStats& stats() const{return stats_;}

    void reserveIndex(std::size_t n){index_.reserve(n);}

private:
    //pool allocator with null_mutex: no mutex overhead for single-threaded use
    using OrderList = std::list<Order,
        boost::fast_pool_allocator<Order,
            boost::default_user_allocator_new_delete,
            boost::details::pool::null_mutex>>;

    struct PriceLevel{
        Qty total_qty{0};
        OrderList orders;
    };

    //bids: ascending (default less<>), best bid = rbegin (highest)
    using BidSide = boost::container::flat_map<Price, PriceLevel>;
    //asks: descending (greater<>), best ask = rbegin (lowest) → O(1) erase of best
    using AskSide = boost::container::flat_map<Price, PriceLevel, std::greater<Price>>;

    struct OrderLocator{
        Side side;
        Price price;
        OrderList::iterator it;
    };

    using OrderIndex = boost::unordered_flat_map<OrderId, OrderLocator>;

    SymbolId symbol_id_;
    const char* symbol_name_; //borrowed, valid for engine lifetime
    TradeCallback callback_;
    OrderId next_id_;

    BidSide bids_;
    AskSide asks_;
    OrderIndex index_;
    BookStats stats_;

    void emitTrade(Price price, Qty qty, OrderId buy_id, OrderId sell_id){
        ++stats_.trade_count;
        stats_.traded_qty += qty;
        stats_.last_trade_price = price;
        stats_.has_last_trade = true;
        callback_(Trade{symbol_id_, symbol_name_, price, qty, buy_id, sell_id});
    }

    void match(Order& incoming){
        if(incoming.side == Side::Buy){matchBuy(incoming);}
        else{matchSell(incoming);}
    }

    //match incoming buy against resting asks
    //asks stored descending, so best ask (lowest) = prev(end) → O(1) erase
    void matchBuy(Order& buy){
        while(buy.qty > 0 && !asks_.empty()){
            auto bestAskIt = std::prev(asks_.end()); //lowest price
            Price bestAskPx = bestAskIt->first;
            if(buy.type == OrderType::Limit && buy.price < bestAskPx){break;}

            PriceLevel& lvl = bestAskIt->second;
            auto it = lvl.orders.begin();
            while(it != lvl.orders.end() && buy.qty > 0){
                Order& sell = *it;
                Qty traded = std::min(buy.qty, sell.qty);
                buy.qty -= traded;
                sell.qty -= traded;
                lvl.total_qty -= traded;

                emitTrade(bestAskPx, traded, buy.id, sell.id);

                if(sell.qty == 0){
                    index_.erase(sell.id);
                    it = lvl.orders.erase(it);
                }
                else{++it;}
            }
            if(lvl.orders.empty()){asks_.erase(bestAskIt);} //O(1) erase at end
        }
    }

    //match incoming sell against resting bids
    //bids stored ascending, so best bid (highest) = prev(end) → O(1) erase
    void matchSell(Order& sell){
        while(sell.qty > 0 && !bids_.empty()){
            auto bestBidIt = std::prev(bids_.end()); //highest price
            Price bestBidPx = bestBidIt->first;
            if(sell.type == OrderType::Limit && sell.price > bestBidPx){break;}

            PriceLevel& lvl = bestBidIt->second;
            auto it = lvl.orders.begin();
            while(it != lvl.orders.end() && sell.qty > 0){
                Order& buy = *it;
                Qty traded = std::min(sell.qty, buy.qty);
                sell.qty -= traded;
                buy.qty -= traded;
                lvl.total_qty -= traded;

                emitTrade(bestBidPx, traded, buy.id, sell.id);

                if(buy.qty == 0){
                    index_.erase(buy.id);
                    it = lvl.orders.erase(it);
                }
                else{++it;}
            }
            if(lvl.orders.empty()){bids_.erase(bestBidIt);} //O(1) erase at end
        }
    }

    void addRestingOrder(const Order& o){
        if(o.side == Side::Buy){
            PriceLevel& lvl = bids_[o.price];
            lvl.orders.push_back(o);
            lvl.total_qty += o.qty;
            index_[o.id] = OrderLocator{o.side, o.price, std::prev(lvl.orders.end())};
        } else {
            PriceLevel& lvl = asks_[o.price];
            lvl.orders.push_back(o);
            lvl.total_qty += o.qty;
            index_[o.id] = OrderLocator{o.side, o.price, std::prev(lvl.orders.end())};
        }
    }

    bool canFullyMatch(Side side, Price price, Qty qty) const{
        if(qty <= 0){return true;}
        Qty need = qty;
        if(side == Side::Buy){
            //iterate asks from best (lowest) upward; rbegin = lowest with greater<>
            for(auto it = asks_.rbegin(); it != asks_.rend(); ++it){
                if(it->first > price){break;}
                need -= it->second.total_qty;
                if(need <= 0){return true;}
            }
        }
        else{
            //iterate bids from best (highest) downward; rbegin = highest with less<>
            for(auto it = bids_.rbegin(); it != bids_.rend(); ++it){
                if(it->first < price){break;}
                need -= it->second.total_qty;
                if(need <= 0){return true;}
            }
        }
        return false;
    }
};
}